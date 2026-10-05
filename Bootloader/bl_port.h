// 移植契约：库与芯片之间唯一的耦合面。换芯片就实现这 13 个函数。
//
// 库不做任何初始化 —— 时钟、串口、Flash 接口时钟都由宿主工程负责，
// 本层只提供「操作」。已经有一份现成的 STM32F4 实现：bl_port_stm32f4.cpp。
//
// 三条约定：
//   · 失败一律用 Status 返回，不用异常、不用动态内存
//   · flashErase 必须拒绝擦除 Bootloader 自身区域（最后一道保护）
//   · 所有「等标志位」的循环都要带超时，硬件异常时也不能死等
#ifndef BL_PORT_H
#define BL_PORT_H

#include "bl.h"

namespace bl {

// 复位原因。只保留判定用得到的几种，其余（欠压/低功耗等）归入 Unknown，
// 一律按「不是软件复位」处理。
enum class ResetCause : uint32_t {
    Unknown  = 0,
    PowerOn,        // 上电、掉电复位
    Pin,            // NRST 引脚复位（用户按复位键）
    Software,       // 软件复位（APP 主动触发的那种）
};

// ---- Flash ----
uint32_t flashSectorSize(uint32_t addr) noexcept;        // 含 addr 的扇区大小；0 = 地址非法
uint32_t flashBytesToSectorEnd(uint32_t addr) noexcept;  // addr 到下一扇区边界的字节数
Status   flashErase(uint32_t addr, uint32_t len) noexcept;   // 整扇区擦除（addr/len 已对齐）
Status   flashWrite(uint32_t addr, const void* data, uint32_t len) noexcept;
Status   flashRead(uint32_t addr, void* buf, uint32_t len) noexcept;

// ---- 串口（宿主已初始化好，本层只收发）----
// uartRead 的 timeoutMs 是「总超时」而不是「每字节超时」
Status uartRead(uint8_t* buf, uint32_t len, uint32_t timeoutMs, uint32_t* outRead) noexcept;
Status uartWrite(const uint8_t* buf, uint32_t len) noexcept;
void   uartFlushRx() noexcept;                            // 清空接收缓冲与溢出标志

// ---- 时基 ----
uint32_t tickMs() noexcept;
void     delayMs(uint32_t ms) noexcept;

// ---- 复位与跳转 ----
// 标志位是累积的 → 实现必须「读后即清」，否则「软件复位」会一直粘住。
// 软件复位必须优先于引脚复位：在线探针连着时两者会同时置位。
ResetCause resetCause() noexcept;

// 上电时是否要求强制留在 IAP —— 按住硬件按钮上电 = 不进 APP。
// 读一次引脚电平即可，没有时间窗，不拖慢正常启动。板子上没按钮的平台直接 return false;
// ⚠️ 读之前必须确认该 GPIO 端口时钟已开（宿主 MX_GPIO_Init 负责把引脚配成输入 + 上拉）：
//    时钟没开时读 IDR 恒为 0，会被误判成「按住」→ 每次上电都进 IAP、APP 起不来。
//    故时钟没开应按「没按」处理：宁可按键失效，也不能让 APP 永远起不来。
bool bootPinHeld() noexcept;

// 跳转到 APP。Cortex-M 上这八步顺序不能乱：
//   关中断 → 停 SysTick → 复位 RCC → 清 NVIC
//   → 设 SCB->VTOR → 设 MSP → 设 CONTROL=0 → 跳到入口
// 正常不返回。（RISC-V 内核请查手册，向量表机制不同）
void jumpToApp(uint32_t appBase) noexcept;

} // namespace bl

#endif /* BL_PORT_H */
