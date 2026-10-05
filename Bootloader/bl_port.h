/**
 * @file    bl_port.h
 * @brief   移植契约 —— 库与芯片之间唯一的耦合面（换芯片就实现这些函数）
 *
 * bl.cpp 只调用本文件声明的函数，看不到任何芯片细节。
 * 已经有一份现成的 STM32F4 实现：bl_port_stm32f4.cpp，照着写就行。
 *
 * ---------------------------------------------------------------------------
 *  移植步骤
 * ---------------------------------------------------------------------------
 *   1. 复制 bl_port_stm32f4.cpp 为 bl_port_<你的平台>.cpp
 *   2. 改它顶部的「板级配置」区（串口用哪一路、哪几个脚）
 *   3. 照下面的注释逐项实现这些函数
 *   4. bl.cpp 一个字都不用改；改配置去 bl_config.h
 *
 * ---------------------------------------------------------------------------
 *  实现要点
 * ---------------------------------------------------------------------------
 *   · 全部用 Status 返回失败，不要用异常 / 动态内存
 *   · flash_erase 必须拒绝擦除 Bootloader 自身区域（最后一道保护）
 *   · reset_cause 必须「读后即清」，且软件复位要优先于引脚复位
 *     （在线探针会连带拉 NRST，两者会同时置位）
 *   · 所有「等标志位」的循环都要带超时，硬件异常时也不能死等
 */
#ifndef BL_PORT_H
#define BL_PORT_H

#include "bl.h"

namespace bl {

/* ==========================================================================
 * 复位原因 —— 「APP 唤回 Bootloader」就是靠它识别
 * ==========================================================================*/
enum class ResetCause : uint32_t {
    Unknown = 0,   ///< 无法判定
    PowerOn,       ///< 上电 / 掉电复位
    Pin,           ///< NRST 引脚复位（用户按复位键）
    Software,      ///< 软件复位（APP 调 bl_request_update 触发的那种）
    BrownOut,      ///< 欠压复位
    LowPower,      ///< 低功耗模式复位
};

/* ==========================================================================
 * 移植函数（照 bl_port_stm32f4.cpp 的实现填）
 * ==========================================================================*/

/* ---- 平台 ---- */

/// 建立运行环境：HAL/时钟/1ms 时基/调试串口引脚。可重复调用
Status platform_init() noexcept;

/* ---- Flash ---- */

Status   flash_init() noexcept;                              ///< 解锁 Flash、开接口时钟
uint32_t flash_sector_size(uint32_t addr) noexcept;          ///< 含 addr 的扇区大小；0=地址非法
uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept;  ///< addr 到下一扇区边界的字节数
Status   flash_erase(uint32_t addr, uint32_t len) noexcept;  ///< 擦除（addr/len 已按扇区对齐）
Status   flash_write(uint32_t addr, const void* data, uint32_t len) noexcept;  ///< 编程写入
Status   flash_read(uint32_t addr, void* buf, uint32_t len) noexcept;          ///< 读取

/* ---- 串口 ---- */

Status uart_init(uint32_t baudrate) noexcept;                ///< 8N1，含引脚复用
Status uart_read(uint8_t* buf, uint32_t len, uint32_t timeout_ms,
                 uint32_t* out_read) noexcept;               ///< 阻塞读；超时是「总超时」
bool   uart_try_getc(uint8_t* ch) noexcept;                  ///< 非阻塞收一个字节
Status uart_write(const uint8_t* buf, uint32_t len) noexcept;///< 阻塞发送
void   uart_flush_rx() noexcept;                             ///< 清空接收缓冲与错误标志

/* ---- 系统 ---- */

/**
 * 跳转到 APP。Cortex-M 上这八步的顺序不能乱：
 *   关中断 → 停 SysTick → 复位 RCC → 清 NVIC
 *   → 设 SCB->VTOR → 设 MSP → 设 CONTROL=0 → 跳到入口
 * 正常不返回。（RISC-V 内核请查手册，向量表机制不同）
 */
void jump_to_app(uint32_t app_base) noexcept;

uint32_t tick_ms() noexcept;              ///< 上电起的毫秒数
void     delay_ms(uint32_t ms) noexcept;  ///< 毫秒延时

/// 上次复位原因。实现必须「读后即清」，否则标志会粘住导致误判
ResetCause reset_cause() noexcept;

} // namespace bl

/* ==========================================================================
 * 填空模板：实现新平台时对着这些 TODO 写
 * ==========================================================================
 *
 *  Status platform_init() noexcept;
 *      TODO 1) HAL / 芯片底座初始化
 *           2) 系统时钟（HSE 起不来要能退回内部 RC，别死在死循环里）
 *           3) 1ms 时基
 *
 *  Status flash_init() noexcept;
 *      TODO 解锁 Flash，使能接口时钟
 *
 *  uint32_t flash_sector_size(uint32_t addr) noexcept;
 *      TODO 返回包含 addr 的扇区（页）大小，地址非法返回 0
 *           注意两类 Flash：不等长扇区（STM32F4）与等长页（GD32）
 *
 *  uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept;
 *      TODO addr 到下一扇区边界的字节数（用于判断分区是否对齐）
 *
 *  Status flash_erase(uint32_t addr, uint32_t len) noexcept;
 *      TODO 整扇区擦除（addr/len 已对齐）
 *           ★ 必须拒绝擦除 Bootloader 自身区域，这是最后一道保护
 *
 *  Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept;
 *      TODO 编程写入，处理好对齐 / 补齐
 *
 *  Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept;
 *      TODO 多数 Cortex-M 是内存映射，memcpy 即可
 *
 *  Status uart_init(uint32_t baudrate) noexcept;
 *      TODO 8N1 + 引脚复用 + 使能收发
 *
 *  Status uart_read(uint8_t* buf, uint32_t len, uint32_t timeout_ms,
 *                   uint32_t* out_read) noexcept;
 *      TODO 阻塞读；语义是「总超时」而不是「每字节超时」，
 *           已读字节数写入 out_read，超时返回 Status::Timeout
 *
 *  bool uart_try_getc(uint8_t* ch) noexcept;
 *      TODO 非阻塞探测一个字节，有则写入 *ch 并返回 true
 *
 *  Status uart_write(const uint8_t* buf, uint32_t len) noexcept;
 *      TODO 阻塞发送完
 *
 *  void uart_flush_rx() noexcept;
 *      TODO 清空接收缓冲与溢出错误标志
 *
 *  void jump_to_app(uint32_t app_base) noexcept;
 *      TODO 从 app_base 的向量表取 [0] 设 MSP、取 [1] 跳到入口；
 *           跳转前把内核状态清干净（见上方注释的八步）
 *
 *  uint32_t tick_ms() noexcept;
 *      TODO 上电起的毫秒数（HAL_GetTick 或自己的 SysTick 计数）
 *
 *  void delay_ms(uint32_t ms) noexcept;
 *      TODO 毫秒延时
 *
 *  ResetCause reset_cause() noexcept;
 *      TODO 读并清除复位原因标志。
 *           标志是累积的 → 必须读后即清，否则「软件复位」会一直粘住
 *           ★ 软件复位必须优先于引脚复位：探针连着时两者会同时置位
 *           没有复位原因寄存器就返回 Unknown（那就用不了唤回窗口）
 */
#endif /* BL_PORT_H */
