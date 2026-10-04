/**
 * @file    bl_crc.c
 * @brief   CRC16/XMODEM 与 CRC32/ISO-HDLC 实现
 */
#include "bl_crc.h"
#include "bl_port.h"
#include "bl_config.h"

/* ========================================================================
 * CRC16 / XMODEM —— poly 0x1021，逐位实现，代码小且不易写错
 * ======================================================================*/
uint16_t bl_crc16_update(uint16_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    int      b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t bl_crc16(const void *data, uint32_t len)
{
    return bl_crc16_update(0x0000u, data, len);
}

/* ========================================================================
 * CRC32 / ISO-HDLC —— 半字节查表（16 项，兼顾速度与 ROM 占用）
 *
 * 表项为「反向多项式 0xEDB88320 的 4 位查表」，
 * 一次处理 4 bit，比纯逐位快约 4 倍。
 * ======================================================================*/
static const uint32_t s_crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t bl_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0x0Fu];
    }
    return crc;
}

uint32_t bl_crc32_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t bl_crc32(const void *data, uint32_t len)
{
    return bl_crc32_finish(bl_crc32_update(BL_CRC32_INIT, data, len));
}

/* ========================================================================
 * 直接对 Flash 区域算 CRC32
 * ======================================================================*/
uint32_t bl_crc32_flash(uint32_t addr, uint32_t len)
{
    uint8_t  buf[256];
    uint32_t crc = BL_CRC32_INIT;
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (bl_port_flash_read(addr + done, buf, chunk) != BL_OK) {
            /* 读失败时返回一个不可能与正常结果相等的哨兵值 */
            return 0xDEADBEEFu;
        }
        crc = bl_crc32_update(crc, buf, chunk);
        done += chunk;
        bl_port_wdg_feed();
    }
    return bl_crc32_finish(crc);
}

/* ========================================================================
 * 自检：用标准测试向量验证实现正确性
 *
 * 这个函数存在的意义：CRC 参数一旦写错（比如 poly 或 init 选错），
 * 现象是「PC 端算出的值和板子算出的不一致」，很难排查。
 * 上电时跑一次自检，能在最早的时刻暴露问题。
 * ======================================================================*/
bl_status_t bl_crc_selftest(void)
{
    static const char vec[] = "123456789";

    if (bl_crc16(vec, 9) != BL_CRC16_CHECK) {
        return BL_ERR_CRC;
    }
    if (bl_crc32(vec, 9) != BL_CRC32_CHECK) {
        return BL_ERR_CRC;
    }
    return BL_OK;
}
