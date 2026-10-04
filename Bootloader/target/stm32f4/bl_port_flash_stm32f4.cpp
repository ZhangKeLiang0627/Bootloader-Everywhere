/**
 * @file    bl_port_flash_stm32f4.cpp
 * @brief   STM32F4 内部 Flash 驱动（单 bank 型号：F401/F405/F407/F411/F415/F417）
 *
 * 两个容易踩的点：
 *   1. 扇区大小不等长：S0-S3 各 16KB、S4 为 64KB、S5 及以上各 128KB。
 *      擦除必须按实际扇区大小推进，不能按固定值步进。
 *   2. 编程粒度为 32 位字且地址必须 4 字节对齐，长度不足 4 字节的
 *      尾巴要做「读-改-写」。
 *
 * 扇区表不硬编码：F4 全系遵循同一条布局规则，按地址算即可 ——
 * 同一份驱动就能同时服务 512KB 的 F401 与 1MB 的 F405，
 * 换容量只需要改 bl_config.h 里的 BL_FLASH_SIZE。
 *
 * 适用边界：本规则适用于 1MB 及以下的单 bank 器件。
 * F42x/F43x（2MB、双 bank）扇区划分不同，需另写一份 target 适配。
 */
#include "bl_config.h"          /* 必须最先：芯片参数 */

#include <cstring>

#include "stm32f4xx_hal.h"

#include "port/bl_port.hpp"
#include "core/bl_log.hpp"

namespace bl {
namespace {

/* ============================================================================
 * F4 扇区布局规则
 *
 *   偏移 0     - 64KB  : S0..S3，每扇区 16KB
 *   偏移 64KB  - 128KB : S4，单扇区 64KB
 *   偏移 128KB - 末尾  : S5..，每扇区 128KB
 * ==========================================================================*/
constexpr uint32_t kSmallSize  = 16U * 1024U;
constexpr uint32_t kSmallCount = 4U;
constexpr uint32_t kMidSize    = 64U * 1024U;
constexpr uint32_t kLargeSize  = 128U * 1024U;

constexpr uint32_t kMidBase   = kSmallCount * kSmallSize;    /* 64KB  */
constexpr uint32_t kLargeBase = kMidBase + kMidSize;         /* 128KB */

/** 全片扇区个数 = 5 + (容量 - 128KB) / 128KB。512KB→8，1MB→12 */
constexpr uint32_t kSectorCount =
    5U + (BL_FLASH_SIZE - kLargeBase) / kLargeSize;

static_assert(BL_FLASH_SIZE >= (256U * 1024U),
              "BL_FLASH_SIZE 太小：F4 至少要 256KB 才放得下 16KB Bootloader + 配置区");

/** 地址是否落在本片 Flash 内 */
constexpr bool in_flash(uint32_t addr) noexcept
{
    return addr >= BL_FLASH_BASE && addr < (BL_FLASH_BASE + BL_FLASH_SIZE);
}

/** 地址所属扇区序号；越界时返回值 >= kSectorCount */
constexpr uint32_t sector_index(uint32_t addr) noexcept
{
    const uint32_t off = addr - BL_FLASH_BASE;
    return (off < kMidBase)   ? (off / kSmallSize) :
           (off < kLargeBase) ? kSmallCount :
           (kSmallCount + 1U + (off - kLargeBase) / kLargeSize);
}

/** 扇区起始地址 */
constexpr uint32_t sector_base(uint32_t idx) noexcept
{
    return (idx < kSmallCount)  ? (BL_FLASH_BASE + idx * kSmallSize) :
           (idx == kSmallCount) ? (BL_FLASH_BASE + kMidBase) :
           (BL_FLASH_BASE + kLargeBase + (idx - kSmallCount - 1U) * kLargeSize);
}

/** 扇区大小 */
constexpr uint32_t sector_size(uint32_t idx) noexcept
{
    return (idx < kSmallCount)  ? kSmallSize :
           (idx == kSmallCount) ? kMidSize : kLargeSize;
}

} // namespace

/* ============================================================================
 * 初始化
 * ==========================================================================*/
Status flash_init() noexcept
{
    /* F4 的 Flash 接口时钟由 HAL_Init 打开，这里只需确保处于锁定态 */
    HAL_FLASH_Lock();
    return Status::Ok;
}

uint32_t flash_sector_size(uint32_t addr) noexcept
{
    if (!in_flash(addr)) {
        return 0U;
    }
    const uint32_t idx = sector_index(addr);
    return (idx < kSectorCount) ? sector_size(idx) : 0U;
}

uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept
{
    if (!in_flash(addr)) {
        return 0U;
    }
    const uint32_t idx = sector_index(addr);
    if (idx >= kSectorCount) {
        return 0U;
    }
    return (sector_base(idx) + sector_size(idx)) - addr;
}

/* ============================================================================
 * 擦除
 *
 * core 层已保证 addr 落在扇区起始、len 是若干扇区之和；这里仍做独立校验，
 * 并额外拒绝擦除 Bootloader 自身 —— 就算上层逻辑写出 bug，
 * 也不可能把「重刷入口」擦掉。
 * ==========================================================================*/
Status flash_erase(uint32_t addr, uint32_t len) noexcept
{
    if (len == 0U) {
        return Status::Ok;
    }

    if (addr < (BL_BOOT_BASE + BL_BOOT_SIZE)) {
        BL_LOG("[flash] refuse to erase bootloader region\r\n");
        return Status::BadParam;
    }
    if (!in_flash(addr) || !in_flash(addr + len - 1U)) {
        BL_LOG("[flash] erase out of flash range: 0x%08lX +%lu\r\n",
               static_cast<unsigned long>(addr),
               static_cast<unsigned long>(len));
        return Status::BadParam;
    }

    const uint32_t first = sector_index(addr);
    if (first >= kSectorCount || addr != sector_base(first)) {
        BL_LOG("[flash] erase addr not sector-aligned: 0x%08lX\r\n",
               static_cast<unsigned long>(addr));
        return Status::BadParam;
    }

    /* 沿扇区边界走完整个区间，确认 len 正好由整数个扇区构成 */
    uint32_t remaining = len;
    uint32_t count     = 0;
    uint32_t cur       = addr;
    while (remaining > 0U) {
        const uint32_t idx = sector_index(cur);
        if (idx >= kSectorCount || cur != sector_base(idx)) {
            BL_LOG("[flash] erase len not sector-multiple\r\n");
            return Status::BadParam;
        }
        const uint32_t sz = sector_size(idx);
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
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;      /* 2.7V - 3.6V */
    erase.Sector       = first;
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

/* ============================================================================
 * 写入
 *
 * F4 编程单位是 32 位字。正常路径下 core 传入的地址与长度都是 4 的倍数
 * （YMODEM 数据区天然对齐、配置区槽为 64 字节），但这里仍处理尾巴不足
 * 一个字的情况：读出原字 → 合并 → 写回。
 * ==========================================================================*/
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

    const auto*    src = static_cast<const uint8_t*>(data);
    const uint32_t end = addr + len;

    HAL_FLASH_Unlock();

    while (addr < end) {
        wdg_feed();

        const uint32_t remain   = end - addr;
        uint32_t       word     = 0;
        uint32_t       consumed = 4U;

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

/* ============================================================================
 * 读取（Flash 内存映射，直接拷贝）
 * ==========================================================================*/
Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }
    std::memcpy(buf, reinterpret_cast<const void*>(addr), len);
    return Status::Ok;
}

} // namespace bl
