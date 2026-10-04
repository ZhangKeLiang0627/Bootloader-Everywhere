/**
 * @file    bl_meta.cpp
 * @brief   配置区日志式槽位读写的实现
 */
#include <cstring>

#include "core/bl_meta.hpp"
#include "core/bl_crc.hpp"
#include "port/bl_port.hpp"
#include "core/bl_log.hpp"

namespace bl {

static bool is_erased(const void* p, uint32_t len) noexcept
{
    const auto* b = static_cast<const uint8_t*>(p);
    for (uint32_t i = 0; i < len; ++i) {
        if (b[i] != 0xFFU) return false;
    }
    return true;
}

static bool slot_valid(const Meta::Slot& s) noexcept
{
    return s.magic == Meta::kSlotMagic &&
           Crc32::compute(reinterpret_cast<const uint8_t*>(&s), Meta::kCrcSpan) == s.slot_crc32;
}

Status Meta::init() noexcept
{
    Slot     slot{};
    bool     found = false;
    uint32_t best_seq = 0;

    next_offset_ = 0;
    seq_         = 0;
    meta_        = Slot{};
    meta_.state  = FwState::Invalid;

    for (uint32_t off = 0; off + BL_META_SLOT_SIZE <= BL_META_SIZE; off += BL_META_SLOT_SIZE) {
        if (!ok(flash_read(BL_META_BASE + off, &slot, sizeof(slot)))) {
            return Status::FlashFail;
        }
        if (is_erased(&slot, sizeof(slot))) break;              // 顺序追加，遇空槽即止
        next_offset_ = off + BL_META_SLOT_SIZE;                  // 占位槽（含损坏的）都要跳过
        if (!slot_valid(slot)) continue;                         // 掉电残槽，跳过
        if (!found || slot.seq > best_seq) {
            meta_ = slot; best_seq = slot.seq; found = true;
        }
    }

    if (found) {
        seq_ = meta_.seq;
    } else {
        meta_        = Slot{};
        meta_.state  = FwState::Invalid;
        seq_         = 0;
        next_offset_ = 0;
    }

    inited_ = true;
    return Status::Ok;
}

Status Meta::write_slot(const Slot& s) noexcept
{
    if (!inited_) return Status::BadState;

    // 槽位用尽：整片擦除后从头开始
    if (next_offset_ + BL_META_SLOT_SIZE > BL_META_SIZE) {
        if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) return Status::FlashFail;
        next_offset_ = 0;
    }

    Slot slot = s;
    slot.seq    = ++seq_;
    slot.magic  = kSlotMagic;
    for (uint32_t& r : slot.reserved) r = 0;
    slot.slot_crc32 = Crc32::compute(reinterpret_cast<const uint8_t*>(&slot), kCrcSpan);

    const uint32_t off = next_offset_;
    if (!ok(flash_write(BL_META_BASE + off, &slot, sizeof(slot)))) return Status::FlashFail;

    // 回读校验，确认真的落盘
    Slot verify{};
    if (!ok(flash_read(BL_META_BASE + off, &verify, sizeof(verify)))) return Status::FlashFail;
    if (std::memcmp(&verify, &slot, sizeof(slot)) != 0) return Status::FlashFail;

    next_offset_ = off + BL_META_SLOT_SIZE;
    meta_        = slot;
    return Status::Ok;
}

Status Meta::mark_download() noexcept
{
    Slot s = meta_;
    s.state    = FwState::Download;
    s.fw_size  = 0;
    s.fw_crc32 = 0;
    return write_slot(s);
}

Status Meta::commit(uint32_t size, uint32_t crc32, uint32_t version) noexcept
{
    Slot s = meta_;
    s.state      = FwState::Valid;
    s.fw_size    = size;
    s.fw_crc32   = crc32;
    s.fw_version = version;
    return write_slot(s);
}

Status Meta::erase_all() noexcept
{
    if (!inited_) return Status::BadState;
    if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) return Status::FlashFail;
    meta_        = Slot{};
    meta_.state  = FwState::Invalid;
    next_offset_ = 0;
    seq_         = 0;
    return Status::Ok;
}

void Meta::dump() const noexcept
{
    BL_LOG("[meta] state=%s seq=%lu size=%lu crc=0x%08lX ver=%lu\r\n",
           to_string(meta_.state),
           static_cast<unsigned long>(meta_.seq),
           static_cast<unsigned long>(meta_.fw_size),
           static_cast<unsigned long>(meta_.fw_crc32),
           static_cast<unsigned long>(meta_.fw_version));
}

Meta& meta() noexcept
{
    static Meta inst;
    return inst;
}

} // namespace bl
