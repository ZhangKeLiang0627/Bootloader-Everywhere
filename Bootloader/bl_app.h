/**
 * @file    bl_app.h
 * @brief   APP 侧「请求进入 Bootloader」的最小接口
 *
 * 这是 APP 与 Bootloader 之间唯一需要对齐的一行式接入。APP 检测到升级
 * 指令后调一次 bl_request_update()：写 RAM 标志 + 软件复位，Bootloader
 * 上电看到标志即进入 IAP（不等待、不抢时间窗）。
 *
 * 极简：不依赖 Bootloader 库的其它任何文件，只依赖 CMSIS（Cortex-M，
 * STM32 / GD32 / CH32 通用）。因此 APP 侧几乎零侵入。
 *
 * 用法：
 *   #include "bl_app.h"                       // 路径按实际调整
 *   ...
 *   if (收到升级指令) bl_request_update();
 *
 * 注意：
 *   - BL_UPDATE_REQ_ADDR / BL_UPDATE_REQ_MAGIC 必须与 Bootloader 的
 *     bl_config.h 保持一致。
 *   - 该地址默认取片内 SRAM 末尾 - 4，写完立即复位，实际不会与栈冲突；
 *     换芯片时按 SRAM 容量调整（见下）。
 *   - 调用前需已 include 芯片的 CMSIS 头（提供 __DSB / NVIC_SystemReset）。
 */
#ifndef BL_APP_H
#define BL_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 与 Bootloader/bl_config.h 保持一致。
 * 默认按 STM32F401（96KB SRAM @0x20000000）的末尾 - 4：
 *   96KB   → 0x20017FFC
 *   128KB  → 0x2001FFFC   （F405/F407）
 */
#ifndef BL_UPDATE_REQ_ADDR
#define BL_UPDATE_REQ_ADDR         0x20017FFCUL
#endif
#ifndef BL_UPDATE_REQ_MAGIC
#define BL_UPDATE_REQ_MAGIC        0xB007B007UL
#endif

/**
 * @brief 请求进入 Bootloader：写 RAM 标志并软复位。本函数不返回。
 */
static inline void bl_request_update(void)
{
    volatile uint32_t* req = (volatile uint32_t*)BL_UPDATE_REQ_ADDR;
    *req = BL_UPDATE_REQ_MAGIC;
    __DSB();              /* 确保标志落进 RAM，再触发复位 */
    NVIC_SystemReset();   /* SYSRESETREQ：SRAM 内容保留，标志可穿透 */
    for (;;) { }          /* 兜底，理论上走不到 */
}

#ifdef __cplusplus
}
#endif

#endif /* BL_APP_H */
