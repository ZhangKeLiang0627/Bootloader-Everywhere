/**
 * @file    bl_boot.hpp
 * @brief   启动决策 —— 决定「跳转 APP」还是「留在 IAP」
 *
 * 判定顺序（顺序是安全设计的一部分）：
 *   1. 固件状态非 Valid？       → 进 IAP
 *   2. 软件复位 + Valid？       → 进限时升级窗口（APP 唤回主通道）
 *   3. 向量表非法？             → 作废进 IAP
 *   4. 整镜像 CRC32 不匹配？    → 作废进 IAP
 *   5. 全部通过                 → 跳转 APP
 */
#ifndef BL_BOOT_HPP
#define BL_BOOT_HPP

#include "core/bl_types.hpp"
#include "bl_config.h"

namespace bl {

class Boot {
public:
    enum class Action : uint8_t {
        JumpToApp,     ///< 跳转应用
        EnterIap,      ///< 留在 IAP 等待升级（无限等待）
        EnterIapTimed, ///< 软件复位唤回的限时窗口：等上位机，超时跳回 APP
    };

    struct Config {
        bool     verify_crc_on_boot = (BL_BOOT_VERIFY_CRC32 != 0);
        uint32_t app_base           = BL_APP_BASE;
    };

    struct Decision {
        Action      action = Action::EnterIap;
        const char* reason = "";

        Decision() = default;
        constexpr Decision(Action a, const char* r) noexcept : action(a), reason(r) {}
    };

    static Decision decide() noexcept;
    static Decision decide(const Config& cfg) noexcept;

    /// 跳转到 APP（内部调用 port 层，正常不返回）
    static void jump(uint32_t app_base) noexcept;
};

} // namespace bl

#endif /* BL_BOOT_HPP */
