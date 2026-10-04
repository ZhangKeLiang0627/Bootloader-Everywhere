/**
 * @file    bl_port_template.cpp
 * @brief   移植模板 —— 换到新芯片时复制到 target/<平台>/ 逐项填空
 *
 * 本文件勿直接加入编译（会与 target/ 实现重复符号）。
 * core/ 与 app/ 一行都不用改 —— 这就是分层的目的。
 */
#include "bl_config.h"          /* 必须最先：芯片参数 */
#include "port/bl_port.hpp"

/* 换成目标平台的器件头文件 */
// #include "gd32f30x.h"
// #include "ch32v30x.h"

namespace bl {

/* ---- 平台初始化 ---- */

Status platform_init() noexcept
{
    /* TODO: 1) HAL 底座  2) 系统时钟（HSE 失败退回内部 RC）  3) 1ms 时基 */
    return Status::Error;
}

/* ---- Flash ---- */

Status flash_init() noexcept
{
    /* TODO: 解锁 Flash、使能接口时钟 */
    return Status::Ok;
}

uint32_t flash_sector_size(uint32_t addr) noexcept
{
    /* TODO: 含 addr 的扇区（页）大小，0=非法。注意不等长扇区（F4）或等长页（GD32） */
    (void)addr;
    return 0;
}

uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept
{
    /* TODO: addr 到下一扇区边界的字节数 */
    (void)addr;
    return 0;
}

Status flash_erase(uint32_t addr, uint32_t len) noexcept
{
    /* TODO: 擦除（addr/len 已对齐）。务必拒绝擦除 Bootloader 自身区域 */
    (void)addr; (void)len;
    return Status::Error;
}

Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept
{
    /* TODO: 编程写入。处理对齐、读-改-写补齐 */
    (void)addr; (void)data; (void)len;
    return Status::Error;
}

Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept
{
    /* TODO: 读。多数 Cortex-M 内存映射，直接 memcpy 即可 */
    (void)addr; (void)buf; (void)len;
    return Status::Error;
}

/* ---- UART ---- */

Status uart_init(uint32_t baudrate) noexcept
{
    /* TODO: 初始化 8N1，含引脚复用 */
    (void)baudrate;
    return Status::Error;
}

Status uart_read(uint8_t* buf, uint32_t len,
                 uint32_t timeout_ms, uint32_t* out_read) noexcept
{
    /* TODO: 阻塞读，语义是「总超时」而非「每字节超时」 */
    (void)buf; (void)len; (void)timeout_ms; (void)out_read;
    return Status::Timeout;
}

bool uart_try_getc(uint8_t* ch) noexcept
{
    /* TODO: 非阻塞探测单字节 */
    (void)ch;
    return false;
}

Status uart_write(const uint8_t* buf, uint32_t len) noexcept
{
    /* TODO: 阻塞发送 */
    (void)buf; (void)len;
    return Status::Error;
}

void uart_flush_rx() noexcept
{
    /* TODO: 清空接收缓冲与错误标志 */
}

/* ---- 系统 ---- */

void jump_to_app(uint32_t app_base) noexcept
{
    /* TODO: Cortex-M 跳转八步：关中断 → 停 SysTick → 复位 RCC → 清 NVIC
     * → 设 VTOR → 设 MSP → 设 CONTROL → 跳入口。
     * RISC-V 内核（CH32V307）向量表寄存器与跳转方式不同，查手册 */
    (void)app_base;
}

uint32_t tick_ms() noexcept
{
    /* TODO: 上电起毫秒数 */
    return 0;
}

void delay_ms(uint32_t ms) noexcept
{
    (void)ms;
}

ResetCause reset_cause() noexcept
{
    /* TODO: 读并清除复位原因（标志是累积的，须读后即清）。
     * 软件复位须优先于引脚复位（探针会连带拉 NRST）。
     * 无复位原因寄存器则返回 Unknown */
    return ResetCause::Unknown;
}

} // namespace bl
