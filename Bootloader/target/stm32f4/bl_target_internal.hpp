/**
 * @file    bl_target_internal.hpp
 * @brief   STM32F4 适配层内部共享声明（不属于移植契约）
 *
 * 只给 target/stm32f4/ 里的几个 .cpp 互相用，宿主工程和 core/ 都不需要它。
 * 放到这里而不是各自 extern 声明，是为了让「谁持有串口句柄」这件事
 * 只有一个出处。
 */
#ifndef BL_TARGET_INTERNAL_HPP
#define BL_TARGET_INTERNAL_HPP

#include "stm32f4xx_hal.h"

namespace bl {
namespace stm32f4 {

/// 控制台串口句柄（定义在 bl_port_uart_stm32f4.cpp）
UART_HandleTypeDef& console_uart() noexcept;

/**
 * @brief 等待发送移位寄存器彻底空掉
 *
 * 复位/跳转前调用，避免最后几行日志还没出完就被打断。
 * 带超时：串口从未初始化时 TC 永远不会置位，不能无限等下去。
 */
void console_tx_flush(uint32_t timeout_ms) noexcept;

} // namespace stm32f4
} // namespace bl

#endif /* BL_TARGET_INTERNAL_HPP */
