/**
 * @file    bl_target_config.h
 * @brief   STM32F4 适配层的板级配置 —— 换板子只改这个文件
 *
 * 分工：
 *   bl_config.h            芯片级：Flash 容量、分区、SRAM 范围、时钟树
 *   target/stm32f4/bl_target_config.h   板级：调试串口用哪一路、哪几个引脚
 *
 * 为什么把串口引脚单独拎出来：它既不属于「芯片规格」（同一颗 F405
 * 换块板子就可能换脚），也不适合塞进通用配置 —— 别的平台根本没有
 * GPIO_AF7_USART1 这种概念，放进去会污染 bl_config.h 的跨平台性。
 *
 * 默认值取自本仓库目标板（USART1 / PA9 / PA10），与 STM32F401-DAP
 * 工程一致。接线不同就改下面几行。
 */
#ifndef BL_TARGET_CONFIG_H
#define BL_TARGET_CONFIG_H

#include "stm32f4xx_hal.h"

/* ---- 调试 / IAP 串口 ---- */
#define BL_UART_INSTANCE        USART1
#define BL_UART_GPIO_PORT       GPIOA
#define BL_UART_TX_PIN          GPIO_PIN_9
#define BL_UART_RX_PIN          GPIO_PIN_10
#define BL_UART_GPIO_AF         GPIO_AF7_USART1

#define BL_UART_CLK_ENABLE()    __HAL_RCC_USART1_CLK_ENABLE()
#define BL_UART_GPIO_CLK_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()

/* 中断向量名（HardFault 直写寄存器时用不到，留给需要中断收发的场景） */
#define BL_UART_IRQn            USART1_IRQn

#endif /* BL_TARGET_CONFIG_H */
