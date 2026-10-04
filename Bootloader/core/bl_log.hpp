/**
 * @file    bl_log.hpp
 * @brief   轻量串口日志 —— 不依赖标准库 printf
 *
 * 为什么不直接用 vsnprintf：
 *   实测（AC6 / -Os）链接一个 vsnprintf 会额外引入约 6.5KB 的
 *   格式化与浮点库代码（`_printf_wctomb` / `btod` / `bigflt0` 等），
 *   而 Bootloader 总共只有 16KB 可用。这对一个「只想打印几个整数」
 *   的调试输出而言代价过高。
 *
 * 本实现只支持 Bootloader 实际用到的格式：
 *   %d %i  有符号十进制      %u   无符号十进制
 *   %x %X  十六进制          %s   字符串
 *   %c     字符              %%   百分号
 *   l 长度修饰（long）       0 与数字宽度（如 %08lX 补零到 8 位）
 * 不支持浮点、不支持 * 动态宽度 —— 这些 bootloader 用不到。
 *
 * 因此本模块放在 core/ 而不是 port/：它是平台无关逻辑，
 * 移植到新芯片时不需要重新实现（只需 port 层提供 uart_write）。
 */
#ifndef BL_LOG_HPP
#define BL_LOG_HPP

#include <cstdint>

namespace bl {
namespace log {

/// 格式化输出（见文件头支持的格式集）
void printf(const char* fmt, ...) noexcept;

/// 直接输出一个以 '\0' 结尾的字符串
void puts(const char* s) noexcept;

/// 输出一个换行
void newline() noexcept;

} // namespace log
} // namespace bl

#endif /* BL_LOG_HPP */
