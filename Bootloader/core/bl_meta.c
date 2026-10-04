/**
 * @file    bl_meta.c
 * @brief   配置区日志式槽位读写实现
 */
#include <string.h>
#include "bl_meta.h"
#include "bl_crc.h"
#include "bl_port.h"
#include "bl_config.h"

/* ========================================================================
 * 内部状态
 * ======================================================================*/

/** 内存中的当前状态快照 */
static bl_meta_t s_meta;

/** 下一个可写的槽字节偏移 */
static uint32_t  s_next_offset;

/** 单调递增的槽序号基准 */
static uint32_t  s_seq;

/** 配置区是否已成功初始化 */
static bool      s_inited;

/** 槽内参与 CRC 计算的字节数（末尾 4 字节 slot_crc32 自身不参与） */
#define BL_META_CRC_SPAN    (BL_META_SLOT_SIZE - sizeof(uint32_t))

/* ========================================================================
 * 内部工具
 * ======================================================================*/

/** 判断一段缓冲是否全为 0xFF（即 Flash 擦除态） */
static bool is_erased(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

/** 校验单个槽的完整性与魔数 */
static bool slot_valid(const bl_meta_t *m)
{
    if (m->magic != BL_META_SLOT_MAGIC) {
        return false;
    }
    return (bl_crc32(m, BL_META_CRC_SPAN) == m->slot_crc32);
}

/* ========================================================================
 * 初始化：扫描配置区，装载最新有效槽
 * ======================================================================*/
bl_status_t bl_meta_init(void)
{
    bl_meta_t slot;
    bool      found = false;
    uint32_t  off;

    s_next_offset = 0;
    s_seq         = 0;
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.fw_state = BL_FW_INVALID;

    for (off = 0; off + BL_META_SLOT_SIZE <= BL_META_SIZE; off += BL_META_SLOT_SIZE) {

        if (bl_port_flash_read(BL_META_BASE + off, &slot, sizeof(slot)) != BL_OK) {
            return BL_ERR_FLASH;
        }

        /* 顺序追加写入，遇到第一个空槽即可停止扫描 */
        if (is_erased((const uint8_t *)&slot, sizeof(slot))) {
            break;
        }

        /* 本槽已被占用（无论是否损坏），下一个可写位置都要往后推 */
        s_next_offset = off + BL_META_SLOT_SIZE;

        if (!slot_valid(&slot)) {
            continue;   /* 写入中途掉电留下的残槽，跳过 */
        }

        if (!found || slot.seq > s_meta.seq) {
            s_meta = slot;
            found  = true;
        }
    }

    if (found) {
        s_seq = s_meta.seq;
    } else {
        /* 配置区为空或全部损坏：视为无有效固件 */
        memset(&s_meta, 0, sizeof(s_meta));
        s_meta.fw_state = BL_FW_INVALID;
        s_seq = 0;
        s_next_offset = 0;
    }

    s_inited = true;
    return BL_OK;
}

const bl_meta_t *bl_meta_current(void)
{
    return &s_meta;
}

/* ========================================================================
 * 写入：追加一个新槽
 * ======================================================================*/
bl_status_t bl_meta_write(const bl_meta_t *m)
{
    bl_meta_t slot;
    uint32_t  off;

    if (!s_inited) {
        return BL_ERR_STATE;
    }
    if (m == NULL) {
        return BL_ERR_PARAM;
    }

    /* 槽位用尽：擦除整个配置扇区后从头开始。
     * 擦除期间若掉电，配置区全 0xFF 会被判为 INVALID，
     * 结果是「停在 IAP 等待重刷」——可用性下降，但不是变砖。 */
    if (s_next_offset + BL_META_SLOT_SIZE > BL_META_SIZE) {
        if (bl_port_flash_erase(BL_META_BASE, BL_META_SIZE) != BL_OK) {
            return BL_ERR_FLASH;
        }
        s_next_offset = 0;
    }

    slot = *m;
    slot.seq        = ++s_seq;
    slot.magic      = BL_META_SLOT_MAGIC;
    slot.slot_crc32 = bl_crc32(&slot, BL_META_CRC_SPAN);

    off = s_next_offset;
    if (bl_port_flash_write(BL_META_BASE + off, &slot, sizeof(slot)) != BL_OK) {
        return BL_ERR_FLASH;
    }

    s_next_offset = off + BL_META_SLOT_SIZE;
    s_meta        = slot;

    /* 回读校验，确认真的落盘（Flash 编程失败不易被察觉） */
    {
        bl_meta_t verify;
        if (bl_port_flash_read(BL_META_BASE + off, &verify, sizeof(verify)) != BL_OK) {
            return BL_ERR_FLASH;
        }
        if (memcmp(&verify, &slot, sizeof(slot)) != 0) {
            return BL_ERR_FLASH;
        }
    }

    return BL_OK;
}

/* ========================================================================
 * 语义化状态迁移
 * ======================================================================*/
bl_status_t bl_meta_mark_download(void)
{
    bl_meta_t m = s_meta;

    m.fw_state      = BL_FW_DOWNLOAD;
    m.fw_size       = 0;
    m.fw_crc32      = 0;
    m.boot_attempts = 0;
    m.flags        &= ~BL_META_FLAG_UPDATE_REQ;

    return bl_meta_write(&m);
}

bl_status_t bl_meta_commit(uint32_t size, uint32_t crc32, uint32_t version)
{
    bl_meta_t m = s_meta;

    m.fw_state      = BL_FW_TESTING;   /* 先试运行，等待 APP 自检确认 */
    m.fw_size       = size;
    m.fw_crc32      = crc32;
    m.fw_version    = version;
    m.boot_attempts = 0;

    return bl_meta_write(&m);
}

bl_status_t bl_meta_confirm_app(void)
{
    bl_meta_t m = s_meta;

    if (m.fw_state != BL_FW_TESTING && m.fw_state != BL_FW_VALID) {
        return BL_ERR_STATE;
    }
    /* 已确认且无待清计数 → 无需重复写入，避免无谓的 Flash 消耗 */
    if (m.fw_state == BL_FW_VALID && m.boot_attempts == 0) {
        return BL_OK;
    }

    m.fw_state      = BL_FW_VALID;
    m.boot_attempts = 0;

    return bl_meta_write(&m);
}

bl_status_t bl_meta_revoke(void)
{
    bl_meta_t m = s_meta;

    m.fw_state = BL_FW_REVOKED;

    return bl_meta_write(&m);
}

bl_status_t bl_meta_request_update(void)
{
    bl_meta_t m = s_meta;

    m.flags |= BL_META_FLAG_UPDATE_REQ;

    return bl_meta_write(&m);
}

bl_status_t bl_meta_clear_update_request(void)
{
    bl_meta_t m = s_meta;

    if ((m.flags & BL_META_FLAG_UPDATE_REQ) == 0) {
        return BL_OK;
    }
    m.flags &= ~BL_META_FLAG_UPDATE_REQ;

    return bl_meta_write(&m);
}

bl_status_t bl_meta_erase_all(void)
{
    if (!s_inited) {
        return BL_ERR_STATE;
    }
    if (bl_port_flash_erase(BL_META_BASE, BL_META_SIZE) != BL_OK) {
        return BL_ERR_FLASH;
    }
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.fw_state = BL_FW_INVALID;
    s_next_offset   = 0;
    s_seq           = 0;
    return BL_OK;
}

/* ========================================================================
 * 启动决策辅助
 * ======================================================================*/
bool bl_meta_should_boot(void)
{
    return bl_fw_state_bootable((bl_fw_state_t)s_meta.fw_state);
}

uint32_t bl_meta_bump_boot_attempts(void)
{
    bl_meta_t m;

    /* 仅试运行态需要计数；已确认的固件正常启动不再产生任何写入 */
    if (s_meta.fw_state != BL_FW_TESTING) {
        return 0;
    }

    m = s_meta;
    m.boot_attempts++;

    if (bl_meta_write(&m) != BL_OK) {
        return m.boot_attempts;
    }
    return s_meta.boot_attempts;
}

/* ========================================================================
 * 调试输出
 * ======================================================================*/
void bl_meta_dump(void)
{
    const char *name;

    switch ((bl_fw_state_t)s_meta.fw_state) {
        case BL_FW_INVALID:  name = "INVALID";  break;
        case BL_FW_DOWNLOAD: name = "DOWNLOAD"; break;
        case BL_FW_TESTING:  name = "TESTING";  break;
        case BL_FW_VALID:    name = "VALID";    break;
        case BL_FW_REVOKED:  name = "REVOKED";  break;
        case BL_FW_ERASED:   name = "ERASED";   break;
        default:             name = "UNKNOWN";  break;
    }

    BL_LOG("[meta] state=%s seq=%lu size=%lu crc=0x%08lX ver=%lu attempts=%lu flags=0x%lX\r\n",
           name,
           (unsigned long)s_meta.seq,
           (unsigned long)s_meta.fw_size,
           (unsigned long)s_meta.fw_crc32,
           (unsigned long)s_meta.fw_version,
           (unsigned long)s_meta.boot_attempts,
           (unsigned long)s_meta.flags);
}
