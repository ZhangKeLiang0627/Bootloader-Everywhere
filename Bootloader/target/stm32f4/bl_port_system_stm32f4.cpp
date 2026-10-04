/**
 * @file    bl_port_system_stm32f4.cpp
 * @brief   STM32F4 系统控制：跳转、看门狗、时基、复位
 */
#include "bl_config.h"          /* 必须最先 */

#include "target/stm32f4/bl_target_internal.hpp"

#include "port/bl_port.hpp"
#include "core/bl_log.hpp"

namespace bl {

/* ========================================================================
 * 跳转到 APP
 *
 * 八个步骤缺一不可，顺序也不能乱。漏掉哪一步的典型症状：
 *   - 不停 SysTick       → APP 里 HAL_Delay 走时不对
 *   - 不复位 RCC         → APP 以为时钟还是 Bootloader 配的，串口波特率全错
 *   - 不清 NVIC 挂起标志 → APP 一开中断就冲进某个已挂起的中断服务函数
 *   - 不设 VTOR          → APP 的中断跳到 Bootloader 的向量表里
 *   - 不设 MSP           → 栈指针还停在 Bootloader 的栈上，一压栈就踩坏数据
 *   - 不清 CONTROL       → 若此前用过 PSP，APP 会在错误的栈上运行
 * ======================================================================*/
void jump_to_app(uint32_t app_base) noexcept
{
    /* 取向量表前两字：初始栈顶与复位入口 */
    const uint32_t initial_sp = *reinterpret_cast<volatile uint32_t*>(app_base);
    const uint32_t reset_vec  = *reinterpret_cast<volatile uint32_t*>(app_base + 4U);

    BL_LOG("[jump] sp=0x%08lX entry=0x%08lX\r\n",
           static_cast<unsigned long>(initial_sp),
           static_cast<unsigned long>(reset_vec));

    /* 1. 关全局中断 */
    __disable_irq();

    /* 2. 停 SysTick 并清计数 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* 3. 复位 RCC 到默认态（HSI）。
     *    APP 的 SystemInit 会按自己的配置重建 PLL。 */
    HAL_RCC_DeInit();

    /* 4. 清所有 NVIC 中断使能与挂起标志 */
    for (uint32_t i = 0; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* 5. 重定位向量表到 APP */
    SCB->VTOR = app_base;

    /* 6. 设置主堆栈指针 */
    __set_MSP(initial_sp);

    /* 7. 回到特权级 + 使用 MSP（若之前用过 PSP） */
    __set_CONTROL(0U);

    /* 8. 开中断并跳转 */
    __enable_irq();

    using AppEntry = void (*)(void);
    auto entry = reinterpret_cast<AppEntry>(reset_vec);
    entry();

    /* 正常情况下不会执行到这里 */
    for (;;) {
    }
}

/* ========================================================================
 * 看门狗
 *
 * 直接用寄存器操作 IWDG，不走 HAL。原因：
 *   - 本工程的 HAL 配置里没有启用 HAL_IWDG_MODULE_ENABLED，
 *     HAL 的 iwdg 源文件也就没被生成；
 *   - IWDG 只有 4 个寄存器、3 个键值，直接操作更直观；
 *   - 省下一个 HAL 模块的 ROM 占用（Bootloader 只有 16KB 可用）。
 *
 * 提醒：IWDG 一旦启动就无法停止（只能靠复位），因此启用后 APP 也必须
 * 持续喂狗，否则 APP 会被反复复位。由 BL_USE_WATCHDOG 控制开关。
 *
 * 超时值取 BL_WATCHDOG_TIMEOUT_MS：它必须大于「中间没机会喂狗的最长阻塞」，
 * 也就是一次整扇区擦除（128KB 最坏 4 秒）。默认给 6 秒。
 * ======================================================================*/
namespace {

/* IWDG 寄存器布局（RM0090 §21.4） */
struct IwdgRegs {
    volatile uint32_t KR;    ///< 键寄存器   偏移 0x00
    volatile uint32_t PR;    ///< 预分频     偏移 0x04
    volatile uint32_t RLR;   ///< 重载值     偏移 0x08
    volatile uint32_t SR;    ///< 状态       偏移 0x0C
};

constexpr uint32_t kIwdgBase = 0x40003000UL;

/* KR 的三个键值 */
constexpr uint16_t kKeyReload      = 0xAAAAU;   ///< 重载计数器（喂狗）
constexpr uint16_t kKeyEnable      = 0xCCCCU;   ///< 启动看门狗
constexpr uint16_t kKeyWriteEnable = 0x5555U;   ///< 允许改写 PR/RLR

/* LSI 约 32kHz，PR=4 对应 64 分频 → 计数器 500 Hz，即每 2ms 加一 */
constexpr uint32_t kPrescalerDiv64 = 4U;
constexpr uint32_t kTickHz         = 500U;

/* 重载值 = 超时(ms) × 500 / 1000；寄存器只有 12 位，上限 4095 */
constexpr uint32_t kReloadRaw = BL_WATCHDOG_TIMEOUT_MS * kTickHz / 1000U;
constexpr uint32_t kReloadValue = (kReloadRaw > 4095U) ? 4095U : kReloadRaw;

static_assert(BL_WATCHDOG_TIMEOUT_MS >= 100U, "BL_WATCHDOG_TIMEOUT_MS 太小，没有意义");
static_assert(kReloadRaw <= 4095U,
              "BL_WATCHDOG_TIMEOUT_MS 超过 IWDG 上限（500Hz 下最长约 8.19 秒），"
              "请改小或调整 kPrescalerDiv64 并同步修改 kTickHz");

inline IwdgRegs& iwdg() noexcept
{
    return *reinterpret_cast<IwdgRegs*>(kIwdgBase);
}

} // namespace

void wdg_init() noexcept
{
#if BL_USE_WATCHDOG
    /* 确保 LSI 已起振（IWDG 的时钟源） */
    RCC->CSR |= RCC_CSR_LSION;
    while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) {
    }

    iwdg().KR = kKeyWriteEnable;
    iwdg().PR = kPrescalerDiv64;
    iwdg().RLR = kReloadValue;
    iwdg().KR = kKeyReload;      /* 装载 */
    iwdg().KR = kKeyEnable;      /* 启动，此后不可停止 */
#endif
}

void wdg_feed() noexcept
{
#if BL_USE_WATCHDOG
    iwdg().KR = kKeyReload;
#endif
}

/* ========================================================================
 * 时基
 * ======================================================================*/
uint32_t tick_ms() noexcept
{
    return HAL_GetTick();
}

void delay_ms(uint32_t ms) noexcept
{
    /* 分片延时并喂狗：单次长延时可能超过看门狗超时 */
    const uint32_t slice = 100U;
    while (ms > 0U) {
        const uint32_t step = (ms > slice) ? slice : ms;
        HAL_Delay(step);
        wdg_feed();
        ms -= step;
    }
}

/* ========================================================================
 * 复位
 * ======================================================================*/
void system_reset() noexcept
{
    /* 等串口把最后几行日志发完再复位，否则调试时总会缺半行 */
    stm32f4::console_tx_flush(100U);

    NVIC_SystemReset();

    for (;;) {
    }
}

} // namespace bl
