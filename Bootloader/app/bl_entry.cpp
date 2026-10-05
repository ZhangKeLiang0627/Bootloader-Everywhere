/**
 * @file    bl_entry.cpp
 * @brief   Bootloader 主流程 —— 库的入口实现
 *
 * 流程：平台初始化 → 自检 → 装载配置区 → 分区校验 → 启动决策
 *      → 跳转 APP，或留在 IAP 等 YMODEM。
 *
 * main() 默认由本文件提供（BL_PROVIDE_MAIN=1）。要接进已有工程就设 0，
 * 在自己的 main() 里调 bl::bl_entry()。
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
    switch (a) {
        case Boot::Action::JumpToApp:     return "JUMP";
        case Boot::Action::EnterIap:      return "IAP";
        case Boot::Action::EnterIapTimed: return "IAP_TIMED";
        default:                          return "?";
    }
}

const char* outcome_name(IapResult r) noexcept
{
    switch (r) {
        case IapResult::Idle:    return "IDLE";
        case IapResult::Done:    return "DONE";
        case IapResult::Failed:  return "FAILED";
        case IapResult::Aborted: return "ABORTED";
        case IapResult::NoSpace: return "NOSPACE";
        default:                 return "?";
    }
}

/// 无法继续时停在这里（Bootloader 本身永不被自己擦掉，停在原地等调试）
[[noreturn]] void fatal(const char* why) noexcept
{
    for (;;) {
        BL_LOG("[main] FATAL: %s\r\n", why);
        for (volatile uint32_t i = 0; i < 8000000U; ++i) {}
    }
}

/// 分区基址必须落在扇区边界（Flash 只能整扇区擦，差一个字节会毁邻近区）
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
               name, static_cast<unsigned long>(base), static_cast<unsigned long>(unit));
        return false;
    }
    return true;
}

bool layout_check() noexcept
{
    bool all = true;
    if (!partition_aligned("BOOT", BL_BOOT_BASE)) all = false;
    if (!partition_aligned("APP",  BL_APP_BASE))  all = false;
    if (!partition_aligned("META", BL_META_BASE)) all = false;
    return all;
}

void banner() noexcept
{
    BL_LOG("\r\n===== LUMOS-bootloader =====\r\n");
    BL_LOG("[main] chip  : %s\r\n", BL_CHIP_NAME);
    BL_LOG("[main] flash : 0x%08lX + %lu KB\r\n",
           static_cast<unsigned long>(BL_FLASH_BASE), static_cast<unsigned long>(BL_FLASH_SIZE / 1024U));
    BL_LOG("[main] boot  : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_BOOT_BASE), static_cast<unsigned long>(BL_BOOT_SIZE / 1024U));
    BL_LOG("[main] app   : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_APP_BASE), static_cast<unsigned long>(BL_APP_SIZE / 1024U));
    BL_LOG("[main] meta  : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_META_BASE), static_cast<unsigned long>(BL_META_SIZE / 1024U));
}

} // namespace

[[noreturn]] void bl_entry() noexcept
{
    if (!ok(platform_init())) fatal("platform init failed");
    if (!ok(uart_init(BL_UART_BAUDRATE))) fatal("uart init failed");
    if (!ok(flash_init())) fatal("flash init failed");
    if (!ok(crc_selftest())) fatal("crc selftest failed (check poly/init)");
    if (!ok(meta().init())) fatal("meta init failed");

    banner();
    meta().dump();

    // 分区校验：地址错了会毁邻近区，宁可停在报错也不进 IAP 做破坏性擦除
    if (!layout_check()) {
        BL_LOG("[main] FATAL: partition layout invalid\r\n");
        if (meta().should_boot()) {
            BL_LOG("[main] firmware state is bootable, attempting jump anyway\r\n");
            Boot::jump(BL_APP_BASE);
        }
        fatal("partition layout invalid");
    }

    const Boot::Decision decision = Boot::decide();
    BL_LOG("[main] decision: %s (%s)\r\n", action_name(decision.action), decision.reason);

    if (decision.action == Boot::Action::JumpToApp) {
        Boot::jump(BL_APP_BASE);
        BL_LOG("[main] jump failed, fallback to IAP\r\n");
    }

    // 软件复位唤回的限时窗口：窗口内没等到 YMODEM 首包就跳回 APP
    bool timed_window = (decision.action == Boot::Action::EnterIapTimed);

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
            // 升级成功，直接跳新固件（session 里已校验过向量表 + CRC）
            BL_LOG("[main] upgrade done, jumping...\r\n");
            delay_ms(300);
            Boot::jump(BL_APP_BASE);
            BL_LOG("[main] jump failed, keep IAP\r\n");
        }

        // 限时窗口超时（握手没等到首包）→ 跳回 APP
        if (timed_window &&
            r.outcome == IapResult::Failed &&
            r.error == Status::Timeout &&
            r.fw_size == 0U) {
            BL_LOG("[main] upgrade window timeout, jumping to app\r\n");
            Boot::jump(BL_APP_BASE);
            BL_LOG("[main] jump failed, keep IAP\r\n");
            timed_window = false;
        }

        delay_ms(200);
    }
}

} // namespace bl

#if BL_PROVIDE_MAIN
int main(void)
{
    bl::bl_entry();
}
#endif
