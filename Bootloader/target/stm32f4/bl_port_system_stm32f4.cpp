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
    /* 顺序与 ST 的 HAL_IWDG_Init 保持一致：先启动、再改参数。
     *
     *   1) 写 0xCCCC 启动 IWDG —— 硬件会顺带把 LSI 振荡器打开
     *   2) 等 SR 的 PVU/RVU 落（此时 LSI 已在跑，这两位才可能被清除）
     *   3) 写 0x5555 允许改写，再写 PR/RLR
     *   4) 喂一次
     *
     * 反过来的顺序（先 0x5555 + PR/RLR 再 0xCCCC）会踩坑：
     * LSI 尚未起振时 IWDG 没有时钟，SR 的 PVU/RVU 会一直保持 1，
     * 任何"等它清零"的循环都会死锁。所有等待都带超时，硬件异常时也能走完。 */
    iwdg().KR = kKeyEnable;                          /* 启动（LSI 随之使能） */

    for (uint32_t i = 0; i < 0x100000U; ++i) {       /* 等 LSI 起振、SR 清零 */
        if (iwdg().SR == 0U) {
            break;
        }
    }

    iwdg().KR = kKeyWriteEnable;                     /* 允许改写 PR/RLR */
    iwdg().PR = kPrescalerDiv64;
    iwdg().RLR = kReloadValue;

    for (uint32_t i = 0; i < 0x100000U; ++i) {       /* 等参数写入生效 */
        if ((iwdg().SR & 0x3U) == 0U) {
            break;
        }
    }

    iwdg().KR = kKeyReload;                          /* 装载，此后不可停止 */
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
 * 复位原因
 *
 * RCC_CSR 里有一组**累积**的复位标志（写 RMVF 一次性清除）：
 *   LPWRRSTF  bit31  低功耗模式
 *   WWDGRSTF  bit30  窗口看门狗
 *   IWDGRSTF  bit29  独立看门狗      ← 这套自确认机制真正关心的
 *   SFTRSTF   bit28  软件复位（升级完成后本工程主动触发的那种）
 *   PORRSTF   bit27  上电 / 掉电
 *   PINRSTF   bit26  NRST 引脚（用户按复位键）
 *   BORRSTF   bit25  欠压
 *   RMVF      bit24  写 1 清除上面全部
 *
 * ⚠️ 正因为是累积的，必须「读后即清」—— 否则「看门狗复位」这个标志
 *    会一直粘着，之后每一次正常上电都会被误判成 APP 跑飞。
 *
 * ⚠️ 多个位可能同时置位（上电瞬间 POR 与 PIN 常一起置），所以判定按
 *    「信息量」排序：看门狗最具体，优先识别；不然 APP 跑飞会被误判成
 *    普通上电，回滚机制就形同虚设。
 *
 * ⚠️ 软件复位（SFTRSTF）排在 POR/PIN 之前：APP 软复位（SYSRESETREQ）时，
 *    若在线探针（DAPLink）连着，会连带把 NRST 拉一下，PINRSTF 同时置位。
 *    软件复位是更明确、更具体的意图，必须优先于引脚复位，否则
 *    「软件复位唤回 Bootloader」这条通道在开发期会被探针干扰而失灵。
 * ======================================================================*/
ResetCause reset_cause() noexcept
{
    const uint32_t csr = RCC->CSR;

    /* 读后即清 */
    RCC->CSR |= RCC_CSR_RMVF;

    if ((csr & RCC_CSR_IWDGRSTF) != 0U) { return ResetCause::Watchdog; }
    if ((csr & RCC_CSR_WWDGRSTF) != 0U) { return ResetCause::Watchdog; }
    if ((csr & RCC_CSR_SFTRSTF)  != 0U) { return ResetCause::Software; }
    if ((csr & RCC_CSR_PORRSTF)  != 0U) { return ResetCause::PowerOn;  }
    if ((csr & RCC_CSR_PINRSTF)  != 0U) { return ResetCause::Pin;      }
    if ((csr & RCC_CSR_BORRSTF)  != 0U) { return ResetCause::BrownOut; }
    if ((csr & RCC_CSR_LPWRRSTF) != 0U) { return ResetCause::LowPower; }

    return ResetCause::Unknown;
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
