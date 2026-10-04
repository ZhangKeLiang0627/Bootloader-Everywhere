/**
 * @file    bl_port_flash_stm32f4.cpp
 * @brief   STM32F4（大容量，1MB）内部 Flash 驱动
 *
 * F4 的 Flash 有两个容易踩的点：
 *   1. 扇区大小不等长：S0-S3 各 16KB、S4 为 64KB、S5 及以上各 128KB。
 *      擦除时必须按实际扇区大小推进，不能按固定值步进。
 *   2. 编程粒度为 32 位字，且地址必须 4 字节对齐。
 *      长度不足 4 字节的尾巴要做「读-改-写」。
 */
#include <cstring>

#include "stm32f4xx_hal.h"
#include "bl_port.hpp"
#include "bl_config.h"

namespace bl {

/* ========================================================================
 * 扇区表
 * ======================================================================*/
namespace {

struct SectorDesc {
    uint32_t base;
    uint32_t size;
    uint32_t hal_id;   ///< HAL 的 FLASH_SECTOR_x
};

/* 1MB 共 12 个扇区；地址范围 0x08000000 - 0x080FFFFF */
constexpr SectorDesc kSectors[] = {
    { 0x08000000UL,  16UL * 1024UL, FLASH_SECTOR_0  },
    { 0x08004000UL,  16UL * 1024UL, FLASH_SECTOR_1  },
    { 0x08008000UL,  16UL * 1024UL, FLASH_SECTOR_2  },
    { 0x0800C000UL,  16UL * 1024UL, FLASH_SECTOR_3  },
    { 0x08010000UL,  64UL * 1024UL, FLASH_SECTOR_4  },
    { 0x08020000UL, 128UL * 1024UL, FLASH_SECTOR_5  },
    { 0x08040000UL, 128UL * 1024UL, FLASH_SECTOR_6  },
    { 0x08060000UL, 128UL * 1024UL, FLASH_SECTOR_7  },
    { 0x08080000UL, 128UL * 1024UL, FLASH_SECTOR_8  },
    { 0x080A0000UL, 128UL * 1024UL, FLASH_SECTOR_9  },
    { 0x080C0000UL, 128UL * 1024UL, FLASH_SECTOR_10 },
    { 0x080E0000UL, 128UL * 1024UL, FLASH_SECTOR_11 },
};

constexpr uint32_t kSectorCount = sizeof(kSectors) / sizeof(kSectors[0]);

/// 地址落在哪个扇区；返回 -1 表示越界
int sector_of(uint32_t addr) noexcept
{
    for (uint32_t i = 0; i < kSectorCount; ++i) {
        if (addr >= kSectors[i].base &&
            addr <  (kSectors[i].base + kSectors[i].size)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

/* ========================================================================
 * 初始化
 * ======================================================================*/
Status flash_init() noexcept
{
    /* F4 的 Flash 接口时钟由 HAL_Init 打开，此处只需确保处于锁定态 */
    HAL_FLASH_Lock();
    return Status::Ok;
}

uint32_t flash_sector_size(uint32_t addr) noexcept
{
    const int i = sector_of(addr);
    return (i < 0) ? 0U : kSectors[i].size;
}

uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept
{
    const int i = sector_of(addr);
    if (i < 0) {
        return 0U;
    }
    return (kSectors[i].base + kSectors[i].size) - addr;
}

/* ========================================================================
 * 擦除
 *
 * core 层保证 addr 落在扇区起始、len 为扇区大小之和，
 * 但仍做合法性检查，避免误擦到 Bootloader 自身所在的 S0。
 * ======================================================================*/
Status flash_erase(uint32_t addr, uint32_t len) noexcept
{
    if (len == 0U) {
        return Status::Ok;
    }

    /* 安全检查：绝不擦除 Bootloader 自身区域 */
    if (addr < (BL_BOOT_BASE + BL_BOOT_SIZE)) {
        BL_LOG("[flash] refuse to erase bootloader region\r\n");
        return Status::BadParam;
    }

    const int first = sector_of(addr);
    if (first < 0) {
        return Status::BadParam;
    }

    /* 统计本次覆盖的扇区个数（要求 addr 落在扇区起点） */
    uint32_t remaining = len;
    uint32_t count     = 0;
    uint32_t cur       = addr;
    while (remaining > 0U) {
        const int idx = sector_of(cur);
        if (idx < 0) {
            return Status::BadParam;
        }
        if (cur != kSectors[idx].base) {
            BL_LOG("[flash] erase addr not sector-aligned: 0x%08lX\r\n",
                   static_cast<unsigned long>(cur));
            return Status::BadParam;
        }
        const uint32_t sz = kSectors[idx].size;
        if (remaining < sz) {
            BL_LOG("[flash] erase len not sector-multiple\r\n");
            return Status::BadParam;
        }
        remaining -= sz;
        cur       += sz;
        ++count;
    }

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase{};
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;   /* 2.7V - 3.6V */
    erase.Sector       = kSectors[first].hal_id;
    erase.NbSectors    = count;

    uint32_t sector_error = 0;
    const HAL_StatusTypeDef rc = HAL_FLASHEx_Erase(&erase, &sector_error);

    HAL_FLASH_Lock();

    if (rc != HAL_OK || sector_error != 0xFFFFFFFFUL) {
        BL_LOG("[flash] erase error rc=%d sectorErr=0x%08lX\r\n",
               static_cast<int>(rc),
               static_cast<unsigned long>(sector_error));
        return Status::FlashFail;
    }
    return Status::Ok;
}

/* ========================================================================
 * 写入
 *
 * F4 编程单位为 32 位字。正常路径下 core 传入的地址与长度都是 4 的倍数
 * （YMODEM 数据区天然对齐、配置区槽为 64 字节），但这里仍处理尾巴不足
 * 一个字的情况，采用「读出原字 → 合并 → 写回」。
 * ======================================================================*/
Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept
{
    if (data == nullptr || len == 0U) {
        return Status::BadParam;
    }
    if ((addr & 0x3U) != 0U) {
        BL_LOG("[flash] write addr not word-aligned: 0x%08lX\r\n",
               static_cast<unsigned long>(addr));
        return Status::BadParam;
    }

    const auto* src = static_cast<const uint8_t*>(data);
    const uint32_t end = addr + len;

    HAL_FLASH_Unlock();

    while (addr < end) {
        wdg_feed();

        const uint32_t remain = end - addr;
        uint32_t word = 0;
        uint32_t consumed = 4U;

        if (remain >= 4U) {
            std::memcpy(&word, src, 4);
        } else {
            /* 尾巴不足一个字：读出当前内容后合并，未覆盖的字节保持原值 */
            uint32_t old = 0;
            std::memcpy(&old, reinterpret_cast<const void*>(addr), 4);

            uint8_t merged[4];
            std::memcpy(merged, &old, 4);
            std::memcpy(merged, src, remain);
            std::memcpy(&word, merged, 4);
            consumed = remain;
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK) {
            HAL_FLASH_Lock();
            BL_LOG("[flash] program error at 0x%08lX\r\n",
                   static_cast<unsigned long>(addr));
            return Status::FlashFail;
        }

        addr += 4U;
        src  += consumed;
    }

    HAL_FLASH_Lock();
    return Status::Ok;
}

/* ========================================================================
 * 读取（内存映射，直接拷贝即可）
 * ======================================================================*/
Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }
    std::memcpy(buf, reinterpret_cast<const void*>(addr), len);
    return Status::Ok;
}

} // namespace bl
