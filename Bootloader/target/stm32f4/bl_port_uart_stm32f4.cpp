/**
 * @file    bl_port_uart_stm32f4.cpp
 * @brief   STM32F4 串口驱动 —— 自持句柄与引脚，不依赖宿主工程的 usart.c
 *
 * 方案：阻塞轮询 + 总超时。
 *   YMODEM 是停等协议（PC 收到 ACK 才发下一帧），MCU 收 1KB 约 89ms，
 *   然后用 1ms 级的时间校验并写 Flash —— 不存在「边收边处理」的压力。
 *   115200 下字节间隔 87µs，而单字节轮询开销只有几微秒，余量两个数量级，
 *   所以不会丢字节。换来的是行为可预测、没有中断竞态。
 *
 * 什么时候必须换实现：波特率提到 921600 时字节间隔降到 10.8µs，
 * 轮询就濒临丢字节，届时把 uart_read 改成 DMA + 环形缓冲即可 ——
 * port/bl_port.hpp 的接口一行都不用动。
 */
#include "bl_config.h"          /* 必须最先 */

#include "target/stm32f4/bl_target_config.h"
#include "target/stm32f4/bl_target_internal.hpp"

#include "port/bl_port.hpp"

namespace bl {
namespace {

/** 控制台串口句柄：由本文件独占持有 */
UART_HandleTypeDef g_uart{};

/** 配置 TX/RX 引脚复用。放在 uart_init 里而不是 MspInit，
 *  是为了让整个适配层不依赖 HAL 的回调约定，调用路径更直白。 */
void gpio_setup() noexcept
{
    BL_UART_GPIO_CLK_ENABLE();
    BL_UART_CLK_ENABLE();

    GPIO_InitTypeDef gpio{};
    gpio.Pin       = BL_UART_TX_PIN | BL_UART_RX_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = BL_UART_GPIO_AF;
    HAL_GPIO_Init(BL_UART_GPIO_PORT, &gpio);
}

} // namespace

namespace stm32f4 {

void console_tx_flush(uint32_t timeout_ms) noexcept
{
    const uint32_t start = HAL_GetTick();
    while (__HAL_UART_GET_FLAG(&g_uart, UART_FLAG_TC) == RESET) {
        if ((HAL_GetTick() - start) >= timeout_ms) {
            break;
        }
    }
}

} // namespace stm32f4

/* ========================================================================
 * 初始化
 * ======================================================================*/
Status uart_init(uint32_t baudrate) noexcept
{
    if (baudrate == 0U) {
        return Status::BadParam;
    }

    gpio_setup();

    g_uart.Instance          = BL_UART_INSTANCE;
    g_uart.Init.BaudRate     = baudrate;
    g_uart.Init.WordLength   = UART_WORDLENGTH_8B;
    g_uart.Init.StopBits     = UART_STOPBITS_1;
    g_uart.Init.Parity       = UART_PARITY_NONE;
    g_uart.Init.Mode         = UART_MODE_TX_RX;
    g_uart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    g_uart.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&g_uart) != HAL_OK) {
        return Status::Error;
    }

    uart_flush_rx();
    return Status::Ok;
}

/* ---- 读取：逐字节收，带总超时 ---- */
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
                break;                          /* 总超时 */
            }
            slice = timeout_ms - elapsed;
        } else {
            slice = 1U;                         /* 0 表示只试一次 */
        }

        uint8_t ch = 0;
        if (HAL_UART_Receive(&g_uart, &ch, 1, slice) == HAL_OK) {
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

/// 非阻塞探测单字节：直接读 DR 才是真「不等待」
bool uart_try_getc(uint8_t* ch) noexcept
{
    if (ch == nullptr) {
        return false;
    }
    if (__HAL_UART_GET_FLAG(&g_uart, UART_FLAG_RXNE) == RESET) {
        return false;
    }
    *ch = static_cast<uint8_t>(g_uart.Instance->DR & 0xFFU);
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

    const uint32_t slice = 1000U + (len / 10U);      /* 按长度给足余量 */

    if (HAL_UART_Transmit(&g_uart, const_cast<uint8_t*>(buf), len, slice) != HAL_OK) {
        return Status::Timeout;
    }
    return Status::Ok;
}

/* ========================================================================
 * 清空接收缓冲
 * ======================================================================*/
void uart_flush_rx() noexcept
{
    /* 先清错误标志，否则后续接收会一直被阻塞 */
    __HAL_UART_CLEAR_OREFLAG(&g_uart);
    __HAL_UART_CLEAR_FEFLAG(&g_uart);
    __HAL_UART_CLEAR_NEFLAG(&g_uart);
    __HAL_UART_CLEAR_PEFLAG(&g_uart);

    uint32_t guard = 0;
    while (__HAL_UART_GET_FLAG(&g_uart, UART_FLAG_RXNE) != RESET && guard++ < 4096U) {
        (void)g_uart.Instance->DR;
    }
}

} // namespace bl
