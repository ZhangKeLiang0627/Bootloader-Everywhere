/**
 * @file    bl_boot.cpp
 * @brief   启动决策的实现
 */
#include "core/bl_boot.hpp"
#include "core/bl_meta.hpp"
#include "core/bl_verify.hpp"
#include "port/bl_port.hpp"
#include "core/bl_log.hpp"

namespace bl {

/* ========================================================================
 * Backdoor 窗口
 *
 * 用于「APP 本身是好的，但需要强制刷机」的场景（开发期尤其常用）。
 * 也是设备变砖后的最后一道人工救援通道。
 * ======================================================================*/
bool Boot::wait_backdoor(uint32_t window_ms, uint8_t trigger) noexcept
{
    if (window_ms == 0U) {
        return false;
    }

    const uint32_t start = tick_ms();

    while ((tick_ms() - start) < window_ms) {
        wdg_feed();

        uint8_t ch = 0;
        if (uart_try_getc(&ch)) {
            if (ch == trigger) {
                BL_LOG("[boot] backdoor triggered\r\n");
                return true;
            }
            /* 其他字节丢弃：这段时间的串口噪声不应被当成命令 */
        }
    }
    return false;
}

/* ========================================================================
 * 启动决策
 * ======================================================================*/
Boot::Decision Boot::decide() noexcept
{
    return decide(Config{});
}

Boot::Decision Boot::decide(const Config& cfg) noexcept
{
    Meta& m = meta();

    /* ---- 1. APP 是否请求过升级 ---- */
    if (m.update_requested()) {
        (void)m.clear_update_request();
        return { Action::EnterIap, "update requested by app" };
    }

    /* ---- 2. 状态是否可跳转 ---- */
    if (!m.should_boot()) {
        return { Action::EnterIap, "no bootable firmware" };
    }

    /* ---- 3. Backdoor 窗口 ---- */
    if (wait_backdoor(cfg.backdoor_window_ms, cfg.backdoor_char)) {
        return { Action::EnterIap, "backdoor" };
    }

    /* ---- 4. Testing 态的判据（「自确认」机制的核心） ----
     *
     * 本工程不要求 APP 主动调用任何确认接口。Bootloader 通过
     * **复位原因** 客观判断 APP 上一次是否活下来了：
     *
     *   看门狗复位 → APP 没喂狗（跑飞 / 卡死 / 一启动就崩）→ 计数 +1，超限回滚
     *   软件复位   → 升级完成后本工程主动复位的那次 → 第一次运行，保持 Testing
     *   其它       → 上电 / 按复位：说明 APP 上次活到了用户动手的那一刻
     *                → 判定健康，自动转为 Valid
     *
     * 这样 APP 侧零侵入：它不需要知道配置区地址、槽位格式、CRC 算法，
     * 也不必引入本库的任何头文件，只需要做它本来就该做的事 —— 喂狗。
     */
    if (m.state() == FwState::Testing) {
#if BL_BOOT_SELF_CONFIRM
        switch (reset_cause()) {
        case ResetCause::Watchdog: {
            /* APP 没能活过看门狗超时 —— 这是「跑不起来」的客观证据 */
            const uint32_t attempts = m.bump_boot_attempts();
            BL_LOG("[boot] watchdog reset: app did not survive (%lu/%lu)\r\n",
                   static_cast<unsigned long>(attempts),
                   static_cast<unsigned long>(cfg.max_attempts));

            if (attempts > cfg.max_attempts) {
                BL_LOG("[boot] rollback: app keeps failing after upgrade\r\n");
                (void)m.revoke();
                return { Action::EnterIap, "app keeps crashing (watchdog), revoked" };
            }
            break;                      /* 再给它一次机会 */
        }

        case ResetCause::Software:
            /* 升级完成后本工程主动触发的那次复位 —— 新固件还没被验证过 */
            (void)m.bump_boot_attempts();
            BL_LOG("[boot] first boot after upgrade, keep TESTING\r\n");
            break;

        default:
            /* 上电 / 按复位 / 欠压：APP 上次活到了用户动手的时刻。
             *
             * 但这份证据只在「从未因看门狗失败过」时才采信
             * （boot_attempts <= 1 表示只记了升级后那一次）。
             * 否则一个反复跑飞的固件只要用户断电上电就会被洗白成
             * Valid，回滚机制就永远触发不了 —— 那是比不回滚更糟的状态。 */
            if (m.current().boot_attempts <= 1U) {
                BL_LOG("[boot] clean boot -> firmware self-confirmed\r\n");
                (void)m.confirm_app();
            } else {
                BL_LOG("[boot] clean boot, but watchdog failures on record\r\n");
            }
            break;
        }
#else
        /* 未启用自确认：退回「每次启动都计数」，
         * 此时需要 APP 自己调用 bl::app_confirm()。 */
        const uint32_t attempts = m.bump_boot_attempts();
        BL_LOG("[boot] testing: attempt %lu/%lu\r\n",
               static_cast<unsigned long>(attempts),
               static_cast<unsigned long>(cfg.max_attempts));

        if (attempts > cfg.max_attempts) {
            BL_LOG("[boot] rollback: app never confirmed\r\n");
            (void)m.revoke();
            return { Action::EnterIap, "app not confirmed, revoked" };
        }
#endif
    }

    /* ---- 5. 向量表校验（廉价，每次必做） ---- */
    if (!ok(verify_vector_table(cfg.app_base, nullptr))) {
        BL_LOG("[boot] invalid vector table\r\n");
        (void)m.revoke();
        return { Action::EnterIap, "vector table invalid" };
    }

    /* ---- 6. 可选：整镜像 CRC32 ---- */
    if (cfg.verify_crc_on_boot) {
        const Meta::Slot& s = m.current();
        if (!ok(verify_image(cfg.app_base, s.fw_size, s.fw_crc32, nullptr))) {
            BL_LOG("[boot] image crc mismatch\r\n");
            (void)m.revoke();
            return { Action::EnterIap, "image crc mismatch" };
        }
    }

    /* ---- 7. 全部通过 ---- */
    return { Action::JumpToApp, "ok" };
}

/* ========================================================================
 * 跳转
 * ======================================================================*/
void Boot::jump(uint32_t app_base) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n",
           static_cast<unsigned long>(app_base));

    /* 跳转前的全部准备工作由移植层完成
     * （关中断、停 SysTick、复位 RCC、清 NVIC、设 VTOR、设 MSP、切特权级） */
    jump_to_app(app_base);

    /* 走到这里说明跳转失败 */
    BL_LOG("[boot] jump failed!\r\n");
}

} // namespace bl
