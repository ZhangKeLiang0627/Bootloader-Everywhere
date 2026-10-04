/**
 * @file    bl_platform_stm32f4.cpp
 * @brief   STM32F4 平台初始化 —— HAL 底座、时钟树、时基与异常处理
 *
 * 为什么 Bootloader 自带这些，而不用 CubeMX 生成的 Core/Src：
 *   Core/Src 是「某一颗芯片 + 某一块板子」的产物，时钟树、引脚、外设实例
 *   全被固化在里面。Bootloader 要能整包搬到别的工程，就必须把这份差异
 *   收敛到 bl_config.h 的几个宏里，再由本文件按统一流程建立起来。
 *   于是搬运一个新平台只需要：写一份 target/<平台>/，bl_config.h 填数。
 *
 * 时钟方案取自 bl_config.h（通常来自 presets/ 下的预置头）：
 *   HSE --(PLLM)--> 1MHz 参考 --(PLLN)--> VCO --(PLLP)--> SYSCLK
 *
 * HSE 与 HSE_VALUE 必须一致，否则 HAL 会算错系统频率、串口波特率全错。
 * 本文件顶部有编译期自检，不一致会直接编译不过（而不是上板才发现乱码）。
 */
#include "bl_config.h"          /* 必须最先：把芯片/时钟参数带进来 */

#include "stm32f4xx_hal.h"

#include "port/bl_port.hpp"
#include "core/bl_log.hpp"

/* ============================================================================
 * 编译期自检：参数齐不齐、自不自洽
 * ==========================================================================*/
#if !defined(BL_HSE_HZ) || !defined(BL_PLL_M) || !defined(BL_PLL_N) || \
    !defined(BL_PLL_P) || !defined(BL_PLL_Q) || !defined(BL_AHB_DIV) || \
    !defined(BL_APB1_DIV) || !defined(BL_APB2_DIV) || !defined(BL_FLASH_LATENCY)
#error "target/stm32f4 需要时钟参数：请在 bl_config.h 第一节选用 presets 里的某个 STM32F4 预置，或手填 BL_HSE_HZ / BL_PLL_* / BL_AHB_DIV / BL_APB1_DIV / BL_APB2_DIV / BL_FLASH_LATENCY"
#endif

#if defined(HSE_VALUE) && (HSE_VALUE != BL_HSE_HZ)
#error "HSE_VALUE 与 bl_config.h 的 BL_HSE_HZ 不一致。HAL 会按 HSE_VALUE 反算系统频率，导致串口波特率全错（现象是乱码）。请在工程选项里加宏 HSE_VALUE=<BL_HSE_HZ>，或改掉 bl_config.h 里的 BL_HSE_HZ。"
#endif

