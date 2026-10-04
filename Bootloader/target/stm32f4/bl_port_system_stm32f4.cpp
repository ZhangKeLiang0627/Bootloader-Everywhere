/**
 * @file    bl_port_system_stm32f4.cpp
 * @brief   STM32F4 系统控制：跳转、时基、复位原因
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
    stm32f4::console_tx_flush(100U);   /* 等最后几行日志发完再跳 */

    /* 取向量表前两字：初始栈顶与复位入口 */
    const uint32_t initial_sp = *reinterpret_cast<volatile uint32_t*>(app_base);
    const uint32_t reset_vec  = *reinterpret_cast<volatile uint32_t*>(app_base + 4U);

    BL_LOG("[jump] sp=0x%08lX entry=0x%08lX\r\n",
           static_cast<unsigned long>(initial_sp),
           static_cast<unsigned long>(reset_vec));

    /* 1. 关全局中断 */
    __disable_irq();

    /* 2. 复位 RCC 到默认态（HSI）。
     *    APP 的 SystemInit 会按自己的配置重建 PLL。
     *
     * ⚠️ 顺序陷阱（实测踩过，很隐蔽）：
     *    HAL_RCC_DeInit() 内部末尾会调用 HAL_InitTick()，
     *    也就是**重新把 SysTick 配成 1ms 并使能它的中断**。
     *    因此「关 SysTick」必须放在它**之后**，放到前面会被它悄悄重新打开。
     *    后果是 APP 一跑起来就不断被 SysTick 中断打断，而 APP 的向量表里
     *    SysTick_Handler 通常是空的（Default_Handler = 一条 B .），
     *    于是直接卡死在异常处理里 —— 现象是「APP 完全不输出任何字符」，
     *    光看串口根本无法定位。必须用调试器读 ICSR 才能看到
     *    VECTACTIVE = 15。 */
    HAL_RCC_DeInit();

    /* 3. 关 SysTick 并清掉可能已经挂起的请求 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* SysTick / PendSV 是系统异常，不在 NVIC 里，必须单独清挂起位，
     * 否则 APP 一开中断就会立刻冲进这两个 handler。 */
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* 4. 清所有 NVIC 中断使能与挂起标志 */
    for (uint32_t i = 0; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* 5. 重定位向量表到 APP，并保证它对后续取指立即生效 */
    SCB->VTOR = app_base;
    __DSB();

    /* 6. 设置主堆栈指针 */
    __set_MSP(initial_sp);

    /* 7. 回到特权级 + 使用 MSP（若之前用过 PSP） */
    __set_CONTROL(0U);
    __ISB();

    /* 8. 开中断并跳转 */
    __enable_irq();

    using AppEntry = void (*)(void);
    auto entry = reinterpret_cast<AppEntry>(reset_vec);
    entry();

    /* 正常情况下不会执行到这里 */
    for (;;) {
    }
}

/* ---- 时基 ---- */

uint32_t tick_ms() noexcept
{
    return HAL_GetTick();
}

void delay_ms(uint32_t ms) noexcept
{
    HAL_Delay(ms);
}

/* ---- 复位原因 ---- */

ResetCause reset_cause() noexcept
{
    // 复位标志是累积的，读后即清；SFTRSTF 优先于 POR/PIN：
    // 探针连着时软件复位会连带拉 NRST，PINRSTF 同时置位
    const uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;

    if ((csr & RCC_CSR_SFTRSTF)  != 0U) { return ResetCause::Software; }
    if ((csr & RCC_CSR_PORRSTF)  != 0U) { return ResetCause::PowerOn;  }
    if ((csr & RCC_CSR_PINRSTF)  != 0U) { return ResetCause::Pin;      }
    if ((csr & RCC_CSR_BORRSTF)  != 0U) { return ResetCause::BrownOut; }
    if ((csr & RCC_CSR_LPWRRSTF) != 0U) { return ResetCause::LowPower; }

    return ResetCause::Unknown;
}

} // namespace bl
