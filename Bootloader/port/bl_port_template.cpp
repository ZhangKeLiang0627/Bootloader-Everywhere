/**
 * @file    bl_port_template.cpp
 * @brief   移植模板 —— 换到新芯片时照此填空
 *
 * ⚠ 本文件是「移植参考」，请勿直接加入编译：它和 target/<平台>/ 里的实现
 *    是同一组函数，一起编译会产生重复符号。
 *    正确做法是复制到 target/<新平台>/ 下，改名为
 *    bl_port_<平台>.cpp，然后逐项填实现。
 *
 * 移植三步（详见 docs/PORTING.md）：
 *   1. 复制本文件到 target/<新平台>/，实现下面全部 19 个函数
 *   2. 改 bl_config.h 第一节：Flash 容量 / SRAM 范围 / 时钟
 *      （分区地址由容量派生，但要确认落点正好在扇区/页边界上）
 *   3. 把 core/*.cpp、app/bl_entry.cpp、target/<新平台>/*.cpp 加入编译，
 *      include 路径加上 Bootloader/ 根目录
 *
 * core/ 与 app/ 下的代码一行都不用改 —— 这就是分层的目的。
 * 若新平台与 STM32F4 同族（F401/F405/F407/F411/F415/F417），
 * 直接复用 target/stm32f4/ 即可，不需要写本文件。
 */
#include "bl_config.h"          /* 必须最先：芯片参数 */

#include "port/bl_port.hpp"

/* 换成目标平台的器件头文件 */
// #include "gd32f30x.h"
// #include "ch32v30x.h"

