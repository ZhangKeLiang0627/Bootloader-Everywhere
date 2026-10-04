/**
 * @file    bl_types.h
 * @brief   公共类型、错误码与固件状态定义（平台无关）
 */
#ifndef BL_TYPES_H
#define BL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ========================================================================
 * 返回码
 * ======================================================================*/
typedef enum {
    BL_OK               =  0,   /**< 成功 */
    BL_ERR              = -1,   /**< 通用失败 */
    BL_ERR_PARAM        = -2,   /**< 参数非法 */
    BL_ERR_TIMEOUT      = -3,   /**< 超时 */
    BL_ERR_FLASH        = -4,   /**< Flash 操作失败 */
    BL_ERR_CRC          = -5,   /**< CRC 校验失败 */
    BL_ERR_PROTOCOL     = -6,   /**< 协议帧错误 */
    BL_ERR_NO_SPACE     = -7,   /**< 空间不足 */
    BL_ERR_STATE        = -8,   /**< 状态不允许该操作 */
    BL_ERR_CANCELLED    = -9,   /**< 被 CAN 取消 */
} bl_status_t;

/* ========================================================================
 * 固件状态机
 *
 * 用魔数而非 0/1/2 顺序值，是为了与 Flash 擦除态（0xFFFFFFFF）
 * 及误写入的随机数据区分开。
 * ======================================================================*/
typedef enum {
    /** 无有效固件（首次烧录、或已被判定作废） */
    BL_FW_INVALID   = 0x494E564CUL,   /* "INVL" */
    /** 升级进行中：已擦除或正在传输，绝不可跳转 */
    BL_FW_DOWNLOAD  = 0x444C4F41UL,   /* "DLOA" */
    /** 新固件已通过校验，等待 APP 自检确认（可跳转，但会回滚） */
    BL_FW_TESTING   = 0x54455354UL,   /* "TEST" */
    /** 已确认可用（正常状态） */
    BL_FW_VALID     = 0x56414C44UL,   /* "VALD" */
    /** 已被回滚作废，等待重刷 */
    BL_FW_REVOKED   = 0x5245564BUL,   /* "REVK" */
    /** 擦除态，等同无效 */
    BL_FW_ERASED    = 0xFFFFFFFFUL,
} bl_fw_state_t;

/** 判断某状态是否允许跳转到 APP */
static inline bool bl_fw_state_bootable(bl_fw_state_t s)
{
    return (s == BL_FW_VALID || s == BL_FW_TESTING);
}

/* ========================================================================
 * 升级会话结果
 * ======================================================================*/
typedef enum {
    BL_IAP_IDLE         = 0,   /**< 尚未开始 */
    BL_IAP_RUNNING      = 1,   /**< 正在传输 */
    BL_IAP_DONE         = 2,   /**< 传输并校验成功 */
    BL_IAP_FAILED       = 3,   /**< 失败（超时/CRC/协议） */
    BL_IAP_ABORTED      = 4,   /**< 被上位机主动取消 */
    BL_IAP_NO_SPACE     = 5,   /**< 固件超出 APP 区 */
} bl_iap_result_t;

#endif /* BL_TYPES_H */
