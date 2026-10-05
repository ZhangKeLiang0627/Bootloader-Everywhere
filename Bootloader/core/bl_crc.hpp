/**
 * @file    bl_crc.hpp
 * @brief   CRC16（YMODEM 帧校验）与 CRC32（整镜像校验）
 *
 * 算法参数（必须与 PC 端、与网页上位机完全一致）：
 *   CRC16 = CRC-16/XMODEM  poly=0x1021 init=0x0000 refin=0 refout=0 xorout=0
 *           自检：对 "123456789" 应得 0x31C3
 *   CRC32 = CRC-32/ISO-HDLC poly=0x04C11DB7 init=0xFFFFFFFF
 *           refin=refout=true xorout=0xFFFFFFFF
 *           自检：对 "123456789" 应得 0xCBF43926
 *
 * 两者都提供「一次性计算」与「增量累加」两种用法。
 * CRC32 的增量接口用于边收边算，避免传输结束后重读整个 Flash。
 */
#ifndef BL_CRC_HPP
#define BL_CRC_HPP

#include "core/bl_types.hpp"

namespace bl {

/* ========================================================================
 * CRC16 / XMODEM —— YMODEM 每一帧的校验
 * ======================================================================*/
class Crc16 {
public:
    static constexpr uint16_t kInit  = 0x0000U;
    static constexpr uint16_t kCheck = 0x31C3U;   ///< 标准自检值

    constexpr Crc16() noexcept : value_(kInit) {}

    /// 复位到初始值，便于复用同一个对象
    ///
    /// 注意这里不能标 constexpr：C++11 下 constexpr 成员函数隐含 const，
    /// 而它要改成员，会编译不过。本库刻意保持 C++11 可编译 ——
    /// 目标工程未必把标准调到 C++14。
    void reset() noexcept { value_ = kInit; }

    /// 增量累加
    void update(const void* data, uint32_t len) noexcept;

    constexpr uint16_t value() const noexcept { return value_; }

    /// 一次性计算（内部新建临时对象，不改变本对象状态）
    static uint16_t compute(const void* data, uint32_t len) noexcept
    {
        Crc16 c;
        c.update(data, len);
        return c.value();
    }

private:
    uint16_t value_;
};

/* ========================================================================
 * CRC32 / ISO-HDLC —— 整镜像校验
 * ======================================================================*/
class Crc32 {
public:
    static constexpr uint32_t kInit  = 0xFFFFFFFFU;
    static constexpr uint32_t kCheck = 0xCBF43926U;   ///< 标准自检值

    constexpr Crc32() noexcept : value_(kInit) {}

    /// 复位（同样不能标 constexpr，原因见 Crc16::reset）
    void reset() noexcept { value_ = kInit; }

    /// 增量累加（value_ 保存的是未做最终异或的中间值）
    void update(const void* data, uint32_t len) noexcept;

    /// 取最终结果（内部做最终异或）
    constexpr uint32_t value() const noexcept { return value_ ^ 0xFFFFFFFFU; }

    static uint32_t compute(const void* data, uint32_t len) noexcept
    {
        Crc32 c;
        c.update(data, len);
        return c.value();
    }

private:
    uint32_t value_;
};

/// 直接对 Flash 区域计算 CRC32（分块读，不占大缓冲）
uint32_t crc32_flash(uint32_t addr, uint32_t len) noexcept;

/* ========================================================================
 * 自检
 *
 * CRC 参数一旦写错，现象是「PC 算的值和板子算的对不上」，极难排查。
 * 上电时跑一次自检，能在最早时刻暴露问题。
 * ======================================================================*/
Status crc_selftest() noexcept;

} // namespace bl

#endif /* BL_CRC_HPP */
