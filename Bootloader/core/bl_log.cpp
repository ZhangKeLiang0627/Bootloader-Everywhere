/**
 * @file    bl_log.cpp
 * @brief   轻量日志实现（自写格式化，不依赖 stdio）
 */
#include <cstdarg>

#include "core/bl_log.hpp"
#include "port/bl_port.hpp"
#include "bl_config.h"

namespace bl {
namespace log {

namespace {

/* 行缓冲：攒满一行再一次性发出，避免逐字符调用串口带来的开销 */
constexpr uint32_t kLineBufSize = 128U;
char     g_buf[kLineBufSize];
uint32_t g_len = 0;

void flush() noexcept
{
    if (g_len > 0U) {
        (void)uart_write(reinterpret_cast<const uint8_t*>(g_buf), g_len);
        g_len = 0U;
    }
}

void put(char c) noexcept
{
    if (g_len >= kLineBufSize) {
        flush();
    }
    g_buf[g_len++] = c;
}

/**
 * @brief 输出一个整数（含符号、补零、宽度处理）
 * @param value    绝对值
 * @param base     进制（10 / 16）
 * @param negative 是否为负数
 */
void put_number(uint32_t value, uint32_t base, bool upper,
                uint32_t width, bool zero_pad, bool negative) noexcept
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[11];                       /* 32 位十进制最多 10 位 */
    uint32_t n = 0;

    if (value == 0U) {
        tmp[n++] = '0';
    } else {
        while (value != 0U) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }

    /* 数字位数 + 符号位 */
    const uint32_t digits_total = n + (negative ? 1U : 0U);

    /* 补零时符号要写在最前面，所以先单独输出 */
    if (zero_pad && negative) {
        put('-');
        negative = false;
    }

    /* 填充到指定宽度 */
    for (uint32_t fill = (width > digits_total) ? (width - digits_total) : 0U;
         fill > 0U; --fill) {
        put(zero_pad ? '0' : ' ');
    }

    /* 非补零情形下符号在此输出 */
    if (negative) {
        put('-');
    }

    /* 数字是逆序生成的，倒着吐出来 */
    while (n > 0U) {
        put(tmp[--n]);
    }
}

void put_string(const char* s, uint32_t width, bool zero_pad) noexcept
{
    if (s == nullptr) {
        s = "(null)";
    }
    uint32_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    for (uint32_t fill = (width > len) ? (width - len) : 0U; fill > 0U; --fill) {
        put(zero_pad ? '0' : ' ');
    }
    for (uint32_t i = 0; i < len; ++i) {
        put(s[i]);
    }
}

} // namespace

/* ========================================================================
 * 格式化输出
 * ======================================================================*/
void printf(const char* fmt, ...) noexcept
{
#if BL_DEBUG_LOG
    if (fmt == nullptr) {
        return;
    }

    g_len = 0;

    va_list ap;
    va_start(ap, fmt);

    for (const char* p = fmt; *p != '\0'; ++p) {

        if (*p != '%') {
            put(*p);
            continue;
        }

        ++p;    /* 跳过 '%' */

        /* ---- 标志 ---- */
        bool zero_pad = false;
        if (*p == '0') {
            zero_pad = true;
            ++p;
        }

        /* ---- 宽度 ---- */
        uint32_t width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10U + static_cast<uint32_t>(*p - '0');
            ++p;
        }

        /* ---- 长度修饰 ---- */
        bool is_long = false;
        if (*p == 'l' || *p == 'L') {
            is_long = true;
            ++p;
        }

        /* ---- 转换符 ---- */
        switch (*p) {

        case 'd':
        case 'i': {
            const int32_t v = is_long
                                  ? static_cast<int32_t>(va_arg(ap, long))
                                  : static_cast<int32_t>(va_arg(ap, int));
            const bool neg = (v < 0);
            const uint32_t mag = neg
                                     ? static_cast<uint32_t>(-(static_cast<int64_t>(v)))
                                     : static_cast<uint32_t>(v);
            put_number(mag, 10U, false, width, zero_pad, neg);
            break;
        }

        case 'u': {
            const uint32_t v = is_long
                                   ? static_cast<uint32_t>(va_arg(ap, unsigned long))
                                   : static_cast<uint32_t>(va_arg(ap, unsigned int));
            put_number(v, 10U, false, width, zero_pad, false);
            break;
        }

        case 'x':
        case 'X': {
            const uint32_t v = is_long
                                   ? static_cast<uint32_t>(va_arg(ap, unsigned long))
                                   : static_cast<uint32_t>(va_arg(ap, unsigned int));
            put_number(v, 16U, (*p == 'X'), width, zero_pad, false);
            break;
        }

        case 's':
            put_string(va_arg(ap, const char*), width, zero_pad);
            break;

        case 'c':
            put(static_cast<char>(va_arg(ap, int)));
            break;

        case '%':
            put('%');
            break;

        case '\0':
            /* 格式串以 '%' 结尾，直接收尾 */
            goto done;

        default:
            /* 未知转换符：原样输出，便于发现写错的格式串 */
            put('%');
            put(*p);
            break;
        }
    }

done:
    va_end(ap);
    flush();
#else
    (void)fmt;
#endif
}

void puts(const char* s) noexcept
{
#if BL_DEBUG_LOG
    if (s == nullptr) {
        return;
    }
    g_len = 0;
    while (*s != '\0') {
        put(*s++);
    }
    flush();
#else
    (void)s;
#endif
}

void newline() noexcept
{
#if BL_DEBUG_LOG
    g_len = 0;
    put('\r');
    put('\n');
    flush();
#endif
}

} // namespace log
} // namespace bl
