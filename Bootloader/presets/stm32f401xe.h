/**
 * @file    presets/stm32f401xe.h
 * @brief   预置参数：STM32F401xE（Nucleo-F401RE / 最小系统板等，512KB Flash）
 *
 * 用法：在 bl_config.h 第一节取消对应 include 的注释。
 *
 * 采用范围：STM32F401xD / STM32F401xE（RE/RD/CE/CD/VD）
 *   - Flash 512KB，扇区 S0-S7（S0-S3 各 16KB、S4 64KB、S5-S7 各 128KB）
 *   - SRAM 96KB，连续位于 0x20000000
 *   - 最高主频 84MHz（注意：不是 168MHz，那是 F405/F407 的规格）
 */
#ifndef BL_PRESET_STM32F401XE_H
#define BL_PRESET_STM32F401XE_H

/* ---- 标识 ---- */
#define BL_CHIP_NAME                "STM32F401xE"

/* ---- 存储器 ---- */
#define BL_FLASH_BASE               0x08000000UL
#define BL_FLASH_SIZE               (512UL * 1024UL)

#define BL_SRAM_BASE                0x20000000UL
#define BL_SRAM_END                 0x20018000UL      /* 96KB，不含 */

/* ---- 配置区：F401 最后一个扇区 S7 = 128KB @ 0x08060000 ---- */
#define BL_META_SIZE                (128UL * 1024UL)

/* ---------------------------------------------------------------------------
 * 时钟树（仅 target/stm32f4 适配会读这些值）
 *
 * HSE 25MHz --(PLLM)--> 1MHz --(PLLN)--> 336MHz --(PLLP)--> 84MHz SYSCLK
 *                                              |
 *                                              +--(PLLQ)--> 48MHz（USB/SDIO，
 *                                                 Bootloader 用不到，仅保持规格正确）
 *
 * 注意：不同板子的晶振不一样。Nucleo-F401RE 出厂不焊 HSE，靠 ST-Link 的
 * MCO 供时钟；自研板常见 25MHz 或 8MHz。填错的现象是「串口全是乱码」，
 * 因为 HAL 会按错误的 SystemCoreClock 算分频。若不确定，把
 * BL_CLOCK_ALLOW_HSI_FALLBACK 保持为 1：HSE 起振失败会自动退回内部
 * HSI 16MHz 跑起来，虽然慢但至少能通信，方便定位。
 * -------------------------------------------------------------------------*/
#define BL_HSE_HZ                   25000000UL
#define BL_SYSCLK_HZ                84000000UL

#define BL_PLL_M                    25UL
#define BL_PLL_N                    336UL
#define BL_PLL_P                    4UL        /* 只允许 2/4/6/8 */
#define BL_PLL_Q                    7UL

#define BL_AHB_DIV                  1UL        /* HCLK  = SYSCLK / N */
#define BL_APB1_DIV                 2UL        /* PCLK1 = HCLK   / N */
#define BL_APB2_DIV                 1UL        /* PCLK2 = HCLK   / N */

#define BL_FLASH_LATENCY            2UL        /* 84MHz @ 2.7-3.6V 需 2 个等待周期 */

#endif /* BL_PRESET_STM32F401XE_H */
