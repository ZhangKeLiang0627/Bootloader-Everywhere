// 串口帧协议（载体层）实现。见 protocol.h

#include "protocol.h"

namespace proto {

uint8_t crc8Step(uint8_t crc, uint8_t byte) noexcept
{
    crc ^= byte;
    for (uint32_t i = 0U; i < 8U; ++i) {
        crc = (crc & 0x80U) ? static_cast<uint8_t>((crc << 1) ^ 0x07U)
                            : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

uint8_t crc8(const void* data, uint32_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint8_t c = 0U;

    while (len-- > 0U) {
        c = crc8Step(c, *p++);
    }
    return c;
}

uint32_t encode(uint8_t id, uint8_t cmd, const void* data, uint16_t len,
                uint8_t* out, uint32_t outCap) noexcept
{
    if (out == nullptr || len > kDataMax || outCap < (kOverhead + len)) {
        return 0U;
    }
    if (len > 0U && data == nullptr) {
        return 0U;
    }

    out[0] = kHead;
    out[1] = id;
    out[2] = cmd;
    putLe16(&out[3], len);

    const auto* src = static_cast<const uint8_t*>(data);
    for (uint16_t i = 0U; i < len; ++i) {
        out[5U + i] = src[i];
    }

    out[5U + len] = crc8(&out[1], 4U + len);   // 覆盖 [ID .. Data 末尾]
    out[6U + len] = kTail;
    return kOverhead + len;
}

void Parser::reset() noexcept
{
    state_ = State::Head;
    crc_   = 0U;
    index_ = 0U;
}

bool Parser::feed(uint8_t byte, Frame& out) noexcept
{
    // 每个字节最多被判两次（一次按原状态，一次作为可能的新帧头），guard 是兜底
    for (uint32_t guard = 0U; guard < 3U; ++guard) {
        switch (state_) {
        case State::Head:
            if (byte != kHead) {
                return false;
            }
            crc_   = 0U;
            index_ = 0U;
            state_ = State::Id;
            return false;

        case State::Id:
            out.id = byte;
            crc_   = crc8Step(crc_, byte);
            state_ = State::Cmd;
            return false;

        case State::Cmd:
            out.cmd = byte;
            crc_    = crc8Step(crc_, byte);
            state_  = State::LenLo;
            return false;

        case State::LenLo:
            out.len = byte;
            crc_    = crc8Step(crc_, byte);
            state_  = State::LenHi;
            return false;

        case State::LenHi:
            out.len = static_cast<uint16_t>(out.len |
                                            (static_cast<uint16_t>(byte) << 8));
            crc_    = crc8Step(crc_, byte);
            if (out.len > kDataMax) {
                state_ = State::Head;      // 长度非法：丢弃整帧
                continue;                  // 本字节可能就是新帧头
            }
            state_ = (out.len == 0U) ? State::Crc : State::Data;
            return false;

        case State::Data:
            out.data[index_++] = byte;
            crc_ = crc8Step(crc_, byte);
            if (index_ >= out.len) {
                state_ = State::Crc;
            }
            return false;

        case State::Crc:
            if (byte != crc_) {
                state_ = State::Head;
                continue;                  // CRC 错：整帧丢弃，重新找帧头
            }
            state_ = State::Tail;
            return false;

        case State::Tail:
            state_ = State::Head;
            if (byte != kTail) {
                continue;                  // 帧尾错：整帧丢弃，重新找帧头
            }
            // 帧长必须落在 [kFrameMin, kFrameMax]。走到这里结构上必然满足，
            // 保留为显式契约：若将来改动状态机（变长头部、新增字段），这里会立刻兜住。
            return (static_cast<uint32_t>(out.len) + kOverhead) >= kFrameMin;
        }
    }
    return false;
}

} // namespace proto
