/**
 * @file    bl_meta.hpp
 * @brief   配置区（Flash 参数区）—— 固件状态与原子提交的载体
 *
 * 日志式槽位轮转：把配置扇区切成 N 个 64 字节槽，状态变更顺序追加，
 * 读时取序号最大的有效槽，写满才擦一次。这样常规状态写入只有几十微秒，
 * 不用每次擦 128KB 扇区。
 */
#ifndef BL_META_HPP
#define BL_META_HPP

#include "core/bl_types.hpp"
#include "bl_config.h"

namespace bl {

class Meta {
public:
    static constexpr uint32_t kSlotMagic = 0x4C554D53UL;  ///< "LUMS"

    /// 单个槽位布局。字段顺序一经发布不可改，否则旧设备读不懂配置区
    struct Slot {
        uint32_t seq;             ///< 自增序号，判定哪槽最新
        FwState  state;           ///< 固件状态
        uint32_t fw_size;         ///< APP 有效字节数
        uint32_t fw_crc32;        ///< APP 整镜像 CRC32
        uint32_t fw_version;      ///< 固件版本号
        uint32_t magic;           ///< kSlotMagic
        uint32_t reserved[9];     ///< 预留，恒为 0
        uint32_t slot_crc32;      ///< 本槽前 60 字节的 CRC32
    };

    static_assert(sizeof(Slot) == BL_META_SLOT_SIZE,
                  "Slot 布局必须与槽位大小一致");

    static constexpr uint32_t kCrcSpan = sizeof(Slot) - sizeof(uint32_t);

    /// 扫描配置区装载当前状态；无有效槽时置 Invalid（不写盘）
    Status init() noexcept;

    const Slot& current() const noexcept { return meta_; }
    bool should_boot() const noexcept { return bootable(meta_.state); }
    FwState state() const noexcept { return meta_.state; }

    /// 置 Download，必须在擦除 APP 区前调用（防「Valid 但已擦空」的必砖组合）
    Status mark_download() noexcept;

    /// 升级完成，置 Valid 并记录 size/crc32/version
    Status commit(uint32_t size, uint32_t crc32, uint32_t version) noexcept;

    /// 擦除整个配置扇区（调试用）
    Status erase_all() noexcept;

    void dump() const noexcept;

private:
    Status write_slot(const Slot& s) noexcept;

    Slot     meta_{};
    uint32_t next_offset_ = 0;
    uint32_t seq_         = 0;
    bool     inited_      = false;
};

/// 全局唯一的配置区实例
Meta& meta() noexcept;

} // namespace bl

#endif /* BL_META_HPP */
