/**
 * @file    presets/stm32f405xx.h
 * @brief   预置参数：STM32F405（RG/RE/VG 等，1MB Flash）
 *
 * 用法：在 bl_config.h 第一节取消对应 include 的注释。
 *
 * 采用范围：STM32F405xx
 *   - Flash 1MB，扇区 S0-S11（S0-S3 各 16KB、S4 64KB、S5-S11 各 128KB）
 *   - SRAM 128KB = 112KB(0x20000000) + 16KB(0x2001C000)，连续
 *     另有 64KB CCM 在 0x10000000，不参与栈顶合法性校验
 *   - 最高主频 168MHz
 */
#ifndef BL_PRESET_STM32F405XX_H
#define BL_PRESET_STM32F405XX_H

/* ---- 标识 ---- */
#define BL_CHIP_NAME                "STM32F405xx"

/* ---- 存储器 ---- */
#define BL_FLASH_BASE               0x08000000UL
#define BL_FLASH_SIZE               (1024UL * 1024UL)

#define BL_SRAM_BASE                0x20000000UL
#define BL_SRAM_END                 0x20020000UL      /* 128KB，不含 */

/* ---- 配置区：F405 最后一个扇区 S11 = 128KB @ 0x080E0000 ---- */
#define BL_META_SIZE                (128UL * 1024UL)

/* ---------------------------------------------------------------------------
 * 时钟树（仅 target/stm32f4 适配会读这些值）
 *
 * HSE 8MHz --(PLLM)--> 1MHz --(PLLN)--> 336MHz --(PLLP)--> 168MHz SYSCLK
 *                                               |
 *                                               +--(PLLQ)--> 48MHz
 * -------------------------------------------------------------------------*/
#define BL_HSE_HZ                   8000000UL
#define BL_SYSCLK_HZ                168000000UL

#define BL_PLL_M                    8UL
#define BL_PLL_N                    336UL
#define BL_PLL_P                    2UL        /* 只允许 2/4/6/8 */
#define BL_PLL_Q                    7UL

#define BL_AHB_DIV                  1UL        /* HCLK  = 168MHz */
#define BL_APB1_DIV                 4UL        /* PCLK1 = 42MHz  */
#define BL_APB2_DIV                 2UL        /* PCLK2 = 84MHz  */

#define BL_FLASH_LATENCY            5UL        /* 168MHz @ 2.7-3.6V 需 5 个等待周期 */

#endif /* BL_PRESET_STM32F405XX_H */
