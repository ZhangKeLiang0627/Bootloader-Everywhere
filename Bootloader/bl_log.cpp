// 轻量日志实现。只调 bl_port.h 的 uartWrite，不认识任何芯片厂商头文件。

#include "bl_log.h"
#include "bl_port.h"

#include <cstdarg>
#include <cstdint>

namespace bl {
namespace log {

namespace {

#if BL_DEBUG_LOG

constexpr uint32_t kLineBufSize = 128U;

char     gBuf[kLineBufSize];
uint32_t gLen   = 0U;
bool     gMuted = false;

void flush() noexcept
{
    if (gLen > 0U) {
        (void)uartWrite(reinterpret_cast<const uint8_t*>(gBuf), gLen);
        gLen = 0U;
    }
}

void put(char c) noexcept
{
    if (gLen >= kLineBufSize) {
        flush();
    }
    gBuf[gLen++] = c;
}

// 只支持十进制与十六进制 + 可选补零宽度，覆盖全库实际用到的四种写法
void putNumber(uint32_t v, bool hex, uint32_t width, bool zeroPad) noexcept
{
    const uint32_t base   = hex ? 16U : 10U;
    const char*    digits = hex ? "0123456789ABCDEF" : "0123456789";
    char     tmp[11];
    uint32_t n = 0U;

    if (v == 0U) {
        tmp[n++] = '0';
    } else {
        while (v != 0U) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }

    for (uint32_t fill = (width > n) ? (width - n) : 0U; fill > 0U; --fill) {
        put(zeroPad ? '0' : ' ');
    }
    while (n > 0U) {
        put(tmp[--n]);
    }
}

#endif // BL_DEBUG_LOG

} // namespace

void mute(bool on) noexcept
{
#if BL_DEBUG_LOG && !BL_LOG_DURING_TRANSFER
    gMuted = on;
#else
    (void)on;
#endif
}

void printf(const char* fmt, ...) noexcept
{
#if BL_DEBUG_LOG
    if (fmt == nullptr || gMuted) {
        return;
    }

    gLen = 0U;

    va_list ap;
    va_start(ap, fmt);

    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            put(*p);
            continue;
        }

        ++p;                                        // 跳过 '%'

        bool zeroPad = false;
        if (*p == '0') {
            zeroPad = true;
            ++p;
        }

        uint32_t width = 0U;
        while (*p >= '0' && *p <= '9') {
            width = width * 10U + static_cast<uint32_t>(*p - '0');
            ++p;
        }

        bool isLong = false;
        if (*p == 'l' || *p == 'L') {
            isLong = true;
            ++p;
        }

        if (*p == 'd') {
            int32_t v = isLong ? static_cast<int32_t>(va_arg(ap, long))
                               : static_cast<int32_t>(va_arg(ap, int));
            if (v < 0) {
                put('-');
                v = -v;
            }
            putNumber(static_cast<uint32_t>(v), false, width, zeroPad);
        } else if (*p == 'u') {                     // %lu 依赖这一支，别删
            const uint32_t v = isLong ? static_cast<uint32_t>(va_arg(ap, unsigned long))
                                      : static_cast<uint32_t>(va_arg(ap, unsigned int));
            putNumber(v, false, width, zeroPad);
        } else if (*p == 'X') {
            const uint32_t v = isLong ? static_cast<uint32_t>(va_arg(ap, unsigned long))
                                      : static_cast<uint32_t>(va_arg(ap, unsigned int));
            putNumber(v, true, width, zeroPad);
        } else if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            while (s != nullptr && *s != '\0') {
                put(*s++);
            }
        } else if (*p == '%') {
            put('%');
        } else if (*p == '\0') {
            break;
        } else {
            put('%');                               // 未知转换符原样输出，便于发现写错的格式串
            put(*p);
        }
    }

    va_end(ap);
    flush();
#else
    (void)fmt;
#endif
}

} // namespace log
} // namespace bl
