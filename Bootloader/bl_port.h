// 移植契约：库与芯片之间唯一的耦合面。换芯片就写这 12 个函数 + 一张扇区表。
//
// 库不做任何外设初始化 —— 时钟、串口、Flash 接口时钟、按钮引脚都由宿主工程负责，
// 本层只提供「操作」。现成的 STM32F4 实现见 bl_port_stm32f4.cpp。
//
// 三条约定：
//   · 失败一律用 Status 返回，不用异常、不用动态内存
//   · flashErase 必须拒绝擦除 Bootloader 自身区域（最后一道保护）
//   · 所有「等标志位」的循环都要带超时，硬件异常时也不能死等
#ifndef BL_PORT_H
#define BL_PORT_H

#include "bl.h"

namespace bl {

// 复位原因。只保留判定用得到的几种，其余（欠压 / 低功耗等）一律按「不是软件复位」处理。
enum class ResetCause : uint32_t {
    Unknown  = 0,
    PowerOn,
    Pin,            // NRST 引脚复位
    Software,       // 软件复位（APP 主动触发的那种）
};

// ---- Flash 扇区表：芯片事实，移植时只填这张表 ----
// 用表而不是算式：F4 的扇区不等长（16KB×4 + 64KB + 128KB×N），列成表地址一眼可查。
struct FlashSector {
    uint32_t base;
    uint32_t size;
};

// 由 port 实现提供。下标 = 擦除时的扇区号。
extern const FlashSector kFlashSectors[];
extern const uint32_t    kFlashSectorCount;

// 返回包含 addr 的扇区；不在表内返回 nullptr
inline const FlashSector* flashSectorAt(uint32_t addr) noexcept
{
    for (uint32_t i = 0U; i < kFlashSectorCount; ++i) {
        const FlashSector& s = kFlashSectors[i];
        if (addr >= s.base && addr < (s.base + s.size)) {
            return &s;
        }
    }
    return nullptr;
}

// ---- Flash ----
Status flashErase(uint32_t addr, uint32_t len) noexcept;   // addr / len 必须按扇区对齐
Status flashWrite(uint32_t addr, const void* data, uint32_t len) noexcept;
Status flashRead(uint32_t addr, void* buf, uint32_t len) noexcept;

// ---- 串口 ----
// 宿主把 USART 配好（8N1、波特率），库自己开接收中断并从接收缓冲取字节。
// uartRead 的 timeoutMs 是「总超时」（必须 > 0），不是「每字节超时」。
Status uartRead(uint8_t* buf, uint32_t len, uint32_t timeoutMs, uint32_t* outRead) noexcept;
Status uartWrite(const uint8_t* buf, uint32_t len) noexcept;
void   uartFlushRx() noexcept;      // 清接收缓冲与溢出标志，并使能接收中断
void   uartRxIrqHandler() noexcept; // 在宿主的 USARTx_IRQHandler 里调用，不要同时调 HAL_UART_IRQHandler

// ---- 时基 ----
uint32_t tickMs() noexcept;
void     delayMs(uint32_t ms) noexcept;

// ---- 复位与跳转 ----
// 标志位是累积的 → 实现必须「读后即清」，否则「软件复位」会一直粘住。
// 软件复位必须优先于引脚复位：在线探针连着时两者会同时置位。
ResetCause resetCause() noexcept;

// 上电时是否要求强制留在 IAP —— 按住硬件按钮上电 = 不进 APP。
// 没有按钮的平台直接 return false。
// ⚠️ 读之前必须确认该 GPIO 端口时钟已开：时钟没开时读 IDR 恒为 0，会被误判成「按住」，
//    于是每次上电都进 IAP。故时钟没开一律按「没按」处理。
bool bootPinHeld() noexcept;

// 跳转到 APP（Cortex-M 上的八步顺序见实现）。正常不返回。
void jumpToApp(uint32_t appBase) noexcept;

} // namespace bl

#endif /* BL_PORT_H */
