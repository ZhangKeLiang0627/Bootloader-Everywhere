/**
 * @file    presets/stm32f407xx.h
 * @brief   预置参数：STM32F407（VG/ZG 等，1MB Flash）
 *
 * 用法：在 bl_config.h 第一节取消对应 include 的注释。
 *
 * 采用范围：STM32F407xx，1MB 容量型号
 *   （407VE/VC 是 512KB，请改用 stm32f401xe.h 的分区思路自行手填）
 *   - Flash 1MB，扇区 S0-S11（S0-S3 各 16KB、S4 64KB、S5-S11 各 128KB）
 *   - SRAM 128KB = 112KB + 16KB，连续
 *   - 最高主频 168MHz
 *
 * 与 STM32F405 的差别只在核对表级别（多了以太网/摄像头接口等外设），
 * 存储与时钟结构完全一致。
 */
#ifndef BL_PRESET_STM32F407XX_H
#define BL_PRESET_STM32F407XX_H

/* ---- 标识 ---- */
#define BL_CHIP_NAME                "STM32F407xx"

/* ---- 存储器 ---- */
#define BL_FLASH_BASE               0x08000000UL
#define BL_FLASH_SIZE               (1024UL * 1024UL)

#define BL_SRAM_BASE                0x20000000UL
#define BL_SRAM_END                 0x20020000UL      /* 128KB，不含 */

/* ---- 配置区：最后一个扇区 S11 = 128KB @ 0x080E0000 ---- */
#define BL_META_SIZE                (128UL * 1024UL)

/* ---- 时钟树：HSE 8MHz → 168MHz ---- */
#define BL_HSE_HZ                   8000000UL
#define BL_SYSCLK_HZ                168000000UL

#define BL_PLL_M                    8UL
#define BL_PLL_N                    336UL
#define BL_PLL_P                    2UL
#define BL_PLL_Q                    7UL

#define BL_AHB_DIV                  1UL
#define BL_APB1_DIV                 4UL
#define BL_APB2_DIV                 2UL

#define BL_FLASH_LATENCY            5UL

#endif /* BL_PRESET_STM32F407XX_H */
