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

Boot::Decision Boot::decide() noexcept
{
    return decide(Config{});
}

Boot::Decision Boot::decide(const Config& cfg) noexcept
{
    Meta& m = meta();

    // 复位原因必须每次启动都读（read-and-clear），否则旧标志会累积到下次启动
    const ResetCause cause = reset_cause();
    BL_LOG("[boot] reset cause = %lu (0=unk 1=por 2=pin 3=sft)\r\n",
           static_cast<unsigned long>(cause));

    // 1. 固件状态不可跳转 → 进 IAP
    if (!m.should_boot()) {
        return { Action::EnterIap, "no bootable firmware" };
    }

    // 2. 软件复位唤回：APP 运行中软复位，进限时升级窗口
    if (cause == ResetCause::Software) {
        return { Action::EnterIapTimed, "soft reset -> upgrade window" };
    }

    // 3. 向量表校验
    if (!ok(verify_vector_table(cfg.app_base, nullptr))) {
        return { Action::EnterIap, "invalid vector table" };
    }

    // 4. 整镜像 CRC32 校验（可选）
    if (cfg.verify_crc_on_boot) {
        const Meta::Slot& s = m.current();
        if (!ok(verify_image(cfg.app_base, s.fw_size, s.fw_crc32, nullptr))) {
            return { Action::EnterIap, "image crc mismatch" };
        }
    }

    // 5. 全部通过 → 跳 APP
    return { Action::JumpToApp, "ok" };
}

void Boot::jump(uint32_t app_base) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n",
           static_cast<unsigned long>(app_base));
    jump_to_app(app_base);
    BL_LOG("[boot] jump failed!\r\n");
}

} // namespace bl
