/**
 * @file    presets/gd32f303re.h
 * @brief   预置参数：GD32F303RE（兆易创新，512KB Flash）— 非 ST 平台示例
 *
 * 用法：在 bl_config.h 第一节取消对应 include 的注释。
 *
 * 这个预置有两个用途：
 *   1. 真要往 GD32 上移植时直接用；
 *   2. 作为「非 ST 平台长什么样」的对照 —— 说明本库的移植并不依赖 ST HAL。
 *
 * 与 STM32F4 的两处结构性差异（都在 target 适配里处理）：
 *   - 擦除单位是「页」而非「扇区」：512KB 型号每页 2KB，全片连续等长，
 *     所以配置区不需要占一整个大扇区，16KB 就够 256 个槽位。
 *   - 编程单位是半字（16 位）而非字（32 位），驱动要按半字写。
 *
 * 时钟树：GD32F303 的 PREDV/PLL 结构与 STM32F4 完全不同（PLL 从
 *         PREDV 输出倍频，且 AHBCLK 需先降速再切换），无法用下面这套
 *         BL_PLL_* 宏表达，因此 PREDV/倍频系数直接写在
 *         target/gd32f30x/bl_platform_gd32f30x.cpp 里。
 */
#ifndef BL_PRESET_GD32F303RE_H
#define BL_PRESET_GD32F303RE_H

/* ---- 标识 ---- */
#define BL_CHIP_NAME                "GD32F303RE"

/* ---- 存储器 ---- */
#define BL_FLASH_BASE               0x08000000UL
#define BL_FLASH_SIZE               (512UL * 1024UL)

#define BL_SRAM_BASE                0x20000000UL
#define BL_SRAM_END                 0x20010000UL      /* 64KB，不含 */

/* ---- 配置区：页 2KB，取 16KB = 8 页 ---- */
#define BL_META_SIZE                (16UL * 1024UL)

/* ---- 时钟（供日志打印；实际配置见 target/gd32f30x） ---- */
#define BL_HSE_HZ                   8000000UL
#define BL_SYSCLK_HZ                120000000UL

#endif /* BL_PRESET_GD32F303RE_H */
