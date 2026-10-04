/**
 * @file    bl_port.hpp
 * @brief   移植契约 —— 库与芯片之间唯一的耦合面
 *
 * core/ 只调本文件声明的函数，看不到任何芯片细节。换芯片时：
 *   - 同族芯片（复用 target/）：只改 bl_config.h 几个数字
 *   - 异族芯片：复制 port/bl_port_template.cpp 到 target/<平台>/ 逐项实现
 * 细节见 docs/PORTING.md。
 */
#ifndef BL_PORT_HPP
#define BL_PORT_HPP

#include "core/bl_types.hpp"

namespace bl {

/* ---- 平台初始化 ---- */

/// 建立运行环境（HAL 底座、系统时钟、1ms 时基、调试串口引脚）。须幂等
Status platform_init() noexcept;

/* ---- Flash 驱动 ---- */

Status flash_init() noexcept;                                       ///< 初始化 Flash 访问
uint32_t flash_sector_size(uint32_t addr) noexcept;                 ///< 含 addr 的扇区大小，0=非法
uint32_t flash_bytes_to_sector_end(uint32_t addr) noexcept;         ///< addr 到下一扇区边界的字节数
Status flash_erase(uint32_t addr, uint32_t len) noexcept;           ///< 擦除（addr/len 已按扇区对齐）
Status flash_write(uint32_t addr, const void* data, uint32_t len) noexcept;  ///< 编程写入（处理对齐/补齐）
Status flash_read(uint32_t addr, void* buf, uint32_t len) noexcept; ///< 读取

/* ---- UART 驱动 ---- */

Status uart_init(uint32_t baudrate) noexcept;                       ///< 初始化串口 8N1
Status uart_read(uint8_t* buf, uint32_t len, uint32_t timeout_ms,
                 uint32_t* out_read) noexcept;                      ///< 阻塞读（总超时）
bool uart_try_getc(uint8_t* ch) noexcept;                           ///< 非阻塞探测单字节
Status uart_write(const uint8_t* buf, uint32_t len) noexcept;       ///< 阻塞写
void uart_flush_rx() noexcept;                                      ///< 清空接收缓冲

/* ---- 系统控制 ---- */

/**
 * 跳转 APP。Cortex-M 八步顺序不可乱：关中断 → 停 SysTick → 复位 RCC
 * → 清 NVIC → 设 SCB->VTOR → 设 MSP → 设 CONTROL → 调入口。正常不返回。
 */
void jump_to_app(uint32_t app_base) noexcept;

uint32_t tick_ms() noexcept;          ///< 上电起毫秒时基
void delay_ms(uint32_t ms) noexcept;  ///< 毫秒延时

/// 上次复位原因（用于「软件复位唤回」）。实现须读后即清
enum class ResetCause : uint32_t {
    Unknown   = 0,   ///< 无法判定
    PowerOn,         ///< 上电 / 掉电复位
    Pin,             ///< NRST 引脚复位
    Software,        ///< 软件复位
    BrownOut,        ///< 欠压复位
    LowPower,        ///< 低功耗模式复位
};
ResetCause reset_cause() noexcept;

} // namespace bl

#endif /* BL_PORT_HPP */
