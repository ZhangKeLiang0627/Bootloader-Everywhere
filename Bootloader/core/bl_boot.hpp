/**
 * @file    bl_boot.hpp
 * @brief   上电启动决策 —— 决定「跳转 APP」还是「留在 IAP」
 *
 * 判定顺序（顺序本身也是安全设计的一部分）：
 *
 *   1. APP 是否请求过升级？         → 是则清标志并进 IAP
 *   2. 固件状态是否可跳转？         → 否（Invalid/Download/Revoked）则进 IAP
 *   3. Backdoor 时间窗内是否有触发？→ 有则进 IAP（强制升级通道）
 *   4. 处于 Testing 态且启动次数超限？ → 判定该固件有运行时缺陷，作废并进 IAP
 *   5. 向量表是否合法？             → 否则作废并进 IAP
 *   6. （可选）整镜像 CRC32 是否匹配？ → 否则作废并进 IAP
 *   7. 全部通过                     → 跳转 APP
 *
 * 注意第 3 步在第 4 步之前：用户主动要求升级时不应消耗启动计数。
 */
#ifndef BL_BOOT_HPP
#define BL_BOOT_HPP

#include "bl_types.hpp"
#include "bl_config.h"

namespace bl {

class Boot {
public:
    enum class Action : uint8_t {
        JumpToApp,   ///< 跳转应用
        EnterIap,    ///< 留在 Bootloader 等待升级
    };

    struct Config {
        uint32_t backdoor_window_ms = BL_BACKDOOR_WINDOW_MS;
        uint8_t  backdoor_char      = BL_BACKDOOR_CHAR;
        uint32_t max_attempts       = BL_BOOT_MAX_ATTEMPTS;
        bool     verify_crc_on_boot = (BL_BOOT_VERIFY_CRC32 != 0);
        uint32_t app_base           = BL_APP_BASE;
    };

    struct Decision {
        Action      action = Action::EnterIap;
        const char* reason = "";
    };

    /**
     * @brief 做出启动决策
     *
     * 内部会读写配置区（递增启动计数、作废固件等）。
     * 调用前应先完成 Meta::init()。
     *
     * 拆成两个重载而非使用默认实参 Config{}：参见 bl_ymodem.hpp 中的说明
     * （类的默认实参不在 complete-class context 中）。
     */
    static Decision decide() noexcept;
    static Decision decide(const Config& cfg) noexcept;

    /**
     * @brief 在时间窗内监听 Backdoor 字符
     * @return true 表示收到触发字符，用户要求进入 IAP
     */
    static bool wait_backdoor(uint32_t window_ms, uint8_t trigger) noexcept;

    /// 跳转到 APP（内部调用 port 层，正常情况下不返回）
    static void jump(uint32_t app_base) noexcept;
};

} // namespace bl

#endif /* BL_BOOT_HPP */
