/**
 * @file    bl_verify.hpp
 * @brief   固件镜像合法性校验
 *
 * 两级校验，代价与强度不同，按场景选用：
 *
 *   1. 向量表校验（廉价）
 *      读 APP 基址的头两个字：初始栈顶 SP 与复位入口 Reset_Handler。
 *      仅需一次 Flash 读，微秒级完成。能挡住「空片（全 0xFF）」与
 *      「扇区擦到一半」这类绝大多数损坏情形，因此每次启动都做。
 *
 *   2. 整镜像 CRC32（昂贵）
 *      遍历全部字节，800KB 在 168MHz 上约数百毫秒。
 *      用于升级完成后的完整性确认；是否每次启动也做由
 *      BL_BOOT_VERIFY_CRC32 决定。
 */
#ifndef BL_VERIFY_HPP
#define BL_VERIFY_HPP

#include "bl_types.hpp"

namespace bl {

/**
 * @brief 校验 APP 向量表是否合法
 *
 * 判据：
 *   - 初始栈顶 SP 必须落在 SRAM 范围内
 *   - 复位入口必须落在 APP 区内，且最低位为 1（Thumb 状态）
 *
 * @param app_base APP 基址
 * @param out      校验明细（可为 nullptr）
 */
Status verify_vector_table(uint32_t app_base, VectorCheck* out = nullptr) noexcept;

/**
 * @brief 完整校验：向量表 + 整镜像 CRC32
 *
 * @param app_base   APP 基址
 * @param size       固件有效字节数
 * @param expect_crc 期望的 CRC32
 * @param out_crc    实际算出的 CRC32（可为 nullptr）
 */
Status verify_image(uint32_t app_base, uint32_t size,
                    uint32_t expect_crc, uint32_t* out_crc = nullptr) noexcept;

} // namespace bl

#endif /* BL_VERIFY_HPP */