namespace bl {
namespace {

/* ============================================================================
 * 把 bl_config.h 里的「分频数字」翻译成 HAL 的枚举
 *
 * 用数字而不是直接写 HAL 枚举，是为了让 bl_config.h 不必包含任何平台头文件
 * —— 那份配置对 GD32/CH32 也照样能看懂。
 * ==========================================================================*/

static_assert(BL_PLL_P == 2 || BL_PLL_P == 4 || BL_PLL_P == 6 || BL_PLL_P == 8,
              "BL_PLL_P 只允许 2 / 4 / 6 / 8");

static_assert(BL_AHB_DIV == 1 || BL_AHB_DIV == 2 || BL_AHB_DIV == 4 ||
                  BL_AHB_DIV == 8 || BL_AHB_DIV == 16 || BL_AHB_DIV == 64 ||
                  BL_AHB_DIV == 128 || BL_AHB_DIV == 256 || BL_AHB_DIV == 512,
              "BL_AHB_DIV 取值非法");

static_assert(BL_APB1_DIV == 1 || BL_APB1_DIV == 2 || BL_APB1_DIV == 4 ||
                  BL_APB1_DIV == 8 || BL_APB1_DIV == 16,
              "BL_APB1_DIV 取值非法");

static_assert(BL_APB2_DIV == 1 || BL_APB2_DIV == 2 || BL_APB2_DIV == 4 ||
                  BL_APB2_DIV == 8 || BL_APB2_DIV == 16,
              "BL_APB2_DIV 取值非法");

constexpr uint32_t pll_p_enum() noexcept
{
    return (BL_PLL_P == 2) ? RCC_PLLP_DIV2 :
           (BL_PLL_P == 4) ? RCC_PLLP_DIV4 :
           (BL_PLL_P == 6) ? RCC_PLLP_DIV6 : RCC_PLLP_DIV8;
}

constexpr uint32_t ahb_div_enum() noexcept
{
    return (BL_AHB_DIV == 1)   ? RCC_SYSCLK_DIV1   :
           (BL_AHB_DIV == 2)   ? RCC_SYSCLK_DIV2   :
           (BL_AHB_DIV == 4)   ? RCC_SYSCLK_DIV4   :
           (BL_AHB_DIV == 8)   ? RCC_SYSCLK_DIV8   :
           (BL_AHB_DIV == 16)  ? RCC_SYSCLK_DIV16  :
           (BL_AHB_DIV == 64)  ? RCC_SYSCLK_DIV64  :
           (BL_AHB_DIV == 128) ? RCC_SYSCLK_DIV128 :
           (BL_AHB_DIV == 256) ? RCC_SYSCLK_DIV256 : RCC_SYSCLK_DIV512;
}

constexpr uint32_t apb1_div_enum() noexcept
{
    return (BL_APB1_DIV == 1)  ? RCC_HCLK_DIV1  :
           (BL_APB1_DIV == 2)  ? RCC_HCLK_DIV2  :
           (BL_APB1_DIV == 4)  ? RCC_HCLK_DIV4  :
           (BL_APB1_DIV == 8)  ? RCC_HCLK_DIV8 : RCC_HCLK_DIV16;
}

constexpr uint32_t apb2_div_enum() noexcept
{
    return (BL_APB2_DIV == 1)  ? RCC_HCLK_DIV1  :
           (BL_APB2_DIV == 2)  ? RCC_HCLK_DIV2  :
           (BL_APB2_DIV == 4)  ? RCC_HCLK_DIV4  :
           (BL_APB2_DIV == 8)  ? RCC_HCLK_DIV8 : RCC_HCLK_DIV16;
}

/* ============================================================================
 * 时钟配置
 * ==========================================================================*/

/** 按 bl_config.h 建立 HSE + PLL 时钟；HSE 起振失败返回失败（由调用方兜底） */
Status clock_from_hse() noexcept
{
    RCC_OscInitTypeDef osc{};
    RCC_ClkInitTypeDef clk{};

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = BL_PLL_M;
    osc.PLL.PLLN       = BL_PLL_N;
    osc.PLL.PLLP       = pll_p_enum();
    osc.PLL.PLLQ       = BL_PLL_Q;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return Status::Error;
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = ahb_div_enum();
    clk.APB1CLKDivider = apb1_div_enum();
    clk.APB2CLKDivider = apb2_div_enum();

    if (HAL_RCC_ClockConfig(&clk, BL_FLASH_LATENCY) != HAL_OK) {
        return Status::Error;
    }
    return Status::Ok;
}

/**
 * 兜底：退回内部 HSI，不走 PLL
 *
 * 触发场景：板子没焊晶振、晶振虚焊、负载电容不匹配。
 * 宁可跑 16MHz 也要能通信 —— 一旦串口通不了，设备就成了砖，
 * 而 16MHz 下 115200 波特率照样工作（HAL 会按实际时钟算分频）。
 */
Status clock_fallback_hsi() noexcept
{
    RCC_OscInitTypeDef osc{};
    RCC_ClkInitTypeDef clk{};

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_NONE;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return Status::Error;
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
        return Status::Error;
    }
    return Status::Ok;
}

} // namespace

