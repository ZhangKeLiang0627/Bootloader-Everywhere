/**
 * @file    bl_boot.hpp
 * @brief   上电启动决策 —— 决定「跳转 APP」还是「留在 IAP」
 *
 * 判定顺序（顺序本身也是安全设计的一部分）：
 *
 *   1. APP 是否请求过升级（配置区）？  → 是则清标志并进 IAP
 *   2. 固件状态是否可跳转？            → 否（Invalid/Download/Revoked）则进 IAP
 *   3. 软件复位 + 状态 Valid？         → 进限时升级窗口（APP 唤回主通道）
 *   4. Backdoor 时间窗内是否有触发？   → 有则进 IAP（默认关闭，向后兼容）
 *   5. 处于 Testing 态？               → 按复位原因判定，见下
 *   6. 向量表是否合法？                → 否则作废并进 IAP
 *   7. （可选）整镜像 CRC32 是否匹配？ → 否则作废并进 IAP
 *   8. 全部通过                        → 跳转 APP
 *
 * 第 3 步是「正常 APP 回 IAP」的主通道（取代旧的 RAM 标志方案）：APP 检测
 * 到升级指令后只做软件复位，Bootloader 靠复位原因识别「软件复位」进入一个
 * 限时窗口 —— 与 OpenBLT 的软复位后门同源。窗口仅对「软件复位 + Valid 态」
 * 开启：正常上电 / 硬件复位零等待直接跳 APP。
 *
 * 为什么限定「状态 == Valid」：升级完成后 Bootloader 自己也会软件复位
 * （bl_entry 的 system_reset），此时状态是 Testing。若一并进窗口，升级后
 * 会永远跳不进新固件。用状态区分两类软件复位：Testing = 升级完成复位，
 * Valid = APP 运行中唤回。
 *
 * 第 5 步是「自确认」机制所在（开关 BL_BOOT_SELF_CONFIRM）。它不要求
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
        JumpToApp,     ///< 跳转应用
        EnterIap,      ///< 留在 Bootloader 等待升级（无限等待）
        EnterIapTimed, ///< 软件复位唤回的限时窗口：等上位机，超时跳回 APP
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
