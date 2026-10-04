/**
 * @file    bl_crc.h
 * @brief   CRC16（YMODEM 帧校验）与 CRC32（整镜像校验）
 *
 * 算法参数（务必与 PC 端一致）：
 *   CRC16  = CRC-16/XMODEM  poly=0x1021 init=0x0000 refin=0 refout=0 xorout=0
 *            自检值：对 "123456789" 应得 0x31C3
 *   CRC32  = CRC-32/ISO-HDLC poly=0x04C11DB7 init=0xFFFFFFFF
 *            refin=refout=true xorout=0xFFFFFFFF
 *            自检值：对 "123456789" 应得 0xCBF43926
 *
 * CRC32 提供增量接口，可在接收数据的同时边收边算，
 * 无需在传输结束后重新读一遍 Flash（省一次 880KB 的遍历）。
 */
#ifndef BL_CRC_H
#define BL_CRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CRC32 初始值（对外部一致，勿改） */
#define BL_CRC32_INIT       0xFFFFFFFFUL

/** CRC32 自检期望值 */
#define BL_CRC32_CHECK      0xCBF43926UL

/** CRC16 自检期望值 */
#define BL_CRC16_CHECK      0x31C3U

/** 一次算完的 CRC16/XMODEM */
uint16_t bl_crc16(const void *data, uint32_t len);

/** 增量式 CRC16（首帧传 crc = 0） */
uint16_t bl_crc16_update(uint16_t crc, const void *data, uint32_t len);

/**
 * @brief 增量式 CRC32 累加
 * @param crc 首次调用传 BL_CRC32_INIT
 */
uint32_t bl_crc32_update(uint32_t crc, const void *data, uint32_t len);

/** CRC32 收尾（做最终异或） */
uint32_t bl_crc32_finish(uint32_t crc);

/** 一次算完的 CRC32 */
uint32_t bl_crc32(const void *data, uint32_t len);

/**
 * @brief 计算 Flash 区域的 CRC32
 *
 * 按块读取以避免占用大缓冲。内部定期喂狗，适合擦写量大的场景。
 * @param addr 起始地址
 * @param len  字节数
 */
uint32_t bl_crc32_flash(uint32_t addr, uint32_t len);

/** 自检：校验两种 CRC 实现是否符合标准测试向量 */
bl_status_t bl_crc_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_CRC_H */
