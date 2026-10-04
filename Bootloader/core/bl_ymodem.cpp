/**
 * @file    bl_ymodem.cpp
 * @brief   YMODEM-1K 接收端实现
 *
 * 流程（本类是接收方，PC 是发送方）：
 *
 *   本类                          PC 上位机
 *    |  ------- 'C' ------------> |   请求 CRC 模式
 *    |  <--- SOH 0 文件名+大小 -- |
 *    |  [回调 on_file_start：校验大小 + 擦除]
 *    |  ------- ACK + 'C' ------> |
 *    |  <--- STX 1 数据 + CRC16- |   逐包停等
 *    |  ------- ACK -----------> |
 *    |          ...              |
 *    |  <--------- EOT --------- |
 *    |  ------- ACK + 'C' ------> |
 *    |  <--- SOH 0 全零(批次结束)- |
 *    |  ------- ACK -----------> |
 *
 * 擦除时机：收到首包拿到大小后，在 on_file_start 内一次性完成。
 * 此刻 PC 正在等 ACK，数秒的擦除耗时不会触发上位机超时；
 * 若改为边收边擦，128KB 扇区约 1 秒的擦除会撑爆多数工具的默认超时。
 */
#include "core/bl_ymodem.hpp"
#include "core/bl_crc.hpp"
#include "port/bl_port.hpp"