/* ============================================================================
 * 对外：平台初始化
 * ==========================================================================*/
namespace {

/**
 * @brief 使能 FPU（CP10 / CP11 全访问）
 *
 * 复位后 CPACR = 0，含义是「禁止访问协处理器」。本工程按硬浮点编译
 * （-mfloat-abi=hard），只要执行到一条 VFP 指令，就会立刻触发
 * UsageFault(NOCP) 并被升级成 HardFault —— 现象是刚启动就卡死，
 * 而且从表面完全看不出跟浮点有关，非常难猜。
 *
 * ST 的 system_stm32f4xx.c 在 SystemInit 里做的第一件事就是它，
 * 这里保持相同的位置与语义：**任何可能用到浮点的代码之前**。
 */
void fpu_enable() noexcept
{
    SCB->CPACR |= ((3UL << 20) | (3UL << 22));   /* CP10, CP11 全访问 */
    __DSB();
    __ISB();
}

} // namespace

Status platform_init() noexcept
{
    /* 0. FPU 必须最先使能 —— 见 fpu_enable 的说明 */
    fpu_enable();

    /* 1. HAL 底座：中断优先级分组、1ms 时基、HAL_MspInit */
    if (HAL_Init() != HAL_OK) {
        return Status::Error;
    }

    /* 2. 系统时钟 */
    const bool hse_ok = ok(clock_from_hse());
    if (!hse_ok) {
        if (!ok(clock_fallback_hsi())) {
            return Status::Error;
        }
    }

    /* 3. 时基要按时钟重算
     *    HAL_Init 里是按复位后的 16MHz 算的，切换 PLL 后 1ms 就不准了。 */
    SystemCoreClockUpdate();
    (void)HAL_InitTick(TICK_INT_PRIORITY);

    if (!hse_ok) {
        BL_LOG("[clk] HSE failed -> running on HSI 16MHz\r\n");
    }
    BL_LOG("[clk] sysclk=%lu Hz (core clock %lu Hz)\r\n",
           static_cast<unsigned long>(HAL_RCC_GetSysClockFreq()),
           static_cast<unsigned long>(SystemCoreClock));

    return Status::Ok;
}

} // namespace bl

/* ============================================================================
 * 中断与异常处理
 *
 * 这些原本由 CubeMX 生成的 Core/Src/stm32f4xx_it.c 与 stm32f4xx_hal_msp.c
 * 提供。因为库不依赖宿主工程的 Core/Src，所以自己实现一份最精简的：
 * 只保留 HAL 时基需要的 SysTick，以及 HAL 底座需要的 MspInit。
 * ==========================================================================*/

/** HAL 的 1ms 时基来源（tick_ms / delay_ms 都靠它） */
extern "C" void SysTick_Handler(void)
{
    HAL_IncTick();
}

/**
 * HAL 底层依赖：使能 SYSCFG 与 PWR 时钟
 *
 * 缺 PWR 时钟的典型症状：HAL_RCC_ClockConfig 里配置调压器失败，
 * 时钟切不过去，但函数未必报错 —— 很难查，所以这里必须开。
 */
extern "C" void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

/**
 * 硬件异常兜底
 *
 * 直写 USART1 数据寄存器汇报，不经过 HAL、不用缓冲区、不做格式化 ——
 * 即使故障来自栈损坏或内存错误，也还有机会把这句话吐出去。
 *
 * 之后死循环等待：若启用了看门狗，2 秒后会自动复位重来；
 * 若没启用，则停在原地等调试器接管。两种情况下设备都能救回来。
 */
extern "C" void HardFault_Handler(void)
{
    static const char kMsg[] = "\r\n!! HARDFAULT in LUMOS-bootloader !!\r\n";

    for (const char* p = kMsg; *p != '\0'; ++p) {
        while ((USART1->SR & USART_SR_TXE) == 0U) {
        }
        USART1->DR = static_cast<uint32_t>(static_cast<unsigned char>(*p));
    }

    for (;;) {
    }
}
