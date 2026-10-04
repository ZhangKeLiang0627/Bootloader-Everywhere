/**
 * @file    bl_verify.cpp
 * @brief   固件镜像合法性校验的实现
 */
#include "bl_verify.hpp"
#include "bl_crc.hpp"
#include "bl_port.hpp"
#include "bl_log.hpp"
#include "bl_config.h"

namespace bl {

/* ========================================================================
 * 向量表校验
 *
 * 这一检查极其廉价（两次 32 位读），却能挡住最常见的两类「坏固件」：
 *   - 空片：全 0xFF → SP=0xFFFFFFFF 不在 SRAM 范围
 *   - 擦到一半的扇区：向量表首字可能已被擦成 0xFF 或残值
 * ======================================================================*/
Status verify_vector_table(uint32_t app_base, VectorCheck* out) noexcept
{
    uint32_t vec[2] = {0, 0};

    if (!ok(flash_read(app_base, vec, sizeof(vec)))) {
        return Status::FlashFail;
    }

    const uint32_t initial_sp    = vec[0];
    const uint32_t reset_handler = vec[1];

    const bool sp_ok =
        (initial_sp >= BL_SRAM_BASE) && (initial_sp <= BL_SRAM_END);

    /* 复位入口须落在 APP 区内，且为 Thumb 地址（最低位为 1） */
    const bool entry_ok =
        (reset_handler >= BL_APP_BASE) &&
        (reset_handler <  (BL_APP_BASE + BL_APP_SIZE)) &&
        ((reset_handler & 0x1U) != 0U);

    if (out != nullptr) {
        out->initial_sp      = initial_sp;
        out->reset_handler   = reset_handler;
        out->sp_in_sram      = sp_ok;
        out->entry_is_thumb  = entry_ok;
    }

    return (sp_ok && entry_ok) ? Status::Ok : Status::CrcFail;
}

/* ========================================================================
 * 完整校验
 * ======================================================================*/
Status verify_image(uint32_t app_base, uint32_t size,
                    uint32_t expect_crc, uint32_t* out_crc) noexcept
{
    if (size == 0U || size > BL_APP_SIZE) {
        return Status::BadParam;
    }

    /* 先做廉价的向量表检查，不通过就没必要算 CRC */
    Status s = verify_vector_table(app_base, nullptr);
    if (!ok(s)) {
        return s;
    }

    /* CRC32 只覆盖固件原始长度 size，不做任何对齐取整。
     *
     * 这一点必须与上位机严格一致，否则校验永远失败：
     * YMODEM 按 1024 字节分包，最后一包不足时发送方会用填充字节补满
     * （不同工具的填充值可能是 0x00 / 0x1A / 0xFF），
     * 这些填充字节同样会被写进 Flash。
     * 若上位机按「原始文件字节」算 CRC、而本端按「对齐后长度」算，
     * 结果必然对不上。因此双方统一约定：CRC 只覆盖前 size 字节。 */
    const uint32_t actual = crc32_flash(app_base, size);
    if (out_crc != nullptr) {
        *out_crc = actual;
    }

    if (actual != expect_crc) {
        BL_LOG("[verify] crc mismatch: calc=0x%08lX expect=0x%08lX size=%lu\r\n",
               static_cast<unsigned long>(actual),
               static_cast<unsigned long>(expect_crc),
               static_cast<unsigned long>(size));
        return Status::CrcFail;
    }
    return Status::Ok;
}

} // namespace bl
