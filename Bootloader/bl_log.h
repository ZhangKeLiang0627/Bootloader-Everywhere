// 轻量日志：不依赖 stdio，只支持 %u %d %X %s %%（可带 0 与宽度修饰，如 %08lX）。
//
// 与协议共用同一个串口，所以传输期间会被 mute() 静音。
// 为什么不用 stdio：标准 vsnprintf 会连带浮点格式化吃掉约 6.5KB，
// 而 Bootloader 只有 16KB 可用；这里的实测开销约 3.6KB（BL_DEBUG_LOG=1）。
#ifndef BL_LOG_H
#define BL_LOG_H

// ---- 开关 ----

// 调试输出。ROM 占用最大的开关（开约 3.6KB、关约 0）。
#ifndef BL_DEBUG_LOG
#define BL_DEBUG_LOG            1
#endif

// 传输期间是否仍打日志。默认关：日志与协议共用同一个串口，开着会污染上位机的接收流。
// 排查协议问题时临时置 1。
#ifndef BL_LOG_DURING_TRANSFER
#define BL_LOG_DURING_TRANSFER  0
#endif

#ifdef __cplusplus

namespace bl {
namespace log {

// 格式化输出。只支持 %u %d %X %s %%，可带 0 与宽度修饰（如 %08lX）。不支持浮点。
void printf(const char* fmt, ...) noexcept;

// 传输期间静音 / 恢复。BL_LOG_DURING_TRANSFER = 1 时本函数是空操作。
void mute(bool on) noexcept;

} // namespace log
} // namespace bl

#if BL_DEBUG_LOG
    #define BL_LOG(...)         do { ::bl::log::printf(__VA_ARGS__); } while (0)
#else
    #define BL_LOG(...)         do { } while (0)
#endif

#endif /* __cplusplus */

#endif /* BL_LOG_H */
