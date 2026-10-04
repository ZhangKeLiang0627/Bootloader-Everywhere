/**
 * @file    bl_meta.hpp
 * @brief   配置区（Flash 参数区）—— 固件状态机与原子提交的载体
 *
 * 存储方式：日志式槽位轮转
 *   把整个配置扇区切成 N 个 64 字节槽，每次状态变更「顺序追加」一个新槽，
 *   不擦除；读取时扫描取序号最大的有效槽。写满整个扇区后才擦除一次。
 *
 * 为什么这么做：常规状态写入只有 64 字节、耗时几十微秒。
 * 若采用「原地擦写」，每次上电记录启动计数都要擦 128KB 扇区
 * （约 1 秒延迟 + 一次擦写寿命消耗），完全不可接受。
 */
#ifndef BL_META_HPP
#define BL_META_HPP

#include "core/bl_types.hpp"
#include "bl_config.h"

namespace bl {

class Meta {
public:
    /// 槽位魔数 "LUMS"
    static constexpr uint32_t kSlotMagic = 0x4C554D53UL;

    /// 状态标志位
    static constexpr uint32_t kFlagUpdateReq = 0x00000001UL;

    /**
     * @brief 单个槽位的完整布局
     *
     * 字段顺序与大小一经发布不可随意改动，
     * 否则已部署的设备读不懂自己的配置区。
     */
    struct Slot {
        uint32_t seq;             ///< 自增序号，判定哪一槽最新
        FwState  state;           ///< 固件状态
        uint32_t fw_size;         ///< APP 有效字节数
        uint32_t fw_crc32;        ///< APP 整镜像 CRC32
        uint32_t fw_version;      ///< 固件版本号
        uint32_t boot_attempts;   ///< 连续启动尝试次数（回滚判据）
        uint32_t flags;           ///< kFlag*
        uint32_t magic;           ///< kSlotMagic
        uint32_t reserved[7];     ///< 预留，必须写 0（与上面 8 个字段合计 60 字节）
        uint32_t slot_crc32;      ///< 本槽前 60 字节的 CRC32
    };

    static_assert(sizeof(Slot) == BL_META_SLOT_SIZE,
                  "Slot 布局必须与配置区的槽位大小一致");

    /// 槽内参与 CRC 计算的字节数（末尾 4 字节 slot_crc32 自身不参与）
    static constexpr uint32_t kCrcSpan = sizeof(Slot) - sizeof(uint32_t);

    /* ---------------- 生命周期 ---------------- */

    /**
     * @brief 扫描配置区，装载当前状态
     *
     * 未找到任何有效槽时，内存状态置为 FwState::Invalid，
     * 但不立即写盘（避免把空配置区变成有内容的配置区）。
     */
    Status init() noexcept;

    /* ---------------- 查询 ---------------- */

    const Slot& current() const noexcept { return meta_; }

    bool should_boot() const noexcept { return bootable(meta_.state); }

    bool update_requested() const noexcept
    {
        return (meta_.flags & kFlagUpdateReq) != 0U;
    }

    FwState state() const noexcept { return meta_.state; }

    /* ---------------- 状态迁移 ---------------- */

    /**
     * @brief 标记「升级开始」，置 FwState::Download
     *
     * 必须在擦除 APP 区之前调用——这是防变砖的关键顺序：
     * 先让状态变成「不可信」，再去破坏 APP 区。
     * 顺序颠倒时，擦除中途掉电会留下
     * 「状态 Valid 但 APP 已是空片」的必砖组合。
     */
    Status mark_download() noexcept;

    /// 标记「传输并校验完成」，置 FwState::Testing
    Status commit(uint32_t size, uint32_t crc32, uint32_t version) noexcept;

    /**
     * @brief APP 侧调用：确认自身运行正常
     *
     * 置 FwState::Valid 并清零启动计数。只写一次，
     * 之后正常启动不再产生任何配置区写入。
     */
    Status confirm_app() noexcept;

    /// 标记固件作废（回滚后调用）
    Status revoke() noexcept;

    /// APP 侧调用：请求下次上电进入升级模式
    Status request_update() noexcept;

    /// 清除「请求升级」标志
    Status clear_update_request() noexcept;

    /// 擦除整个配置扇区（调试用）
    Status erase_all() noexcept;

    /**
     * @brief 启动计数递增
     *
     * 仅在 Testing 态有意义：已确认的固件正常启动不再产生写入。
     * @return 递增后的计数；若当前状态无需计数则返回 0
     */
    uint32_t bump_boot_attempts() noexcept;

    /* ---------------- 调试 ---------------- */

    void dump() const noexcept;

private:
    /// 追加写入一个新槽
    Status write_slot(const Slot& s) noexcept;

    Slot     meta_{};
    uint32_t next_offset_ = 0;
    uint32_t seq_         = 0;
    bool     inited_      = false;
};

/**
 * @brief 全局唯一的配置区实例
 *
 * 配置区在物理上只有一份，用访问函数而非全局对象，
 * 可避免静态初始化顺序问题。
 */
Meta& meta() noexcept;

/* ========================================================================
 * APP 侧便捷接口
 *
 * 下面两个是给「被引导的应用固件」用的，不是给 Bootloader 自己。
 * 做成头文件内联，APP 工程引用到就能用。
 *
 * 最简单的用法（APP 自己的 main 里）：
 *     #include "bl.hpp"
 *     ...
 *     if (self_test_ok()) {
 *         bl::app_confirm();      // 告诉 Bootloader「这次真的跑起来了」
 *     }
 *
 * 不调 app_confirm 的后果：固件停在 TESTING，启动计数每次 +1，
 * 到 BL_BOOT_MAX_ATTEMPTS 次后被判为「能过校验但跑不起来」并自动回滚。
 * ======================================================================*/

/// APP 自检通过时调用：状态转 VALID 并清零启动计数
inline Status app_confirm() noexcept
{
    return meta().confirm_app();
}

/// APP 请求下次上电进入升级模式
inline Status app_request_update() noexcept
{
    return meta().request_update();
}

} // namespace bl

#endif /* BL_META_HPP */
