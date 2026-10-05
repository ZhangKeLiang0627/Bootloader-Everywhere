/**
 * @file    bl_crc.cpp
 * @brief   CRC16/XMODEM 与 CRC32/ISO-HDLC 实现
 */
#include "core/bl_crc.hpp"
#include "bl_config.h"
#include "port/bl_port.hpp"

namespace bl {

/* ========================================================================
 * CRC16 / XMODEM —— poly 0x1021，逐位实现
 *
 * 选逐位而非查表：YMODEM 每帧 1KB，逐位在 168MHz 上约 0.25ms，
 * 完全可以接受；换来的是代码更小、更容易核对正确性。
 * ======================================================================*/
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

/* ========================================================================
 * CRC32 / ISO-HDLC —— 半字节查表（16 项 × 4 字节 = 64B 表）
 *
 * 表项为反向多项式 0xEDB88320 的 4 位查表，
 * 一次处理 4 bit，比纯逐位快约 4 倍，而表只占 64 字节。
 * ======================================================================*/
namespace {
constexpr uint32_t kNibbleTable[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};
} // namespace

void Crc32::update(const void* data, uint32_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);

    for (uint32_t i = 0; i < len; ++i) {
        value_ ^= static_cast<uint32_t>(p[i]);
        value_ = (value_ >> 4) ^ kNibbleTable[value_ & 0x0FU];
        value_ = (value_ >> 4) ^ kNibbleTable[value_ & 0x0FU];
    }
}

/* ========================================================================
 * 直接对 Flash 区域算 CRC32
 * ======================================================================*/
uint32_t crc32_flash(uint32_t addr, uint32_t len) noexcept
{
    uint8_t buf[256];
    Crc32   crc;
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (!ok(flash_read(addr + done, buf, chunk))) {
            /* 读失败时返回哨兵值，正常 CRC 结果不可能等于它 */
            return 0xDEADBEEFU;
        }
        crc.update(buf, chunk);
        done += chunk;
    }
    return crc.value();
}

/* ========================================================================
 * 自检
 * ======================================================================*/
Status crc_selftest() noexcept
{
    static constexpr char kVector[] = "123456789";

    if (Crc16::compute(kVector, 9) != Crc16::kCheck) {
        return Status::CrcFail;
    }
    if (Crc32::compute(kVector, 9) != Crc32::kCheck) {
        return Status::CrcFail;
    }
    return Status::Ok;
}

} // namespace bl
