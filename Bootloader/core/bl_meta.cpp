/**
 * @file    bl_meta.cpp
 * @brief   配置区日志式槽位读写的实现
 */
#include <cstring>

#include "bl_meta.hpp"
#include "bl_crc.hpp"
#include "bl_port.hpp"

namespace bl {

/* ========================================================================
 * 内部工具
 * ======================================================================*/

/// 判断一段缓冲是否全为 0xFF（Flash 擦除态）
static bool is_erased(const void* p, uint32_t len) noexcept
{
    const auto* b = static_cast<const uint8_t*>(p);
    for (uint32_t i = 0; i < len; ++i) {
        if (b[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

/// 校验单个槽的完整性与魔数
static bool slot_valid(const Meta::Slot& s) noexcept
{
    if (s.magic != Meta::kSlotMagic) {
        return false;
    }
    return Crc32::compute(reinterpret_cast<const uint8_t*>(&s), Meta::kCrcSpan) == s.slot_crc32;
}

const char* state_name(FwState s) noexcept
{
    return to_string(s);
}

/* ========================================================================
 * 初始化：扫描配置区，装载最新有效槽
 * ======================================================================*/
Status Meta::init() noexcept
{
    Slot     slot{};
    bool     found = false;
    uint32_t best_seq = 0;

    next_offset_ = 0;
    seq_         = 0;
    meta_        = Slot{};
    meta_.state  = FwState::Invalid;

    for (uint32_t off = 0;
         off + BL_META_SLOT_SIZE <= BL_META_SIZE;
         off += BL_META_SLOT_SIZE) {

        if (!ok(flash_read(BL_META_BASE + off, &slot, sizeof(slot)))) {
            return Status::FlashFail;
        }

        /* 顺序追加写入，遇到第一个空槽即可停止扫描 */
        if (is_erased(&slot, sizeof(slot))) {
            break;
        }

        /* 本槽已被占用（无论是否损坏），下一个可写位置都要往后推 */
        next_offset_ = off + BL_META_SLOT_SIZE;

        if (!slot_valid(slot)) {
            continue;   /* 写入中途掉电留下的残槽，跳过 */
        }

        if (!found || slot.seq > best_seq) {
            meta_     = slot;
            best_seq  = slot.seq;
            found     = true;
        }
    }

    if (found) {
        seq_ = meta_.seq;
    } else {
        /* 配置区为空或全部损坏：视为无有效固件 */
        meta_        = Slot{};
        meta_.state  = FwState::Invalid;
        seq_         = 0;
        next_offset_ = 0;
    }

    inited_ = true;
    return Status::Ok;
}

/* ========================================================================
 * 追加写入一个新槽
 * ======================================================================*/
Status Meta::write_slot(const Slot& s) noexcept
{
    if (!inited_) {
        return Status::BadState;
    }

    /* 槽位用尽：擦除整个配置扇区后从头开始。
     * 擦除期间若掉电，配置区全 0xFF 会被判为 Invalid，
     * 结果是「停在 IAP 等待重刷」——可用性下降，但不是变砖。 */
    if (next_offset_ + BL_META_SLOT_SIZE > BL_META_SIZE) {
        if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) {
            return Status::FlashFail;
        }
        next_offset_ = 0;
    }

    Slot slot = s;
    slot.seq        = ++seq_;
    slot.magic      = kSlotMagic;
    slot.slot_crc32 = Crc32::compute(reinterpret_cast<const uint8_t*>(&slot), kCrcSpan);
    /* reserved 恒为 0，保证同一状态写入的内容可重现 */
    for (uint32_t& r : slot.reserved) {
        r = 0;
    }

    const uint32_t off = next_offset_;
    if (!ok(flash_write(BL_META_BASE + off, &slot, sizeof(slot)))) {
        return Status::FlashFail;
    }

    /* 回读校验：Flash 编程失败不易被察觉，必须确认真的落盘 */
    Slot verify{};
    if (!ok(flash_read(BL_META_BASE + off, &verify, sizeof(verify)))) {
        return Status::FlashFail;
    }
    if (std::memcmp(&verify, &slot, sizeof(slot)) != 0) {
        return Status::FlashFail;
    }

    next_offset_ = off + BL_META_SLOT_SIZE;
    meta_        = slot;
    return Status::Ok;
}

/* ========================================================================
 * 状态迁移
 * ======================================================================*/
Status Meta::mark_download() noexcept
{
    Slot s         = meta_;
    s.state        = FwState::Download;
    s.fw_size      = 0;
    s.fw_crc32     = 0;
    s.boot_attempts = 0;
    s.flags       &= ~kFlagUpdateReq;

    return write_slot(s);
}

Status Meta::commit(uint32_t size, uint32_t crc32, uint32_t version) noexcept
{
    Slot s          = meta_;
    s.state         = FwState::Testing;   /* 先试运行，等 APP 自检确认 */
    s.fw_size       = size;
    s.fw_crc32      = crc32;
    s.fw_version    = version;
    s.boot_attempts = 0;

    return write_slot(s);
}

Status Meta::confirm_app() noexcept
{
    Slot s = meta_;

    if (s.state != FwState::Testing && s.state != FwState::Valid) {
        return Status::BadState;
    }
    /* 已确认且无待清计数 → 无需重复写入，避免无谓的 Flash 消耗 */
    if (s.state == FwState::Valid && s.boot_attempts == 0) {
        return Status::Ok;
    }

    s.state         = FwState::Valid;
    s.boot_attempts = 0;

    return write_slot(s);
}

Status Meta::revoke() noexcept
{
    Slot s   = meta_;
    s.state  = FwState::Revoked;
    return write_slot(s);
}

Status Meta::request_update() noexcept
{
    Slot s  = meta_;
    s.flags |= kFlagUpdateReq;
    return write_slot(s);
}

Status Meta::clear_update_request() noexcept
{
    if ((meta_.flags & kFlagUpdateReq) == 0U) {
        return Status::Ok;
    }
    Slot s  = meta_;
    s.flags &= ~kFlagUpdateReq;
    return write_slot(s);
}

Status Meta::erase_all() noexcept
{
    if (!inited_) {
        return Status::BadState;
    }
    if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) {
        return Status::FlashFail;
    }
    meta_        = Slot{};
    meta_.state  = FwState::Invalid;
    next_offset_ = 0;
    seq_         = 0;
    return Status::Ok;
}

/* ========================================================================
 * 启动计数
 * ======================================================================*/
uint32_t Meta::bump_boot_attempts() noexcept
{
    /* 仅试运行态需要计数；已确认的固件正常启动不再产生任何写入 */
    if (meta_.state != FwState::Testing) {
        return 0;
    }

    Slot s = meta_;
    ++s.boot_attempts;

    if (!ok(write_slot(s))) {
        return s.boot_attempts;
    }
    return meta_.boot_attempts;
}

/* ========================================================================
 * 调试输出
 * ======================================================================*/
void Meta::dump() const noexcept
{
    BL_LOG("[meta] state=%s seq=%lu size=%lu crc=0x%08lX ver=%lu attempts=%lu flags=0x%lX\r\n",
           state_name(meta_.state),
           static_cast<unsigned long>(meta_.seq),
           static_cast<unsigned long>(meta_.fw_size),
           static_cast<unsigned long>(meta_.fw_crc32),
           static_cast<unsigned long>(meta_.fw_version),
           static_cast<unsigned long>(meta_.boot_attempts),
           static_cast<unsigned long>(meta_.flags));
}

/* ========================================================================
 * 全局实例
 * ======================================================================*/
Meta& meta() noexcept
{
    static Meta inst;
    return inst;
}

} // namespace bl
