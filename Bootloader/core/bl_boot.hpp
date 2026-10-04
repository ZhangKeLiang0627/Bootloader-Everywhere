/**
 * @file    bl_boot.hpp
 * @brief   上电启动决策 —— 决定「跳转 APP」还是「留在 IAP」
 *
 * 判定顺序（顺序本身也是安全设计的一部分）：
 *
 *   0. RAM 标志（APP 软复位请求）？→ 是则清标志并进 IAP（正常 APP 唤回的主通道）
 *   1. APP 是否请求过升级？         → 是则清标志并进 IAP
 *   2. 固件状态是否可跳转？         → 否（Invalid/Download/Revoked）则进 IAP
 *   3. Backdoor 时间窗内是否有触发？→ 有则进 IAP（默认关闭，向后兼容）
 *   4. 处于 Testing 态？            → 按复位原因判定，见下
 *   5. 向量表是否合法？             → 否则作废并进 IAP
 *   6. （可选）整镜像 CRC32 是否匹配？ → 否则作废并进 IAP
 *   7. 全部通过                     → 跳转 APP
 *
 * 第 0 步是「正常 APP 回 IAP」的主通道：APP 调 bl_app.h 的 bl_request_update()
 * 写 RAM 标志后软复位，Bootloader 上电即看到、零等待。它取代了旧的
 * backdoor 时间窗（那种要抢上电几百毫秒、还拖慢每次启动）。
 *
 * 第 4 步是「自确认」机制所在（开关 BL_BOOT_SELF_CONFIRM）。它不要求
 * APP 主动调用任何接口，而是读复位原因来客观判断：
 *
 *   - 看门狗复位 → APP 没喂狗，说明它跑不起来 → 计数 +1，超限作废
 *   - 上电/按复位 → APP 上次活下来了 → 自动转 Valid
 *
 * 好处是 APP 侧零侵入：不必包含本库头文件，也不必知道配置区格式，
 * 只需做它本来就该做的事 —— 喂狗。
 */
#ifndef BL_BOOT_HPP
#define BL_BOOT_HPP

#include "core/bl_types.hpp"
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

        /* 显式提供构造函数，而不是靠聚合初始化 {a, b}：
         * C++11 里「带默认成员初始化器的结构体」不算聚合类型，
         * 花括号初始化会编译不过。本库保持 C++11 可编译。 */
        Decision() = default;
        constexpr Decision(Action a, const char* r) noexcept
            : action(a), reason(r) {}
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
