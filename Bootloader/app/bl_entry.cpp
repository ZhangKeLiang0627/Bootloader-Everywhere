/**
 * @file    bl_entry.cpp
 * @brief   Bootloader 主流程 —— 库的入口实现
 *
 * 上线流程：
 *   平台初始化 → 自检 → 装载配置区 → 分区布局校验 → 启动决策
 *   → 跳转 APP，或者留在 IAP 等 YMODEM 传输
 *
 * 关于 main()：默认由本文件提供（BL_PROVIDE_MAIN=1）。
 * 如果你要把库接进一个已有工程，把 BL_PROVIDE_MAIN 设为 0，
 * 然后在自己的 main() 里调用 bl::bl_entry() 即可 —— 库不会碰你的入口。
 */
#include "bl_config.h"          /* 必须最先：芯片参数 */

#include "app/bl_entry.hpp"

#include "core/bl_boot.hpp"
#include "core/bl_crc.hpp"
#include "core/bl_log.hpp"
#include "core/bl_meta.hpp"
#include "core/bl_session.hpp"
#include "core/bl_verify.hpp"
#include "port/bl_port.hpp"

namespace bl {
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

/**
 * @brief 出现无法继续的错误时停在这里并周期性输出
 *
 * 有意不喂狗：若启用了看门狗，让它复位重来；没启用就停在原地等调试器。
 * 两种情况设备都能救回来 —— Bootloader 本身永远不会被自己擦掉。
 */
[[noreturn]] void fatal(const char* why) noexcept
{
    for (;;) {
        BL_LOG("[main] FATAL: %s\r\n", why);
        for (volatile uint32_t i = 0; i < 8000000U; ++i) {
        }
    }
}

/**
 * @brief 校验某个分区基址是否正好落在扇区（页）边界上
 *
 * 这是移植时最容易犯的错：地址差几百字节，平时看不出来，
 * 一旦擦除就会连带擦掉相邻区域。宁可上电就报警，也不要等到升级时才发现。
 */
bool partition_aligned(const char* name, uint32_t base) noexcept
{
    const uint32_t unit = flash_sector_size(base);
    if (unit == 0U) {
        BL_LOG("[cfg] %s base 0x%08lX is outside flash!\r\n",
               name, static_cast<unsigned long>(base));
        return false;
    }
    if (flash_bytes_to_sector_end(base) != unit) {
        BL_LOG("[cfg] %s base 0x%08lX is not on a sector boundary (sector=%lu B)\r\n",
               name, static_cast<unsigned long>(base),
               static_cast<unsigned long>(unit));
        return false;
    }
    return true;
}

/** 三个分区基址都要落在扇区边界上 */
bool layout_check() noexcept
{
    bool all = true;
    if (!partition_aligned("BOOT", BL_BOOT_BASE)) { all = false; }
    if (!partition_aligned("APP",  BL_APP_BASE))  { all = false; }
    if (!partition_aligned("META", BL_META_BASE)) { all = false; }
    return all;
}

void banner() noexcept
{
    BL_LOG("\r\n===== LUMOS-bootloader =====\r\n");
    BL_LOG("[main] chip  : %s\r\n", BL_CHIP_NAME);
    BL_LOG("[main] flash : 0x%08lX + %lu KB\r\n",
           static_cast<unsigned long>(BL_FLASH_BASE),
           static_cast<unsigned long>(BL_FLASH_SIZE / 1024U));
    BL_LOG("[main] boot  : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_BOOT_BASE),
           static_cast<unsigned long>(BL_BOOT_SIZE / 1024U));
    BL_LOG("[main] app   : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_APP_BASE),
           static_cast<unsigned long>(BL_APP_SIZE / 1024U));
    BL_LOG("[main] meta  : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_META_BASE),
           static_cast<unsigned long>(BL_META_SIZE / 1024U));
}

} // namespace

/* ========================================================================
 * 入口
 * ======================================================================*/
[[noreturn]] void bl_entry() noexcept
{
    /* ---- 1. 平台初始化（时钟 / 时基 / HAL 底座） ---- */
    if (!ok(platform_init())) {
        fatal("platform init failed");
    }

    /* ---- 2. 串口：确保波特率与约定一致 ---- */
    if (!ok(uart_init(BL_UART_BAUDRATE))) {
        fatal("uart init failed");
    }

    /* ---- 3. Flash 接口 ---- */
    if (!ok(flash_init())) {
        fatal("flash init failed");
    }

    /* ---- 4. CRC 自检 ----
     * 参数写错的现象是「PC 算的值和板子算的对不上」，极难排查，
     * 所以用标准测试向量在最早的时机把它暴露出来。 */
    if (!ok(crc_selftest())) {
        fatal("crc selftest failed (check poly/init)");
    }

    /* ---- 5. 装载配置区 ---- */
    if (!ok(meta().init())) {
        fatal("meta init failed");
    }

#if BL_USE_WATCHDOG
    wdg_init();
#endif

    banner();
    meta().dump();

    /* ---- 6. 分区布局校验 ----
     * 不通过就绝不进 IAP：擦除是按分区的地址算的，地址错了会毁掉邻近区域。
     * 但若固件本身可启动，仍然放它跑 —— 读操作永远是安全的。 */
    if (!layout_check()) {
        BL_LOG("[main] FATAL: partition layout invalid\r\n");
        if (meta().should_boot()) {
            BL_LOG("[main] firmware state is bootable, attempting jump anyway\r\n");
            Boot::jump(BL_APP_BASE);
        }
        fatal("partition layout invalid");
    }

    /* ---- 7. 启动决策 ---- */
    const Boot::Decision decision = Boot::decide();
    BL_LOG("[main] decision: %s (%s)\r\n",
           action_name(decision.action), decision.reason);

    if (decision.action == Boot::Action::JumpToApp) {
        Boot::jump(BL_APP_BASE);
        /* 跳转失败才走到这里，降级进入 IAP */
        BL_LOG("[main] jump failed, fallback to IAP\r\n");
    }

    /* 软件复位唤回的限时窗口：只给上位机一个有限窗口（15s），
     * 窗口内没等到 YMODEM 首包就跳回 APP。正常 EnterIap（固件不可启动、
     * 回滚等）则必须无限等待，否则会反复跳进坏固件。 */
    bool timed_window = (decision.action == Boot::Action::EnterIapTimed);

    /* ---- 8. IAP 循环 ----
     * 每轮处理一次升级。失败不退出，继续等下一次尝试 ——
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

        /* 限时窗口超时（握手没等到首包）→ 跳回 APP。
         * 判据：握手阶段超时（Timeout）且从未收到首包（fw_size==0）。 */
        if (timed_window &&
            r.outcome == IapResult::Failed &&
            r.error == Status::Timeout &&
            r.fw_size == 0U) {
            BL_LOG("[main] upgrade window timeout, jumping to app\r\n");
            Boot::jump(BL_APP_BASE);
            /* 跳转失败说明 APP 也不可用，转为无限等待 IAP，不再反复尝试跳转 */
            BL_LOG("[main] jump failed, keep IAP\r\n");
            timed_window = false;
        }

        /* 失败：短暂停顿后重新进入等待，避免串口刷屏 */
        delay_ms(200);
    }
}

} // namespace bl

/* ========================================================================
 * 可选：库自带的 main
 *
 * 想把它接进已有工程时，把 bl_config.h 里的 BL_PROVIDE_MAIN 设为 0，
 * 然后在自己的 main() 里调 bl::bl_entry()。
 * ======================================================================*/
#if BL_PROVIDE_MAIN
int main(void)
{
    bl::bl_entry();
}
#endif
