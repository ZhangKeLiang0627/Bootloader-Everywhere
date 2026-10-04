/**
 * @file    bl_port.hpp
 * @brief   LUMOS-bootloader 移植契约 —— 库与芯片之间唯一的耦合面
 *
 * ============================================================================
 * 这是整个库的分界线，请先读这段再动手移植：
 *
 *   core/   平台无关逻辑（状态机 / YMODEM / Flash 抽象 / 启动决策）
 *           —— 移植任何芯片都不需要改动其中任何一行
 *   port/   本文件，声明 19 个函数，是 core 唯一能看到的外部世界
 *   target/ 具体芯片的实现。已带 STM32F4 全系适配；换芯片就新增一个
 *           target/<平台>/ 目录，把本文件的函数实现掉
 *
 * 移植步骤（细节见 docs/PORTING.md）：
 *   1. 把整个 Bootloader/ 目录拷进目标工程
 *   2. 改 bl_config.h 第一节（选预置或手填 Flash/SRAM/时钟）
 *   3. 若目标芯片属于已有平台，直接用对应 target/；否则
 *      复制 port/bl_port_template.cpp 逐项填实现
 *   4. 在工程里加编译、把启动向量表偏移设为 BL_APP_BASE
 *
 * 实现约定：
 *   - 用 .cpp 实现，可以自由调用各平台的 HAL / 固件库
 *   - 不得阻塞超过给定超时；长耗时操作（擦除）内部要定期喂狗
 *   - 不使用异常、动态内存、RTTI（编译器已关闭）
 *   - 所有 Flash 访问都按「字节地址 + 字节长度」进行，
 *     扇区/页对齐、编程粒度、解锁等芯片特性一律在本层内部消化，
 *     core 层永远不需要知道这些
 * ============================================================================
 */
#ifndef BL_PORT_HPP
#define BL_PORT_HPP

#include "core/bl_types.hpp"

namespace bl {

/* ========================================================================
 * 一、平台初始化
 * ======================================================================*/

/**
 * @brief 建立运行环境：HAL/固件库底座、系统时钟、时基、调试串口的引脚
 *
 * 之所以由库自己负责而不是依赖宿主工程：Bootloader 是一份完整固件，
 * 不是宿主程序的一部分。把时钟/时基收敛到这里，整包才能原样搬到别的
 * 工程 —— 否则搬运时还得读一遍对方的 main.c，看它怎么配的时钟。
 *
 * 实现要求：
 *   - 幂等：重复调用不出错（宿主可能已经初始化过）
 *   - 必须建立 1ms 时基，因为 tick_ms()/delay_ms() 依赖它
 *   - HSE 起振失败时应退回内部 RC：宁可频率低也要能通信 ——
 *     串口波特率算错的表现是「全是乱码」，比直接跑不起来更难查
 */
Status platform_init() noexcept;

/* ========================================================================
 * 二、Flash 驱动
 * ======================================================================*/

/// 初始化 Flash 访问（解锁、使能时钟等）
Status flash_init() noexcept;

/// 查询包含指定地址的扇区（页）大小；返回 0 表示地址非法
uint32_t flash_sector_size(uint32_t addr) noexcept;

/// 查询从 addr 起、到下一扇区边界为止的连续字节数
uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept;

/// 擦除区域；core 保证 addr 与 len 均已按扇区对齐
Status flash_erase(uint32_t addr, uint32_t len) noexcept;

/**
 * @brief 编程写入
 *
 * 移植层需处理地址对齐、长度补齐（补 0xFF）、编程粒度。
 * 若 addr/len 不满足硬件对齐要求，由移植层内部「读-改-写」完成。
 */
Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept;

/// 读取
Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept;

/* ========================================================================
 * 三、UART 驱动
 * ======================================================================*/

/// 初始化串口（含引脚复用配置）
Status uart_init(uint32_t baudrate) noexcept;

/**
 * @brief 运行时修改波特率
 *
 * 当前 IAP 阶段固定 115200，本接口为预留能力：
 * 将来若实现「握手 115200、传输切 921600」的提速方案，
 * 由会话层在双方约定的时机调用。
 *
 * 实现要求：切换过程中不得丢失已收数据；切换后清空收发 FIFO。
 * 注意 PC 端必须同步切换，否则链路立即失步。
 */
Status uart_set_baudrate(uint32_t baudrate) noexcept;

/**
 * @brief 读取数据（阻塞至读满或超时）
 * @param out_read 实际读到的字节数，可为 nullptr
 */
Status uart_read(uint8_t* buf, uint32_t len,
                 uint32_t timeout_ms, uint32_t* out_read) noexcept;

/// 探测单个字节（不等待）
bool uart_try_getc(uint8_t* ch) noexcept;

/// 阻塞式写入全部数据
Status uart_write(const uint8_t* buf, uint32_t len) noexcept;

/// 清空接收缓冲（进入 IAP 前调用，丢弃遗留数据）
void uart_flush_rx() noexcept;

/* ========================================================================
 * 四、系统控制
 * ======================================================================*/

/**
 * @brief 从 Bootloader 跳转到 APP
 *
 * Cortex-M 的跳转前准备，八步缺一不可、顺序也不能乱：
 *   1. 关全局中断
 *   2. 停 SysTick 并清计数
 *   3. 复位 RCC 到默认态（APP 的 SystemInit 会重建自己的时钟）
 *   4. 清所有 NVIC 中断使能与挂起标志
 *   5. 重定位向量表 SCB->VTOR = app_base
 *   6. __set_MSP(*(uint32_t*)app_base)
 *   7. __set_CONTROL(0)（APP 使用 MSP 特权级时必需）
 *   8. 取 *(uint32_t*)(app_base + 4) 作为入口调用
 *
 * 正常情况不返回；返回即表示跳转失败。
 */
void jump_to_app(uint32_t app_base) noexcept;

/// 初始化看门狗
void wdg_init() noexcept;

/// 喂狗
void wdg_feed() noexcept;

/// 获取系统毫秒时基（上电起累计）
uint32_t tick_ms() noexcept;

/**
 * @brief 毫秒级延时
 *
 * 不在 app/core 层直接用平台的 HAL_Delay：那会让「换芯片只改 target」
 * 这条约定失效。长延时需分片并喂狗。
 */
void delay_ms(uint32_t ms) noexcept;

/// 触发系统复位（升级完成后重启）
[[noreturn]] void system_reset() noexcept;

} // namespace bl

#endif /* BL_PORT_HPP */
