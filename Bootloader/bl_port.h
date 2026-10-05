/**
 * @file    bl_port.h
 * @brief   移植契约 —— 库与芯片之间唯一的耦合面（换芯片就实现这 15 个函数）
 *
 * bl.cpp 只调用本文件声明的函数，看不到任何芯片细节。
 * 已经有一份现成的 STM32F4 实现：bl_port_stm32f4.cpp，照着写就行。
 *
 * ---------------------------------------------------------------------------
 *  移植步骤
 * ---------------------------------------------------------------------------
 *   1. 复制 bl_port_stm32f4.cpp 为 bl_port_<你的平台>.cpp
 *   2. 改它顶部的「板级配置」区（串口用哪一路、哪几个脚）
 *   3. 照下面的声明逐项实现
 *   4. bl.cpp 一个字都不用改；改分区去 bl_config.h
 *
 * ---------------------------------------------------------------------------
 *  三条铁律
 * ---------------------------------------------------------------------------
 *   · 全部用 Status 返回失败，不用异常、不用动态内存
 *   · flashErase 必须拒绝擦除 Bootloader 自身区域（最后一道保护）
 *   · 所有「等标志位」的循环都要带超时，硬件异常时也不能死等
 */
#ifndef BL_PORT_H
#define BL_PORT_H

#include "bl.h"

namespace bl {

/* ==========================================================================
 * 复位原因 —— 「APP 唤回 Bootloader」就是靠它识别
 *
 * 只保留判定用得到的几种；其余复位（欠压、低功耗等）统一归到 Unknown，
 * decide() 一律按「不是软件复位」处理，不区分。
 * ==========================================================================*/
enum class ResetCause : uint32_t {
    Unknown = 0,   ///< 无法判定 / 非软件复位
    PowerOn,       ///< 上电、掉电复位
    Pin,           ///< NRST 引脚复位（用户按复位键）
    Software,      ///< 软件复位（APP 调 blRequestUpdate 触发的那种）
};

/* ==========================================================================
 * 移植函数
 * ==========================================================================*/

/// 芯片底座：使能 FPU → 系统时钟 → 1ms 时基。可重复调用
Status chipInit() noexcept;

/// 解锁 Flash、使能接口时钟
Status flashInit() noexcept;

/// 含 addr 的扇区（页）大小；0 = 地址非法。
/// 注意两类 Flash：不等长扇区（STM32F4）与等长页（多数国产芯片）
uint32_t flashSectorSize(uint32_t addr) noexcept;

/// addr 到下一扇区边界的字节数（用于判断分区是否对齐）
uint32_t flashBytesToSectorEnd(uint32_t addr) noexcept;

/**
 * 整扇区擦除（addr/len 已对齐）。
 * @warning 必须拒绝擦除 Bootloader 自身区域，这是最后一道保护
 */
Status flashErase(uint32_t addr, uint32_t len) noexcept;

/// 编程写入，处理好对齐 / 补齐
Status flashWrite(uint32_t addr, const void* data, uint32_t len) noexcept;

/// 读取（多数 Cortex-M 是内存映射，memcpy 即可）
Status flashRead(uint32_t addr, void* buf, uint32_t len) noexcept;

/// 8N1 + 引脚复用 + 使能收发
Status uartInit(uint32_t baudrate) noexcept;

/**
 * 阻塞读。
 * @param timeoutMs 语义是「总超时」而不是「每字节超时」
 * @param outRead   已读字节数写回这里
 * @return 收满返回 Ok，超时返回 Status::Timeout
 */
Status uartRead(uint8_t* buf, uint32_t len, uint32_t timeoutMs,
                uint32_t* outRead) noexcept;

/// 阻塞发送完
Status uartWrite(const uint8_t* buf, uint32_t len) noexcept;

/// 清空接收缓冲与溢出错误标志
void uartFlushRx() noexcept;

/**
 * 跳转到 APP。Cortex-M 上这八步的顺序不能乱：
 *   关中断 → 停 SysTick → 复位 RCC → 清 NVIC
 *   → 设 SCB->VTOR → 设 MSP → 设 CONTROL=0 → 跳到入口
 * 正常不返回。（RISC-V 内核请查手册，向量表机制不同）
 */
void jumpToApp(uint32_t appBase) noexcept;

/// 上电起的毫秒数
uint32_t tickMs() noexcept;

/// 毫秒延时
void delayMs(uint32_t ms) noexcept;

/**
 * 上次复位原因。
 * @warning 标志是累积的 → 实现必须「读后即清」，否则「软件复位」会一直粘住
 * @warning 软件复位必须优先于引脚复位：在线探针连着时两者会同时置位
 * @note    没有复位原因寄存器就返回 Unknown（那就用不了唤回窗口）
 */
ResetCause resetCause() noexcept;

} // namespace bl

#endif /* BL_PORT_H */
