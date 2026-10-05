// YMODEM 接收端 与 CRC16/XMODEM 的实现（声明见 bl_protocol.h）。

#include "bl_protocol.h"
#include "bl_port.h"     // uartRead / uartWrite / uartFlushRx / tickMs

namespace bl {

// CRC16 / XMODEM（YMODEM 每帧的校验）

// CRC16 / XMODEM —— poly 0x1021，逐位实现
//
// 选逐位而非查表：YMODEM 每帧 1KB，逐位在 168MHz 上约 0.25ms，
// 完全可以接受；换来的是代码更小、更容易核对正确性。
void Crc16::update(const void* data, uint32_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);

    for (uint32_t i = 0; i < len; ++i) {
        value_ ^= static_cast<uint16_t>(static_cast<uint16_t>(p[i]) << 8);
        for (int b = 0; b < 8; ++b) {
            if (value_ & 0x8000U) {
                value_ = static_cast<uint16_t>((value_ << 1) ^ 0x1021U);
            } else {
                value_ = static_cast<uint16_t>(value_ << 1);
            }
        }
    }
}

// YMODEM 接收端

// 底层收发
bool Ymodem::sendByte(Code c) noexcept
{
    const auto b = static_cast<uint8_t>(c);
    return ok(uartWrite(&b, 1));
}

bool Ymodem::sendAbort() noexcept
{
    /* CAN 必须连发两次才被上位机识别为取消 */
    return sendByte(Code::Ca) && sendByte(Code::Ca);
}

// 解析首包中的「文件名 + 大小」
//
// 首包数据区格式："文件名\0" + "十进制大小" + ' ' + 填充
void Ymodem::parseHeader(const uint8_t* data, char* nameOut, uint32_t& sizeOut) noexcept
{
    uint32_t i = 0;

    /* 文件名：以 '\0' 结束 */
    while (i < (kFilenameMax - 1U) && data[i] != 0x00U) {
        nameOut[i] = static_cast<char>(data[i]);
        ++i;
    }
    nameOut[i] = '\0';

    /* 跳过文件名终止符 */
    if (i < kBlockMax) {
        ++i;
    }

    /* 大小：十进制数字串，遇空格或非数字结束 */
    uint32_t size   = 0;
    uint32_t digits = 0;
    while (i < kBlockMax && digits < 10U &&
           data[i] >= '0' && data[i] <= '9') {
        size = size * 10U + static_cast<uint32_t>(data[i] - '0');
        ++i;
        ++digits;
    }
    sizeOut = size;
}

// 收一帧
Ymodem::RecvRc Ymodem::recvPacket(uint32_t& len) noexcept
{
    len = 0;

    uint8_t  start = 0;
    uint32_t got   = 0;
    if (!ok(uartRead(&start, 1, cfg_.packetTimeoutMs, &got)) || got == 0) {
        return RecvRc::Timeout;
    }

    uint32_t pktSize = 0;
    switch (start) {
        case static_cast<uint8_t>(Code::Soh):
            pktSize = 128U;                 /* 128 字节小包 */
            break;

        case static_cast<uint8_t>(Code::Stx):
            pktSize = kBlockMax;            /* 1024 字节大包 */
            break;

        case static_cast<uint8_t>(Code::Eot):
            return RecvRc::Eot;              /* len 保持 0 */

        case static_cast<uint8_t>(Code::Ca): {
            /* 取消必须成对出现 */
            uint8_t  second = 0;
            uint32_t got2   = 0;
            if (!ok(uartRead(&second, 1, cfg_.packetTimeoutMs, &got2)) ||
                second != static_cast<uint8_t>(Code::Ca)) {
                return RecvRc::BadFrame;
            }
            len = 2;
            return RecvRc::Ca;
        }

        case 0x41U:   /* 'A' 上位机主动放弃 */
        case 0x61U:   /* 'a' */
            return RecvRc::Abort;

        default:
            return RecvRc::BadFrame;
    }

    /* 读 包序号 + 序号反码 + 数据 + CRC16(2字节) */
    const uint32_t want = pktSize + 4U;
    if (!ok(uartRead(&buf_[kNumIndex], want, cfg_.packetTimeoutMs, &got)) ||
        got != want) {
        return RecvRc::Timeout;
    }

    /* 序号反码自校验 */
    if (buf_[kNumIndex] != static_cast<uint8_t>(buf_[kCnumIndex] ^ 0xFFU)) {
        return RecvRc::BadFrame;
    }

    /* CRC16 校验（大端存放） */
    const uint16_t rxCrc =
        static_cast<uint16_t>((static_cast<uint16_t>(buf_[pktSize + kDataIndex]) << 8) |
                              static_cast<uint16_t>(buf_[pktSize + kDataIndex + 1U]));
    if (Crc16::compute(&buf_[kDataIndex], pktSize) != rxCrc) {
        ++stats_.crcErrors;
        return RecvRc::BadFrame;
    }

    len = pktSize;
    return RecvRc::Ok;
}

// 等待批次结束帧（全零首包）
//
// 若上位机不实现结束帧，超时后仍按成功处理——数据此时已经收完。
bool Ymodem::waitEndPacket() noexcept
{
    for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
        if (!sendByte(Code::ReqC)) {
            break;
        }

        uint32_t     len = 0;
        const RecvRc rc  = recvPacket(len);

        if (rc == RecvRc::Ok && buf_[kNumIndex] == 0U && buf_[kDataIndex] == 0U) {
            sendByte(Code::Ack);
            return true;
        }
        if (rc == RecvRc::Timeout) {
            continue;
        }
        if (rc == RecvRc::Ok) {
            sendByte(Code::Nak);
            continue;
        }
        break;
    }
    /* 数据已完整接收，结束帧缺失不影响升级结果 */
    return true;
}

