/**
 * @file    bl_app.h
 * @brief   APP 侧「请求进入 Bootloader」的最小接口
 *
 * 这是 APP 与 Bootloader 之间唯一需要对齐的一行式接入。APP 检测到升级
 * 指令（如串口收到 BL_BOOT_MAGIC_STRING 关键字）后调一次 bl_request_update()：
 * 只做软件复位，不写任何标志。Bootloader 靠复位原因识别「软件复位 + 固件
 * Valid 态」进入限时升级窗口（与 OpenBLT 的软复位后门同源，零等待、零侵入）。
 *
 * 极简：不依赖 Bootloader 库的其它任何文件，只依赖 CMSIS（Cortex-M，
 * STM32 / GD32 / CH32 通用）。因此 APP 侧几乎零侵入。
 *
 * 用法：
 *   #include "bl_app.h"                       // 路径按实际调整
 *   ...
 *   if (检测到升级指令) bl_request_update();
 *
 * 注意：
 *   - 调用前需已 include 芯片的 CMSIS 头（提供 NVIC_SystemReset）。
 *   - 软件复位后 Bootloader 只对「Valid 态」的固件开限时窗口；若 APP 仍
 *     处于 Testing 态（升级后首次运行），唤回需先上电/按复位转 Valid。
 */
#ifndef BL_APP_H
#define BL_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 升级唤醒关键字：上位机（网页）发给 APP 以触发「进入 Bootloader」。
 * APP 应逐字节匹配这个完整字符串（状态机），匹配成功才软复位。
 *
 * Bootloader 本身不检测它 —— Bootloader 靠复位原因识别软件复位。
 */
#ifndef BL_BOOT_MAGIC_STRING
#define BL_BOOT_MAGIC_STRING        "#Bootloader-Everywhere"
#endif

/**
 * @brief 请求进入 Bootloader：软件复位。本函数不返回。
 *
 * Bootloader 读到复位原因 = 软件复位、且固件为 Valid 态时，进入限时升级
 * 窗口；窗口内（默认 15s）等上位机 YMODEM，超时跳回 APP。
 */
static inline void bl_request_update(void)
{
    NVIC_SystemReset();   /* SYSRESETREQ：触发软件复位，SRAM 内容保留 */
    for (;;) { }          /* 兜底，理论上走不到 */
}

#ifdef __cplusplus
}
#endif

#endif /* BL_APP_H */
