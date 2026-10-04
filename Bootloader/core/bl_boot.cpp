/**
 * @file    bl_boot.cpp
 * @brief   启动决策的实现
 */
#include "bl_boot.hpp"
#include "bl_meta.hpp"
#include "bl_verify.hpp"
#include "bl_port.hpp"
#include "bl_log.hpp"

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

    /* ---- 4. Testing 态的启动计数判据 ---- */
    if (m.state() == FwState::Testing) {
        const uint32_t attempts = m.bump_boot_attempts();

        BL_LOG("[boot] testing: attempt %lu/%lu\r\n",
               static_cast<unsigned long>(attempts),
               static_cast<unsigned long>(cfg.max_attempts));

        if (attempts > cfg.max_attempts) {
            /* 连续多次都没等到 APP 自检确认，判定该固件有运行时缺陷。
             * 作废后停在 IAP，等待重刷——这就是「回滚」的实际形态。 */
            BL_LOG("[boot] rollback: app never confirmed\r\n");
            (void)m.revoke();
            return { Action::EnterIap, "app not confirmed, revoked" };
        }
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