// 主接收流程
Ymodem::Outcome Ymodem::receive() noexcept
{
    Outcome out;

    uartFlushRx();

    /* 主动发起握手，请求 CRC 模式（比等着超时再发更规范、更快） */
    if (!sendByte(Code::ReqC)) {
        out.status = Status::Timeout;
        return out;
    }

    /* 握手阶段总超时的计时起点。软件复位唤回 Bootloader 时，只给上位机
     * 一个有限窗口（handshakeTimeoutMs，默认 15s）：窗口内没等到
     * YMODEM 首包就超时退出，让调用方跳回 APP；否则会永远卡在 IAP。
     * 置 0 表示关闭（握手无限等，恢复传统行为）。 */
    const uint32_t handshakeStart = tickMs();

    uint32_t offset       = 0;
    bool     sessionDone = false;

    while (!sessionDone && ok(out.status)) {

        uint32_t seqExpected = 0;      /* 期望的包序号，0-255 循环 */
        bool     fileDone    = false;
        bool     sessionBegun = false;
        uint32_t errors       = 0;      /* 连续超时计数 */
        uint32_t naks         = 0;      /* 连续 NAK 计数 */

        while (!fileDone && ok(out.status)) {
            uint32_t     len = 0;
            const RecvRc rc  = recvPacket(len);

            switch (rc) {

            /* ---------------- 收到合法帧 ---------------- */
            case RecvRc::Ok: {
                errors = 0;

                if (buf_[kNumIndex] != static_cast<uint8_t>(seqExpected)) {
                    /* 序号不符：请求重传，期望序号不变 */
                    sendByte(Code::Nak);
                    ++out.stats.retries;
                    if (++naks > cfg_.maxNak) {
                        sendAbort();
                        out.status = Status::Protocol;
                    }
                    break;
                }
                naks = 0;

                if (seqExpected == 0U) {
                    /* ========== 首包 ========== */
                    if (buf_[kDataIndex] == 0x00U) {
                        /* 空首包 → 批次结束 */
                        sendByte(Code::Ack);
                        fileDone    = true;
                        sessionDone = true;
                        break;
                    }

                    char     name[kFilenameMax] = {0};
                    uint32_t size = 0;
                    parseHeader(&buf_[kDataIndex], name, size);

                    /* 记录文件名供上层展示 */
                    for (uint32_t k = 0; k < kFilenameMax; ++k) {
                        out.stats.filename[k] = name[k];
                        if (name[k] == '\0') {
                            break;
                        }
                    }
                    out.stats.fileSize = size;

                    /* 交由上层决定是否接受（校验大小 + 擦除 Flash） */
                    if (!sink_.onFileStart(name, size)) {
                        sendAbort();
                        out.status = Status::NoSpace;
                        fileDone  = true;
                        break;
                    }

                    offset = 0;
                    sendByte(Code::Ack);
                    sendByte(Code::ReqC);   /* 请求第一个数据包 */
                } else {
                    /* ========== 数据包 ========== */
                    if (!sink_.onFileData(offset, &buf_[kDataIndex], len)) {
                        sendAbort();
                        out.status = Status::FlashFail;
                        fileDone  = true;
                        break;
                    }
                    offset            += len;
                    out.stats.received = offset;
                    ++out.stats.packets;
                    sendByte(Code::Ack);
                }

                ++seqExpected;
                sessionBegun = true;
                break;
            }

            /* ---------------- 传输结束 ---------------- */
            case RecvRc::Eot: {
                sendByte(Code::Ack);
                sink_.onFileEnd(offset);
                fileDone    = true;
                sessionDone = waitEndPacket();
                break;
            }

            /* ---------------- 上位机取消 ---------------- */
            case RecvRc::Ca: {
                sendByte(Code::Ack);
                out.status = Status::Cancelled;
                sessionDone = true;
                break;
            }

            /* ---------------- 上位机主动放弃 ---------------- */
            case RecvRc::Abort: {
                sendAbort();
                out.status = Status::Cancelled;
                sessionDone = true;
                break;
            }

            /* ---------------- 帧错误 / 超时 ---------------- */
            case RecvRc::BadFrame:
            case RecvRc::Timeout:
            default: {
                if (sessionBegun) {
                    /* 已在收数据途中：累计连续超时，超限中止 */
                    if (++errors > cfg_.maxErrors) {
                        sendAbort();
                        out.status = Status::Timeout;
                        fileDone  = true;
                        break;
                    }
                } else if (cfg_.handshakeTimeoutMs != 0U &&
                           (tickMs() - handshakeStart) >= cfg_.handshakeTimeoutMs) {
                    /* 握手总超时：窗口内没等到首包，超时退出 */
                    out.status = Status::Timeout;
                    fileDone  = true;
                    break;
                }
                /* 请求重发：发 'C' 会被当成请求下一包，
                 * 仅对 CRC 模式有效；乱序/损坏时用 NAK 更准确 */
                sendByte(Code::ReqC);
                ++out.stats.retries;
                break;
            }

            } /* switch */
        } /* 内层 while */
    } /* 外层 while */

    return out;
}

} // namespace bl
