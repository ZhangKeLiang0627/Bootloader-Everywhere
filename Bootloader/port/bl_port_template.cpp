/**
 * @file    bl_port_template.cpp
 * @brief   移植模板 —— 换到新芯片时照此填空
 *
 * ⚠ 本文件是「移植参考」，请勿加入编译（会与 target/<chip>/ 中的
 *    实现产生重复符号）。正确做法是复制本文件到 target/<芯片>/ 下，
 *    改名为 bl_port_<芯片>.cpp 后填实现。
 *
 * 移植只需三步：
 *   1. 复制本文件到 target/<芯片>/，实现下面全部函数
 *   2. 修改 config/bl_config.h 里的三个分区地址与 SRAM 范围
 *      （必须与目标芯片的扇区/页边界对齐）
 *   3. 把 target/<芯片>/*.cpp 与 core/*.cpp、app/bl_main.cpp 加入编译
 *
 * core/ 与 app/ 下的代码一行都不需要改 —— 这是分层的目的。
 */
#include "bl_port.hpp"
#include "bl_config.h"

/* 换成目标平台的头文件 */
// #include "gd32f30x.h"
// #include "ch32v30x.h"

namespace bl {

/* ========================================================================
 * Flash
 * ======================================================================*/

Status flash_init() noexcept
{
    /* TODO: 解锁 Flash、使能接口时钟 */
    return Status::Ok;
}

uint32_t flash_sector_size(uint32_t addr) noexcept
{
    /* TODO: 返回包含 addr 的扇区（页）大小
     *
     * 必须支持「不等长扇区」这种情形：
     *   STM32F4   S0-S3=16KB, S4=64KB, S5+ =128KB
     *   GD32F30x  均匀 2KB/页
     *   CH32V307  均匀 4KB/页（注意 RISC-V 内核，跳转流程也不同）
     * 返回 0 表示地址非法。 */
    (void)addr;
    return 0;
}

uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept
{
    /* TODO: 从 addr 到下一个扇区边界的字节数 */
    (void)addr;
    return 0;
}

Status flash_erase(uint32_t addr, uint32_t len) noexcept
{
    /* TODO: 擦除。core 层保证 addr 落在扇区起始、len 为扇区大小之和。
     *
     * 务必加一道「拒绝擦除 Bootloader 自身区域」的保护
     * （addr < BL_BOOT_BASE + BL_BOOT_SIZE 时返回 bad param），
     * 这是防变砖的最后一道防线。 */
    (void)addr; (void)len;
    return Status::Error;
}

Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept
{
    /* TODO: 编程写入。
     *
     * 处理三件事：
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
 * UART
 * ======================================================================*/

Status uart_init(uint32_t baudrate) noexcept
{
    /* TODO: 初始化串口为 8N1，波特率用传入值 */
    (void)baudrate;
    return Status::Error;
}

Status uart_set_baudrate(uint32_t baudrate) noexcept
{
    /* TODO: 运行时改波特率（为提速方案预留）。
     *
     * 要点：改之前等发送移位寄存器空；改之后清空收发 FIFO；
     *       注意 PC 端必须同步切换，否则链路立刻失步。 */
    (void)baudrate;
    return Status::Error;
}

Status uart_read(uint8_t* buf, uint32_t len,
                 uint32_t timeout_ms, uint32_t* out_read) noexcept
{
    /* TODO: 阻塞读，带「总超时」语义（而非单字节超时）
     * timeout_ms 为 0 表示只试一次不等待 */
    (void)buf; (void)len; (void)timeout_ms; (void)out_read;
    return Status::Timeout;
}

bool uart_try_getc(uint8_t* ch) noexcept
{
    /* TODO: 非阻塞探测单字节：有数据则取走并返回 true。
     * Backdoor 窗口会以高频率轮询本函数，实现要尽量轻量。 */
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

void log_printf(const char* fmt, ...) noexcept
{
    /* TODO: 格式化并输出。
     * 建议用 vsnprintf 自己组缓冲，不要链接完整 printf——
     * 带浮点的 printf 在 -O2 下可能一次吃掉 3-4KB ROM，
     * 而 Bootloader 总共只有 16KB。 */
    (void)fmt;
}

/* ========================================================================
 * 系统
 * ======================================================================*/

void jump_to_app(uint32_t app_base) noexcept
{
    /* TODO: Cortex-M 跳转八步（顺序不可乱）：
     *   1. __disable_irq()
     *   2. SysTick->CTRL/LOAD/VAL 清零
     *   3. 复位 RCC 到默认态（HAL_RCC_DeInit 或等价操作）
     *   4. 清所有 NVIC 的 ICER / ICPR
     *   5. SCB->VTOR = app_base
     *   6. __set_MSP(*(uint32_t*)app_base)
     *   7. __set_CONTROL(0)
     *   8. __enable_irq() 后跳 *(uint32_t*)(app_base + 4)
     *
     * 注意：RISC-V 内核（如 CH32V307）的向量表寄存器与跳转方式不同，
     *       需要查对应手册，不能照搬 Cortex-M 的写法。 */
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

void system_reset() noexcept
{
    /* TODO: 触发系统复位 */
}

} // namespace bl
