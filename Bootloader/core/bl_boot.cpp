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

    /* 复位原因必须在**每次启动都读一次** —— 这是 read-and-clear 语义：
     * 硬件把这些标志累积着，只有读了才会清掉。
     *
     * 踩过的坑：最初只在 Testing 分支里读。于是状态为 Valid 的那些启动
     * 从不清标志，POR / PIN 之类的旧原因一直积着；等到下次升级、首次
     * 启动时读到的是陈年旧账，被误判成 clean boot 直接转成 Valid ——
     * 结果是回滚机制永不触发，坏固件被无限重启。
     * （实测症状：故障固件每 6 秒被看门狗复位一次，但 state 始终是
     *  VALID、attempts 始终为 0。） */
    const ResetCause cause = reset_cause();
    (void)cause;        /* BL_BOOT_SELF_CONFIRM = 0 时用不到它，但必须读 */
    BL_LOG("[boot] reset cause = %lu (0=unk 1=por 2=pin 3=sft 4=wdg)\r\n",
           static_cast<unsigned long>(cause));

    /* ---- 1. APP 是否请求过升级（配置区持久化标志） ---- */
    if (m.update_requested()) {
        (void)m.clear_update_request();
        return { Action::EnterIap, "update requested by app" };
    }

    /* ---- 2. 状态是否可跳转 ---- */
    if (!m.should_boot()) {
        return { Action::EnterIap, "no bootable firmware" };
    }

    /* ---- 3. 软件复位唤回（限时升级窗口） ----
     *
     * APP 检测到升级指令后软复位（不写任何标志），Bootloader 靠复位原因
     * 识别：软件复位 + 固件处于 Valid 态 = APP 运行中被唤回，进限时窗口。
     *
     * 为什么限定 Valid：升级完成后 Bootloader 自己也会软复位，那时状态是
     * Testing，不能一并进窗口（否则升级后永远跳不进新固件）。Testing 态的
     * 软复位落到下面第 5 步的自确认逻辑，照常跳 APP。 */
    if (cause == ResetCause::Software && m.state() == FwState::Valid) {
        return { Action::EnterIapTimed, "soft reset -> upgrade window" };
    }

    /* ---- 4. Backdoor 窗口 ---- */
    if (wait_backdoor(cfg.backdoor_window_ms, cfg.backdoor_char)) {
        return { Action::EnterIap, "backdoor" };
    }

    /* ---- 5. Testing 态的判据（「自确认」机制的核心） ----
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
        /* 判据分两层，先看「跑过没有」，再看「是怎么复位的」。
         *
         * 为什么不能只靠复位原因：「首次启动」和「用户按复位」这两件事
         * 在复位原因上区分不开 —— 实测发现升级完成后的
         * NVIC_SystemReset() 会被在线探针连带拖出一次 PIN 复位，
         * 于是本该是 sft 的那次报成了 pin，逻辑就走到错误的支路去了。
         *
         * 而 commit() 会把 boot_attempts 清成 0，所以
         *   attempts == 0  ⇔  新固件刚写入、一次都还没跑过
         * 这个判据不依赖复位原因的准确性，稳得多。 */
        const uint32_t prev = m.current().boot_attempts;

        if (prev == 0U) {
            /* 升级后的第一次运行：开始计时，保持 Testing */
            (void)m.bump_boot_attempts();
            BL_LOG("[boot] fresh upgrade: first run, keep TESTING\r\n");
        } else if (cause == ResetCause::Watchdog) {
            /* APP 没能活过看门狗超时 —— 「跑不起来」的客观证据 */
            const uint32_t attempts = m.bump_boot_attempts();
            BL_LOG("[boot] watchdog reset: app did not survive (%lu/%lu)\r\n",
                   static_cast<unsigned long>(attempts),
                   static_cast<unsigned long>(cfg.max_attempts));

            if (attempts > cfg.max_attempts) {
                BL_LOG("[boot] rollback: app keeps failing after upgrade\r\n");
                (void)m.revoke();
                return { Action::EnterIap, "app keeps crashing (watchdog), revoked" };
            }
        } else if (prev <= 1U) {
            /* 跑过、且没被看门狗拉回来过 → 可判定它是健康的。
             *
             * 上限 1 是为了防「洗白」：一个反复跑飞的固件 attempts 会 >= 2，
             * 那时即使用户断电上电也一概不认，否则回滚机制就永远触发不了。 */
            BL_LOG("[boot] clean boot -> firmware self-confirmed\r\n");
            (void)m.confirm_app();
        } else {
            BL_LOG("[boot] clean boot, but watchdog failures on record\r\n");
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

    /* ---- 6. 向量表校验（廉价，每次必做） ---- */
    if (!ok(verify_vector_table(cfg.app_base, nullptr))) {
        BL_LOG("[boot] invalid vector table\r\n");
        (void)m.revoke();
        return { Action::EnterIap, "vector table invalid" };
    }

    /* ---- 7. 可选：整镜像 CRC32 ---- */
    if (cfg.verify_crc_on_boot) {
        const Meta::Slot& s = m.current();
        if (!ok(verify_image(cfg.app_base, s.fw_size, s.fw_crc32, nullptr))) {
            BL_LOG("[boot] image crc mismatch\r\n");
            (void)m.revoke();
            return { Action::EnterIap, "image crc mismatch" };
        }
    }

    /* ---- 8. 全部通过 ---- */
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
