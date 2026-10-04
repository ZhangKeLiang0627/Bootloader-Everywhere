/**
 * @file    bl_meta.h
 * @brief   配置区（Flash 参数区）读写 —— 固件状态机与原子提交的载体
 *
 * 存储方式：日志式槽位轮转
 *   把整个配置扇区切成 N 个 64 字节槽，每次状态变更「顺序追加」一个新槽，
 *   不擦除。读取时扫描取序号最大的有效槽。
 *   写满整个扇区后才擦除一次并重新开始。
 *
 * 这样做的原因：常规状态写入只有 64 字节、耗时几十微秒，
 * 不会因为「每次上电都要记录启动次数」而引入秒级的扇区擦除延迟，
 * 同时把擦写寿命摊薄到 N 倍。
 */
#ifndef BL_META_H
#define BL_META_H

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 槽位魔数 "LUMS" */
#define BL_META_SLOT_MAGIC      0x4C554D53UL

/** 状态标志位 */
#define BL_META_FLAG_UPDATE_REQ 0x00000001UL   /**< APP 请求进入升级 */

/**
 * @brief 单个槽位的完整布局（恰好 BL_META_SLOT_SIZE 字节）
 *
 * 字段顺序与大小一旦发布就不能随意改动，
 * 否则旧设备升级后会读不懂自己的配置区。
 */
typedef struct {
    uint32_t seq;               /**< 自增序号，用于判定哪一槽最新 */
    uint32_t fw_state;          /**< bl_fw_state_t */
    uint32_t fw_size;           /**< APP 有效字节数 */
    uint32_t fw_crc32;          /**< APP 整镜像 CRC32 */
    uint32_t fw_version;        /**< 固件版本号 */
    uint32_t boot_attempts;     /**< 连续启动尝试次数（回滚判据） */
    uint32_t flags;             /**< BL_META_FLAG_* */
    uint32_t magic;             /**< BL_META_SLOT_MAGIC */
    uint32_t reserved[6];       /**< 预留，必须写 0 */
    uint32_t slot_crc32;        /**< 本槽前 60 字节的 CRC32 */
} bl_meta_t;

/* ---- 编译期校验：结构体大小必须与槽位一致 ---- */
/* 用数组维度做静态断言，避免依赖 C11 _Static_assert 之外的方言 */
typedef char bl_meta_size_check_t[
    (sizeof(bl_meta_t) == 64) ? 1 : -1
];

/**
 * @brief 初始化：扫描配置区，装载当前状态到内存
 *
 * 未找到任何有效槽时，内存状态被置为 BL_FW_INVALID，
 * 但不立即写盘（避免把一个「空配置区」变成有内容的配置区）。
 */
bl_status_t bl_meta_init(void);

/** 取当前内存中的状态快照（只读） */
const bl_meta_t *bl_meta_current(void);

/** 把状态写入配置区（追加新槽） */
bl_status_t bl_meta_write(const bl_meta_t *m);

/* ---- 语义化状态迁移（内部自动递增 seq 并写盘） ---- */

/**
 * @brief 标记「升级开始」，置 BL_FW_DOWNLOAD
 *
 * 必须在擦除 APP 区之前调用。这是防变砖的关键顺序：
 * 先让状态变成「不可信」，再去破坏 APP 区。
 * 若顺序颠倒，擦除中途掉电会留下「状态 VALID 但 APP 已空」的必砖组合。
 */
bl_status_t bl_meta_mark_download(void);

/**
 * @brief 标记「传输并校验完成」，置 BL_FW_TESTING
 * @param size    固件有效字节数
 * @param crc32   固件整镜像 CRC32
 * @param version 固件版本号（可传 0）
 */
bl_status_t bl_meta_commit(uint32_t size, uint32_t crc32, uint32_t version);

/**
 * @brief APP 侧调用：确认自身运行正常，置 BL_FW_VALID 并清零启动计数
 *
 * 建议 APP 启动后延时若干秒、完成自检再调用。
 * 只写一次，之后正常启动不再产生任何配置区写入。
 */
bl_status_t bl_meta_confirm_app(void);

/** 标记固件作废（回滚后调用），置 BL_FW_REVOKED */
bl_status_t bl_meta_revoke(void);

/** APP 侧调用：请求下次上电进入升级模式 */
bl_status_t bl_meta_request_update(void);

/** 清除「请求升级」标志 */
bl_status_t bl_meta_clear_update_request(void);

/** 擦除整个配置扇区（调试用；之后状态回到 INVALID） */
bl_status_t bl_meta_erase_all(void);

/** 供启动决策使用：按当前状态判断是否应当尝试启动 APP */
bool bl_meta_should_boot(void);

/**
 * @brief 启动计数递增（仅在 TESTING 态有意义）
 * @return 递增后的计数；若当前状态无需计数则返回 0
 */
uint32_t bl_meta_bump_boot_attempts(void);

/** 打印当前状态（调试） */
void bl_meta_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_META_H */
