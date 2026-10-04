/**
 * @file    bl_port.hpp
 * @brief   Bootloader 移植接口（唯一契约层，C++ 版）
 *
 * 移植到新芯片时，只需实现本文件声明的全部函数，
 * core/ 与 app/ 下的代码不需要任何修改。
 *
 * 实现参考：target/stm32f4/
 * 移植模板：port/bl_port_template.cpp
 *
 * 约定：
 *   - 实现文件用 .cpp，可直接调用各自平台的 C 语言 HAL/标准库
 *   - 所有函数不得阻塞超过给定超时；长耗时操作（擦除）内部需喂狗
 *   - 不使用异常与动态内存
 */
#ifndef BL_PORT_HPP
#define BL_PORT_HPP

#include "bl_types.hpp"

namespace bl {

/* ========================================================================
 * Flash 驱动
 *
 * core 层只按「字节地址 + 字节长度」请求操作；
 * 扇区对齐、编程粒度、解锁等芯片特性全部由移植层内部处理。
 * ======================================================================*/

/// 初始化 Flash 访问（解锁、使能时钟等）
Status flash_init() noexcept;

/// 查询包含指定地址的扇区（页）大小；返回 0 表示地址非法
uint32_t flash_sector_size(uint32_t addr) noexcept;

/// 查询从 addr 起、到下一扇区边界为止的连续字节数
uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept;

/// 擦除区域；core 层保证 addr 与 len 均已按扇区对齐
Status flash_erase(uint32_t addr, uint32_t len) noexcept;

/**
 * @brief 编程写入
 *
 * 移植层需处理地址对齐、长度补齐（补 0xFF）、编程粒度。
 * 若 addr/len 不满足硬件对齐要求，由移植层内部
 * 「读-改-写」完成，core 层不感知。
 */
Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept;

/// 读取
Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept;

/* ========================================================================
 * UART 驱动
 * ======================================================================*/

/// 初始化串口
Status uart_init(uint32_t baudrate) noexcept;

/**
 * @brief 运行时修改波特率
 *
 * 当前 IAP 阶段固定 115200，本接口为预留能力：
 * 将来若实现「握手 115200、传输切 921600」的提速方案，
 * 由会话层在双方约定时机调用。
 *
 * 实现要求：切换过程中不得丢失已接收数据；切换后清空收发 FIFO。
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
 * 系统控制
 * ======================================================================*/

/**
 * @brief 从 Bootloader 跳转到 APP
 *
 * 实现须包含 Cortex-M 的跳转前准备（顺序不可乱）：
 *   1. 关全局中断
 *   2. 停 SysTick 并清计数
 *   3. 复位 RCC 到默认态（HAL_RCC_DeInit）
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
 * 之所以不直接在 app 层调用平台的 HAL_Delay：app/core 层必须保持
 * 平台无关，否则「换芯片只改 target」这条约定就破了。
 */
void delay_ms(uint32_t ms) noexcept;

/// 触发系统复位（升级完成后重启）
[[noreturn]] void system_reset() noexcept;

} // namespace bl

#endif /* BL_PORT_HPP */