namespace bl {

/* ========================================================================
 * 一、平台初始化
 * ======================================================================*/

Status platform_init() noexcept
{
    /* TODO: 建立运行环境。三件事：
     *   1. 驱动库底座（HAL_Init 或等价的系统初始化）
     *   2. 系统时钟：按 bl_config.h 的 BL_HSE_HZ / BL_SYSCLK_HZ 配置 PLL
     *      —— HSE 起振失败时应退回内部 RC，宁可频率低也要能通信
     *   3. 1ms 时基（tick_ms / delay_ms 依赖它）
     *
     * 另外建议在这儿登记 SysTick 与 HardFault 的处理函数：
     * 库不依赖宿主工程的 it.c，异常兜底要自己提供一份。 */
    return Status::Error;
}

/* ========================================================================
 * 二、Flash
 * ======================================================================*/

Status flash_init() noexcept
{
    /* TODO: 解锁 Flash、使能接口时钟 */
    return Status::Ok;
}

uint32_t flash_sector_size(uint32_t addr) noexcept
{
    /* TODO: 返回包含 addr 的扇区（页）大小，返回 0 表示地址非法。
     *
     * 必须能处理「不等长扇区」：
     *   STM32F4   S0-S3=16KB / S4=64KB / S5+ =128KB（见 target/stm32f4）
     *   GD32F30x  均匀 2KB/页（≤512KB 型号）
     *   CH32V307  均匀 4KB/页；注意 RISC-V 内核，跳转流程也完全不同 */
    (void)addr;
    return 0;
}

uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept
{
    /* TODO: 从 addr 到下一个扇区边界的连续字节数。
     * 上位者用它验证「分区基址是否正好落在扇区起点」。 */
    (void)addr;
    return 0;
}

Status flash_erase(uint32_t addr, uint32_t len) noexcept
{
    /* TODO: 擦除。core 层保证 addr 落在扇区起始、len 为整数个扇区。
     *
     * 务必加一道「拒绝擦除 Bootloader 自身区域」的保护
     * （addr < BL_BOOT_BASE + BL_BOOT_SIZE 时返回 BadParam），
     * 这是防变砖的最后一道防线。 */
    (void)addr; (void)len;
    return Status::Error;
}

Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept
{
    /* TODO: 编程写入。处理三件事：
     *   1. 地址按编程单位对齐（F4 是 32 位字，GD32 多为半字，CH32 视型号）
     *   2. 长度不足一个编程单位时做「读-改-写」，未覆盖的字节保持原值
     *   3. 长循环里喂狗（擦大扇区可达数秒） */
    (void)addr; (void)data; (void)len;
    return Status::Error;
}

Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept
{
    /* TODO: 读。多数 Cortex-M 芯片 Flash 内存映射，直接 memcpy 即可 */
    (void)addr; (void)buf; (void)len;
    return Status::Error;
}

/* ========================================================================
 * 三、UART
 * ======================================================================*/

Status uart_init(uint32_t baudrate) noexcept
{
    /* TODO: 初始化串口为 8N1，波特率用传入值。
     * 别忘了引脚复用配置 —— 库不依赖宿主工程的 usart.c / MspInit。 */
    (void)baudrate;
    return Status::Error;
}

Status uart_set_baudrate(uint32_t baudrate) noexcept
{
    /* TODO: 运行时改波特率（为提速方案预留）。
     *
     * 要点：改之前等发送移位寄存器空；改之后清空收发 FIFO；
     *       PC 端必须同步切换，否则链路立刻失步。 */
    (void)baudrate;
    return Status::Error;
}

Status uart_read(uint8_t* buf, uint32_t len,
                 uint32_t timeout_ms, uint32_t* out_read) noexcept
{
    /* TODO: 阻塞读，语义是「总超时」而不是「每字节超时」。
     * timeout_ms 为 0 表示只试一次、不等待。 */
    (void)buf; (void)len; (void)timeout_ms; (void)out_read;
    return Status::Timeout;
}

bool uart_try_getc(uint8_t* ch) noexcept
{
    /* TODO: 非阻塞探测单字节：有数据则取走并返回 true。
     * Backdoor 窗口会高频轮询本函数，实现要尽量轻量。 */
    (void)ch;
    return false;
}

Status uart_write(const uint8_t* buf, uint32_t len) noexcept
{
    /* TODO: 阻塞式发送完所有字节 */
    (void)buf; (void)len;
    return Status::Error;
}

void uart_flush_rx() noexcept
{
    /* TODO: 清空接收缓冲与各类错误标志（ORE/FE/NE/PE） */
}

/* ========================================================================
 * 四、系统
 * ======================================================================*/

void jump_to_app(uint32_t app_base) noexcept
{
    /* TODO: Cortex-M 跳转八步（顺序不可乱）：
     *   1. __disable_irq()
     *   2. SysTick->CTRL / LOAD / VAL 清零
     *   3. 复位 RCC 到默认态
     *   4. 清所有 NVIC 的 ICER / ICPR
     *   5. SCB->VTOR = app_base
     *   6. __set_MSP(*(uint32_t*)app_base)
     *   7. __set_CONTROL(0)
     *   8. __enable_irq() 后跳 *(uint32_t*)(app_base + 4)
     *
     * 注意：RISC-V 内核（如 CH32V307）的向量表寄存器与跳转方式不同，
     *       要查对应手册，不能照搬 Cortex-M 的写法。 */
    (void)app_base;
}

void wdg_init() noexcept
{
    /* TODO: 初始化看门狗。
     * 提醒：IWDG 一旦启动无法停止，启用后 APP 也必须喂狗。 */
}

void wdg_feed() noexcept
{
    /* TODO: 喂狗。只在 BL_USE_WATCHDOG 打开时才需要真正执行 */
}

uint32_t tick_ms() noexcept
{
    /* TODO: 返回上电以来的毫秒数 */
    return 0;
}

void delay_ms(uint32_t ms) noexcept
{
    /* TODO: 毫秒级延时。长延时请分片并喂狗，
     * 否则一次 delay(3000) 会把 2 秒超时的看门狗拖死。 */
    (void)ms;
}

void system_reset() noexcept
{
    /* TODO: 触发系统复位。复位前建议等串口发完，否则调试时总丢最后半行日志 */
}

} // namespace bl