namespace bl {

/* ========================================================================
 * 底层收发
 * ======================================================================*/
bool Ymodem::send_byte(Code c) noexcept
{
    const auto b = static_cast<uint8_t>(c);
    return ok(uart_write(&b, 1));
}

bool Ymodem::send_abort() noexcept
{
    /* CAN 必须连发两次才被上位机识别为取消 */
    return send_byte(Code::Ca) && send_byte(Code::Ca);
}

/* ========================================================================
 * 解析首包中的「文件名 + 大小」
 *
 * 首包数据区格式："文件名\0" + "十进制大小" + ' ' + 填充
 * ======================================================================*/
void Ymodem::parse_header(const uint8_t* data, char* name_out, uint32_t& size_out) noexcept
{
    uint32_t i = 0;

    /* 文件名：以 '\0' 结束 */
    while (i < (kFilenameMax - 1U) && data[i] != 0x00U) {
        name_out[i] = static_cast<char>(data[i]);
        ++i;
    }
    name_out[i] = '\0';

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
    size_out = size;
}

/* ========================================================================
 * 收一帧
 * ======================================================================*/
Ymodem::RecvRc Ymodem::recv_packet(uint32_t& len) noexcept
{
    len = 0;

    uint8_t  start = 0;
    uint32_t got   = 0;
    if (!ok(uart_read(&start, 1, cfg_.packet_timeout_ms, &got)) || got == 0) {
        return RecvRc::Timeout;
    }

    uint32_t pkt_size = 0;
    switch (start) {
        case static_cast<uint8_t>(Code::Soh):
            pkt_size = 128U;                 /* 128 字节小包 */
            break;

        case static_cast<uint8_t>(Code::Stx):
            pkt_size = kBlockMax;            /* 1024 字节大包 */
            break;

        case static_cast<uint8_t>(Code::Eot):
            return RecvRc::Eot;              /* len 保持 0 */

        case static_cast<uint8_t>(Code::Ca): {
            /* 取消必须成对出现 */
            uint8_t  second = 0;
            uint32_t got2   = 0;
            if (!ok(uart_read(&second, 1, cfg_.packet_timeout_ms, &got2)) ||
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
    const uint32_t want = pkt_size + 4U;
    if (!ok(uart_read(&buf_[kNumIndex], want, cfg_.packet_timeout_ms, &got)) ||
        got != want) {
        return RecvRc::Timeout;
    }

    /* 序号反码自校验 */
    if (buf_[kNumIndex] != static_cast<uint8_t>(buf_[kCnumIndex] ^ 0xFFU)) {
        return RecvRc::BadFrame;
    }

    /* CRC16 校验（大端存放） */
    const uint16_t rx_crc =
        static_cast<uint16_t>((static_cast<uint16_t>(buf_[pkt_size + kDataIndex]) << 8) |
                              static_cast<uint16_t>(buf_[pkt_size + kDataIndex + 1U]));
    if (Crc16::compute(&buf_[kDataIndex], pkt_size) != rx_crc) {
        ++stats_.crc_errors;
        return RecvRc::BadFrame;
    }

    len = pkt_size;
    return RecvRc::Ok;
}

/* ========================================================================
 * 等待批次结束帧（全零首包）
 *
 * 若上位机不实现结束帧，超时后仍按成功处理——数据此时已经收完。
 * ======================================================================*/
bool Ymodem::wait_end_packet() noexcept
{
    for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
        if (!send_byte(Code::ReqC)) {
            break;
        }

        uint32_t     len = 0;
        const RecvRc rc  = recv_packet(len);

        if (rc == RecvRc::Ok && buf_[kNumIndex] == 0U && buf_[kDataIndex] == 0U) {
            send_byte(Code::Ack);
            return true;
        }
        if (rc == RecvRc::Timeout) {
            continue;
        }
        if (rc == RecvRc::Ok) {
            send_byte(Code::Nak);
            continue;
        }
        break;
    }
    /* 数据已完整接收，结束帧缺失不影响升级结果 */
    return true;
}

/* ========================================================================
 * 主接收流程
 * ======================================================================*/
Ymodem::Outcome Ymodem::receive() noexcept
{
    Outcome out;

    uart_flush_rx();

    /* 主动发起握手，请求 CRC 模式（比等着超时再发更规范、更快） */
    if (!send_byte(Code::ReqC)) {
        out.status = Status::Timeout;
        return out;
    }

    /* 握手阶段总超时的计时起点。软件复位唤回 Bootloader 时，只给上位机
     * 一个有限窗口（handshake_timeout_ms，默认 15s）：窗口内没等到
     * YMODEM 首包就超时退出，让调用方跳回 APP；否则会永远卡在 IAP。
     * 置 0 表示关闭（握手无限等，恢复传统行为）。 */
    const uint32_t handshake_start = tick_ms();

    uint32_t offset       = 0;
    bool     session_done = false;

    while (!session_done && ok(out.status)) {

        uint32_t seq_expected = 0;      /* 期望的包序号，0-255 循环 */
        bool     file_done    = false;
        bool     session_begun = false;
        uint32_t errors       = 0;      /* 连续超时计数 */
        uint32_t naks         = 0;      /* 连续 NAK 计数 */

        while (!file_done && ok(out.status)) {
            uint32_t     len = 0;
            const RecvRc rc  = recv_packet(len);

            switch (rc) {

            /* ---------------- 收到合法帧 ---------------- */
            case RecvRc::Ok: {
                errors = 0;

                if (buf_[kNumIndex] != static_cast<uint8_t>(seq_expected)) {
                    /* 序号不符：请求重传，期望序号不变 */
                    send_byte(Code::Nak);
                    ++out.stats.retries;
                    if (++naks > cfg_.max_nak) {
                        send_abort();
                        out.status = Status::Protocol;
                    }
                    break;
                }
                naks = 0;

                if (seq_expected == 0U) {
                    /* ========== 首包 ========== */
                    if (buf_[kDataIndex] == 0x00U) {
                        /* 空首包 → 批次结束 */
                        send_byte(Code::Ack);
                        file_done    = true;
                        session_done = true;
                        break;
                    }

                    char     name[kFilenameMax] = {0};
                    uint32_t size = 0;
                    parse_header(&buf_[kDataIndex], name, size);

                    /* 记录文件名供上层展示 */
                    for (uint32_t k = 0; k < kFilenameMax; ++k) {
                        out.stats.filename[k] = name[k];
                        if (name[k] == '\0') {
                            break;
                        }
                    }
                    out.stats.file_size = size;

                    /* 交由上层决定是否接受（校验大小 + 擦除 Flash） */
                    if (!sink_.on_file_start(name, size)) {
                        send_abort();
                        out.status = Status::NoSpace;
                        file_done  = true;
                        break;
                    }

                    offset = 0;
                    send_byte(Code::Ack);
                    send_byte(Code::ReqC);   /* 请求第一个数据包 */
                } else {
                    /* ========== 数据包 ========== */
                    if (!sink_.on_file_data(offset, &buf_[kDataIndex], len)) {
                        send_abort();
                        out.status = Status::FlashFail;
                        file_done  = true;
                        break;
                    }
                    offset            += len;
                    out.stats.received = offset;
                    ++out.stats.packets;
                    send_byte(Code::Ack);
                }

                ++seq_expected;
                session_begun = true;
                break;
            }

            /* ---------------- 传输结束 ---------------- */
            case RecvRc::Eot: {
                send_byte(Code::Ack);
                sink_.on_file_end(offset);
                file_done    = true;
                session_done = wait_end_packet();
                break;
            }

            /* ---------------- 上位机取消 ---------------- */
            case RecvRc::Ca: {
                send_byte(Code::Ack);
                out.status = Status::Cancelled;
                session_done = true;
                break;
            }

            /* ---------------- 上位机主动放弃 ---------------- */
            case RecvRc::Abort: {
                send_abort();
                out.status = Status::Cancelled;
                session_done = true;
                break;
            }

            /* ---------------- 帧错误 / 超时 ---------------- */
            case RecvRc::BadFrame:
            case RecvRc::Timeout:
            default: {
                if (session_begun) {
                    /* 已在收数据途中：累计连续超时，超限中止 */
                    if (++errors > cfg_.max_errors) {
                        send_abort();
                        out.status = Status::Timeout;
                        file_done  = true;
                        break;
                    }
                } else if (cfg_.handshake_timeout_ms != 0U &&
                           (tick_ms() - handshake_start) >= cfg_.handshake_timeout_ms) {
                    /* 握手总超时：窗口内没等到首包，超时退出 */
                    out.status = Status::Timeout;
                    file_done  = true;
                    break;
                }
                /* 请求重发：发 'C' 会被当成请求下一包，
                 * 仅对 CRC 模式有效；乱序/损坏时用 NAK 更准确 */
                send_byte(Code::ReqC);
                ++out.stats.retries;
                break;
            }

            } /* switch */
        } /* 内层 while */
    } /* 外层 while */

    return out;
}

} // namespace bl
