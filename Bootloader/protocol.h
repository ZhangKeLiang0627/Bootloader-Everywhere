// 串口帧协议（载体层）
//
// 与芯片、工程、业务都无关：只依赖 stdint.h，整对文件拷到任何地方都能用
// （APP 端、上位机、别的 MCU 工程）。线格式与规则见 docs/PROTOCOL_DESIGN.md §0.1。
//
//   [0xA5] [ID] [CMD] [len:2 LE] [Data ≤1024] [CRC8] [0x03]
//     1B    1B   1B   └─ 小端 ─┘   └── N ──┘     1B     1B
//
//   CMD 的 bit7 = 方向位（0 主→从，1 从→主），bit0-6 = 命令码
//   CRC8 覆盖 [ID .. Data 末尾]，不含帧头与帧尾

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

namespace proto {

constexpr uint8_t  kHead     = 0xA5U;              // 帧头
constexpr uint8_t  kTail     = 0x03U;              // 帧尾
constexpr uint8_t  kDirReply = 0x80U;              // CMD bit7：置 1 表示应答
constexpr uint32_t kDataMax  = 1024U;              // Data 区上限
constexpr uint32_t kOverhead = 7U;                 // 头1 + ID1 + CMD1 + len2 + CRC1 + 尾1
constexpr uint32_t kFrameMax = kOverhead + kDataMax;

// 最小帧长 = DataLen 为 0 的空载荷帧（END / STATUS 这类无数据命令）。
// 帧长必须落在 [kFrameMin, kFrameMax]：长度字段被噪声改小或改大时，
// 收下的帧宁可丢掉让主机重传，也不要拿着一个长度可疑的帧往下走。
constexpr uint32_t kFrameMin = kOverhead;

// 小端读写。帧内字段偏移不保证 4 字节对齐，不能直接做指针转换
// （Cortex-M4 容忍非对齐访问，M0 会 HardFault）。
inline uint16_t getLe16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) |
                                 (static_cast<uint32_t>(p[1]) << 8));
}

inline uint32_t getLe32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void putLe16(uint8_t* p, uint16_t v) noexcept
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void putLe32(uint8_t* p, uint32_t v) noexcept
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint8_t crc8Step(uint8_t crc, uint8_t byte) noexcept;
uint8_t crc8(const void* data, uint32_t len) noexcept;

// 收下的一帧。纯缓冲，不做默认初始化 —— 只有 Parser 返回 true 后内容才有效。
struct Frame {
    uint8_t  id;
    uint8_t  cmd;
    uint16_t len;
    uint8_t  data[kDataMax];

    bool    isReply() const noexcept { return (cmd & kDirReply) != 0U; }
    uint8_t code() const noexcept { return static_cast<uint8_t>(cmd & ~kDirReply); }

    uint32_t totalLen() const noexcept { return kOverhead + len; }

    // 帧到来时的长度自检。结构上 Parser 已经保证（按 len 收满才可能走到帧尾），
    // 这里再暴露一次给调用方当第二道防线：帧结构以外的改动出错时能立刻兜住。
    bool lenOk() const noexcept { return totalLen() >= kFrameMin && totalLen() <= kFrameMax; }
};

// 组帧到 out（容量需 len + kOverhead）。返回整帧长度；0 = 参数非法或容量不足。
uint32_t encode(uint8_t id, uint8_t cmd, const void* data, uint16_t len,
                uint8_t* out, uint32_t outCap) noexcept;

// 流式解析：逐字节喂入，凑齐一个 CRC8 与帧尾都正确的帧时返回 true。
// 校验不过会退回扫描帧头，并重新检查当前字节（它可能就是新的帧头）。
class Parser {
public:
    Parser() noexcept : state_(State::Head), crc_(0U), index_(0U) {}

    void reset() noexcept;
    bool feed(uint8_t byte, Frame& out) noexcept;

private:
    enum class State : uint8_t { Head, Id, Cmd, LenLo, LenHi, Data, Crc, Tail };

    State    state_;
    uint8_t  crc_;
    uint16_t index_;
};

} // namespace proto

#endif // PROTOCOL_H
