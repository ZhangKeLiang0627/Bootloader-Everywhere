/**
 * @file    bl_port_uart_stm32f4.cpp
 * @brief   STM32F4 串口驱动（USART1 / PA9 / PA10）
 *
 * 采用「阻塞 + 总超时」的简单实现：IAP 阶段的数据量不大
 * （115200 下 1MB 约 100 秒），HAL 轮询的开销相对传输时间可忽略，
 * 换来的是行为可预测、无中断竞态。
 * 若将来需要提速到 921600，可改为「中断 + 环形缓冲」或 DMA。
 */
#include <cstdarg>
#include <cstdio>

#include "stm32f4xx_hal.h"
#include "usart.h"
#include "bl_port.hpp"
#include "bl_config.h"

namespace bl {

/* ========================================================================
 * 初始化
 * ======================================================================*/
Status uart_init(uint32_t baudrate) noexcept
{
    huart1.Init.BaudRate = baudrate;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        return Status::Error;
    }
    uart_flush_rx();
    return Status::Ok;
}

/* ========================================================================
 * 运行时改波特率（为将来的提速方案预留）
 *
 * 注意：改完之后必须双方同步，否则链路立刻失步。
 * 本端改完会清空收发缓冲，避免残留数据被误当成新协议内容。
 * ======================================================================*/
Status uart_set_baudrate(uint32_t baudrate) noexcept
{
    if (baudrate == 0U) {
        return Status::BadParam;
    }

    /* 等发送移位寄存器彻底空掉，再改参数 */
    while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TC) == RESET) {
        wdg_feed();
    }

    huart1.Init.BaudRate = baudrate;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        return Status::Error;
    }

    uart_flush_rx();
    return Status::Ok;
}

/* ========================================================================
 * 读取：逐字节收，带总超时
 * ======================================================================*/
Status uart_read(uint8_t* buf, uint32_t len,
                 uint32_t timeout_ms, uint32_t* out_read) noexcept
{
    uint32_t got = 0;

    if (out_read != nullptr) {
        *out_read = 0;
    }
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    const uint32_t start = HAL_GetTick();

    while (got < len) {
        uint32_t slice = timeout_ms;

        if (timeout_ms != 0U) {
            const uint32_t elapsed = HAL_GetTick() - start;
            if (elapsed >= timeout_ms) {
                break;                      /* 总超时 */
            }
            slice = timeout_ms - elapsed;
        } else {
            slice = 1U;                     /* 0 表示只试一次 */
        }

        uint8_t ch = 0;
        if (HAL_UART_Receive(&huart1, &ch, 1, slice) == HAL_OK) {
            buf[got++] = ch;
        } else if (timeout_ms == 0U) {
            break;
        } else if ((HAL_GetTick() - start) >= timeout_ms) {
            break;
        }
    }

    if (out_read != nullptr) {
        *out_read = got;
    }
    return (got == len) ? Status::Ok : Status::Timeout;
}

/* ========================================================================
 * 非阻塞探测单字节
 *
 * 直接读数据寄存器：这样是真正的「不等待」。
 * HAL_UART_Receive 即便超时传 0 也会走一轮状态机，在 backdoor
 * 轮询这种高频场景下不够轻量。
 * ======================================================================*/
bool uart_try_getc(uint8_t* ch) noexcept
{
    if (ch == nullptr) {
        return false;
    }
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) == RESET) {
        return false;
    }
    *ch = static_cast<uint8_t>(huart1.Instance->DR & 0xFFU);
    return true;
}

/* ========================================================================
 * 写入
 * ======================================================================*/
Status uart_write(const uint8_t* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    const uint32_t slice = 1000U + (len / 10U);   /* 按长度给足余量 */

    if (HAL_UART_Transmit(&huart1, const_cast<uint8_t*>(buf), len, slice) != HAL_OK) {
        return Status::Timeout;
    }
    return Status::Ok;
}

/* ========================================================================
 * 清空接收缓冲
 * ======================================================================*/
void uart_flush_rx() noexcept
{
    /* 清溢出标志，否则后续接收会一直被阻塞 */
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_FEFLAG(&huart1);
    __HAL_UART_CLEAR_NEFLAG(&huart1);
    __HAL_UART_CLEAR_PEFLAG(&huart1);

    uint32_t guard = 0;
    while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET && guard++ < 4096U) {
        (void)huart1.Instance->DR;
    }
}

/* ========================================================================
 * 调试输出
 *
 * 用 vsnprintf 而非 printf：不引入完整 stdio 的重型依赖，
 * 在 -O2 下通常只占几百字节，远小于链接 printf 的代价。
 * ======================================================================*/
void log_printf(const char* fmt, ...) noexcept
{
#if BL_DEBUG_LOG
    char line[160];

    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n > 0) {
        const uint32_t len = (static_cast<uint32_t>(n) < sizeof(line))
                                 ? static_cast<uint32_t>(n)
                                 : static_cast<uint32_t>(sizeof(line) - 1U);
        (void)uart_write(reinterpret_cast<const uint8_t*>(line), len);
    }
#else
    (void)fmt;
#endif
}

} // namespace bl
