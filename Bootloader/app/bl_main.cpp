/**
 * @file    bl_main.cpp
 * @brief   Bootloader 主流程
 *
 * 由 CubeMX 生成的 Core/Src/main.c 在 USER CODE 2 段调用 Main()。
 * 此时 HAL_Init / SystemClock_Config / MX_GPIO_Init / MX_USART1_UART_Init
 * 均已完成，本函数只负责业务逻辑。
 *
 * 上线流程：
 *   自检 → 装载配置区 → 启动决策 → 跳转 APP 或进入 IAP 循环
 */
#include "bl_port.hpp"
#include "bl_log.hpp"
#include "bl_meta.hpp"
#include "bl_crc.hpp"
#include "bl_verify.hpp"
#include "bl_boot.hpp"
#include "bl_session.hpp"
#include "bl_config.h"

using namespace bl;

/* ========================================================================
 * 内部辅助
 * ======================================================================*/
namespace {

const char* action_name(Boot::Action a) noexcept
{
    return (a == Boot::Action::JumpToApp) ? "JUMP" : "IAP";
}

const char* outcome_name(IapResult r) noexcept
{
    switch (r) {
        case IapResult::Idle:    return "IDLE";
        case IapResult::Running: return "RUNNING";
        case IapResult::Done:    return "DONE";
        case IapResult::Failed:  return "FAILED";
        case IapResult::Aborted: return "ABORTED";
        case IapResult::NoSpace: return "NOSPACE";
        default:                 return "?";
    }
}

/// 出现无法继续的错误时，停在这里并周期性输出，便于排查
[[noreturn]] void fatal(const char* why) noexcept
{
    for (;;) {
        BL_LOG("[main] FATAL: %s\r\n", why);
        /* 有意不喂狗：若启用了看门狗，让系统复位重来 */
        for (volatile uint32_t i = 0; i < 8000000U; ++i) {
        }
    }
}

} // namespace

/* ========================================================================
 * 入口
 * ======================================================================*/
extern "C" void Main(void)
{
    /* ---- 0. 给外设和串口一点上电稳定时间 ---- */
    delay_ms(50);

    /* ---- 1. 串口（确保波特率与约定一致） ---- */
    if (!ok(uart_init(BL_UART_BAUDRATE))) {
        fatal("uart init failed");
    }

    /* ---- 2. Flash 接口 ---- */
    if (!ok(flash_init())) {
        fatal("flash init failed");
    }

    /* ---- 3. CRC 自检 ----
     * 参数写错时现象是「PC 算的值和板子算的对不上」，极难排查，
     * 所以在最早的时机用标准测试向量把它暴露出来。 */
    if (!ok(crc_selftest())) {
        fatal("crc selftest failed (check poly/init)");
    }

    /* ---- 4. 装载配置区 ---- */
    if (!ok(meta().init())) {
        fatal("meta init failed");
    }

#if BL_USE_WATCHDOG
    wdg_init();
#endif

    BL_LOG("\r\n===== LUMOS-bootloader =====\r\n");
    BL_LOG("[main] boot  @ 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_BOOT_BASE),
           static_cast<unsigned long>(BL_BOOT_SIZE / 1024U));
    BL_LOG("[main] app   @ 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_APP_BASE),
           static_cast<unsigned long>(BL_APP_SIZE / 1024U));
    BL_LOG("[main] meta  @ 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_META_BASE),
           static_cast<unsigned long>(BL_META_SIZE / 1024U));
    meta().dump();

    /* ---- 5. 启动决策 ---- */
    const Boot::Decision decision = Boot::decide(Boot::Config{});
    BL_LOG("[main] decision: %s (%s)\r\n",
           action_name(decision.action), decision.reason);

    if (decision.action == Boot::Action::JumpToApp) {
        Boot::jump(BL_APP_BASE);
        /* 跳转失败才走到这里，降级进入 IAP */
        BL_LOG("[main] jump failed, fallback to IAP\r\n");
    }

    /* ---- 6. IAP 循环 ----
     * 每次循环处理一轮升级。失败不会退出，继续等下一次尝试——
     * 只要 Bootloader 还在跑，设备就永远有救回来的机会。 */
    for (;;) {
        BL_LOG("[main] waiting for YMODEM transfer...\r\n");

        Session session;
        const Session::Result r = session.run();

        BL_LOG("[main] outcome=%s size=%lu crc=0x%08lX file=%s\r\n",
               outcome_name(r.outcome),
               static_cast<unsigned long>(r.fw_size),
               static_cast<unsigned long>(r.fw_crc32),
               r.filename);

        if (r.outcome == IapResult::Done) {
            /* 升级成功。复位后由启动决策重新校验并跳转新固件，
             * 这样「跳转路径」只有一条，减少分叉。 */
            BL_LOG("[main] upgrade done, rebooting...\r\n");
            delay_ms(300);
            system_reset();
        }

        /* 失败：短暂停顿后重新进入等待，避免串口刷屏 */
        delay_ms(200);
    }
}
