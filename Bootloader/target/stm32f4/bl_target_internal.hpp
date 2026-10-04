/**
 * @file    bl_target_internal.hpp
 * @brief   STM32F4 适配层内部共享声明（不属于移植契约）
 */
#ifndef BL_TARGET_INTERNAL_HPP
#define BL_TARGET_INTERNAL_HPP

#include "stm32f4xx_hal.h"

namespace bl {
namespace stm32f4 {

/// 等发送移位寄存器空（跳转前调用，避免最后几行日志被打断）
void console_tx_flush(uint32_t timeout_ms) noexcept;

} // namespace stm32f4
} // namespace bl

#endif /* BL_TARGET_INTERNAL_HPP */
