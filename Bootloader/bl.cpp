/**
 * @file    bl.cpp
 * @brief   LUMOS-bootloader 的全部实现（单文件库）
 *
 * 这个文件就是整个 Bootloader，自上而下按「上电后会发生什么」排列：
 *
 *   ① 日志      轻量串口格式化（不依赖 stdio，省 ROM）
 *   ② CRC       XMODEM 帧用的 CRC16 + 整镜像校验用的 CRC32
 *   ③ 元数据    配置区的日志式槽位（记录固件状态）
 *   ④ 校验      向量表 / 整镜像 CRC32
 *   ⑤ YMODEM    串口固件接收协议
 *   ⑥ 会话      一次升级的编排（擦除 → 收 → 校验 → 提交）
 *   ⑦ 决策      上电后：跳 APP 还是留在 IAP
 *   ⑧ 入口      bl_entry() / bl_run() / main()
 *
 * 本文件不包含、也不认识任何芯片厂商的头文件 —— 所有硬件操作都通过
 * bl_port.h 声明的函数完成。这就是它能跨芯片的全部原因。
 *
 * 约束：C++11，无异常、无 RTTI、无动态内存。
 */

/* 本文件的对外声明见 bl.h；移植接口见 bl_port.h。 */


#include "bl.h"
#include "bl_port.h"
#include "bl_config.h"
#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace bl { [[noreturn]] void bl_entry() noexcept; }


namespace bl {

/* --------------------------------------------------------------------------
 * ① 日志：轻量串口输出
 * -------------------------------------------------------------------------- */



namespace log {

/// 格式化输出（见文件头支持的格式集）
void printf(const char* fmt, ...) noexcept;

/// 直接输出一个以 '\0' 结尾的字符串
void puts(const char* s) noexcept;

/// 输出一个换行
void newline() noexcept;

} // namespace log

/* --------------------------------------------------------------------------
 * ① 日志：轻量串口输出
 * -------------------------------------------------------------------------- */



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

/* --------------------------------------------------------------------------
 * ② CRC16（YMODEM 帧）/ CRC32（整镜像）
 * -------------------------------------------------------------------------- */




/* ========================================================================
 * CRC16 / XMODEM —— YMODEM 每一帧的校验
 * ======================================================================*/
class Crc16 {
public:
    static constexpr uint16_t kInit  = 0x0000U;
    static constexpr uint16_t kCheck = 0x31C3U;   ///< 标准自检值

    constexpr Crc16() noexcept : value_(kInit) {}

    /// 复位到初始值，便于复用同一个对象
    ///
    /// 注意这里不能标 constexpr：C++11 下 constexpr 成员函数隐含 const，
    /// 而它要改成员，会编译不过。本库刻意保持 C++11 可编译 ——
    /// 目标工程未必把标准调到 C++14。
    void reset() noexcept { value_ = kInit; }

    /// 增量累加
    void update(const void* data, uint32_t len) noexcept;

    constexpr uint16_t value() const noexcept { return value_; }

    /// 一次性计算（内部新建临时对象，不改变本对象状态）
    static uint16_t compute(const void* data, uint32_t len) noexcept
    {
        Crc16 c;
        c.update(data, len);
        return c.value();
    }

private:
    uint16_t value_;
};

/* ========================================================================
 * CRC32 / ISO-HDLC —— 整镜像校验
 * ======================================================================*/
class Crc32 {
public:
    static constexpr uint32_t kInit  = 0xFFFFFFFFU;
    static constexpr uint32_t kCheck = 0xCBF43926U;   ///< 标准自检值

    constexpr Crc32() noexcept : value_(kInit) {}

    /// 复位（同样不能标 constexpr，原因见 Crc16::reset）
    void reset() noexcept { value_ = kInit; }

    /// 增量累加（value_ 保存的是未做最终异或的中间值）
    void update(const void* data, uint32_t len) noexcept;

    /// 取最终结果（内部做最终异或）
    constexpr uint32_t value() const noexcept { return value_ ^ 0xFFFFFFFFU; }

    static uint32_t compute(const void* data, uint32_t len) noexcept
    {
        Crc32 c;
        c.update(data, len);
        return c.value();
    }

private:
    uint32_t value_;
};

/// 直接对 Flash 区域计算 CRC32（分块读，不占大缓冲）
uint32_t crc32_flash(uint32_t addr, uint32_t len) noexcept;

/* ========================================================================
 * 自检
 *
 * CRC 参数一旦写错，现象是「PC 算的值和板子算的对不上」，极难排查。
 * 上电时跑一次自检，能在最早时刻暴露问题。
 * ======================================================================*/
Status crc_selftest() noexcept;

/* --------------------------------------------------------------------------
 * ② CRC16（YMODEM 帧）/ CRC32（整镜像）
 * -------------------------------------------------------------------------- */



/* ========================================================================
 * CRC16 / XMODEM —— poly 0x1021，逐位实现
 *
 * 选逐位而非查表：YMODEM 每帧 1KB，逐位在 168MHz 上约 0.25ms，
 * 完全可以接受；换来的是代码更小、更容易核对正确性。
 * ======================================================================*/
void Crc16::update(const void* data, uint32_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);

    for (uint32_t i = 0; i < len; ++i) {
        value_ ^= static_cast<uint16_t>(static_cast<uint16_t>(p[i]) << 8);
        for (int b = 0; b < 8; ++b) {
            if (value_ & 0x8000U) {
                value_ = static_cast<uint16_t>((value_ << 1) ^ 0x1021U);
            } else {
                value_ = static_cast<uint16_t>(value_ << 1);
            }
        }
    }
}

/* ========================================================================
 * CRC32 / ISO-HDLC —— 半字节查表（16 项 × 4 字节 = 64B 表）
 *
 * 表项为反向多项式 0xEDB88320 的 4 位查表，
 * 一次处理 4 bit，比纯逐位快约 4 倍，而表只占 64 字节。
 * ======================================================================*/
namespace {
constexpr uint32_t kNibbleTable[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};
} // namespace

void Crc32::update(const void* data, uint32_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);

    for (uint32_t i = 0; i < len; ++i) {
        value_ ^= static_cast<uint32_t>(p[i]);
        value_ = (value_ >> 4) ^ kNibbleTable[value_ & 0x0FU];
        value_ = (value_ >> 4) ^ kNibbleTable[value_ & 0x0FU];
    }
}

/* ========================================================================
 * 直接对 Flash 区域算 CRC32
 * ======================================================================*/
uint32_t crc32_flash(uint32_t addr, uint32_t len) noexcept
{
    uint8_t buf[256];
    Crc32   crc;
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (!ok(flash_read(addr + done, buf, chunk))) {
            /* 读失败时返回哨兵值，正常 CRC 结果不可能等于它 */
            return 0xDEADBEEFU;
        }
        crc.update(buf, chunk);
        done += chunk;
    }
    return crc.value();
}

/* ========================================================================
 * 自检
 * ======================================================================*/
Status crc_selftest() noexcept
{
    static constexpr char kVector[] = "123456789";

    if (Crc16::compute(kVector, 9) != Crc16::kCheck) {
        return Status::CrcFail;
    }
    if (Crc32::compute(kVector, 9) != Crc32::kCheck) {
        return Status::CrcFail;
    }
    return Status::Ok;
}

/* --------------------------------------------------------------------------
 * ③ 元数据：配置区日志式槽位
 * -------------------------------------------------------------------------- */




class Meta {
public:
    static constexpr uint32_t kSlotMagic = 0x4C554D53UL;  ///< "LUMS"

    /// 单个槽位布局。字段顺序一经发布不可改，否则旧设备读不懂配置区
    struct Slot {
        uint32_t seq;             ///< 自增序号，判定哪槽最新
        FwState  state;           ///< 固件状态
        uint32_t fw_size;         ///< APP 有效字节数
        uint32_t fw_crc32;        ///< APP 整镜像 CRC32
        uint32_t fw_version;      ///< 固件版本号
        uint32_t magic;           ///< kSlotMagic
        uint32_t reserved[9];     ///< 预留，恒为 0
        uint32_t slot_crc32;      ///< 本槽前 60 字节的 CRC32
    };

    static_assert(sizeof(Slot) == BL_META_SLOT_SIZE,
                  "Slot 布局必须与槽位大小一致");

    static constexpr uint32_t kCrcSpan = sizeof(Slot) - sizeof(uint32_t);

    /// 扫描配置区装载当前状态；无有效槽时置 Invalid（不写盘）
    Status init() noexcept;

    const Slot& current() const noexcept { return meta_; }
    bool should_boot() const noexcept { return bootable(meta_.state); }
    FwState state() const noexcept { return meta_.state; }

    /// 置 Download，必须在擦除 APP 区前调用（防「Valid 但已擦空」的必砖组合）
    Status mark_download() noexcept;

    /// 升级完成，置 Valid 并记录 size/crc32/version
    Status commit(uint32_t size, uint32_t crc32, uint32_t version) noexcept;

    /// 擦除整个配置扇区（调试用）
    Status erase_all() noexcept;

    void dump() const noexcept;

private:
    Status write_slot(const Slot& s) noexcept;

    Slot     meta_{};
    uint32_t next_offset_ = 0;
    uint32_t seq_         = 0;
    bool     inited_      = false;
};

/// 全局唯一的配置区实例
Meta& meta() noexcept;

/* --------------------------------------------------------------------------
 * ③ 元数据：配置区日志式槽位
 * -------------------------------------------------------------------------- */




static bool is_erased(const void* p, uint32_t len) noexcept
{
    const auto* b = static_cast<const uint8_t*>(p);
    for (uint32_t i = 0; i < len; ++i) {
        if (b[i] != 0xFFU) return false;
    }
    return true;
}

static bool slot_valid(const Meta::Slot& s) noexcept
{
    return s.magic == Meta::kSlotMagic &&
           Crc32::compute(reinterpret_cast<const uint8_t*>(&s), Meta::kCrcSpan) == s.slot_crc32;
}

Status Meta::init() noexcept
{
    Slot     slot{};
    bool     found = false;
    uint32_t best_seq = 0;

    next_offset_ = 0;
    seq_         = 0;
    meta_        = Slot{};
    meta_.state  = FwState::Invalid;

    for (uint32_t off = 0; off + BL_META_SLOT_SIZE <= BL_META_SIZE; off += BL_META_SLOT_SIZE) {
        if (!ok(flash_read(BL_META_BASE + off, &slot, sizeof(slot)))) {
            return Status::FlashFail;
        }
        if (is_erased(&slot, sizeof(slot))) break;              // 顺序追加，遇空槽即止
        next_offset_ = off + BL_META_SLOT_SIZE;                  // 占位槽（含损坏的）都要跳过
        if (!slot_valid(slot)) continue;                         // 掉电残槽，跳过
        if (!found || slot.seq > best_seq) {
            meta_ = slot; best_seq = slot.seq; found = true;
        }
    }

    if (found) {
        seq_ = meta_.seq;
    } else {
        // 无有效槽：回到「无固件」起点。
        // 若扫描时已越过占位槽（说明区内有损坏/旧格式残槽），必须先擦掉
        // 整片再从头写 —— 否则从偏移 0 写入会撞上未擦除的 Flash 而失败，
        // 表现为「永远升级不进去」。
        if (next_offset_ != 0U) {
            if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) {
                return Status::FlashFail;
            }
        }
        meta_        = Slot{};
        meta_.state  = FwState::Invalid;
        seq_         = 0;
        next_offset_ = 0;
    }

    inited_ = true;
    return Status::Ok;
}

Status Meta::write_slot(const Slot& s) noexcept
{
    if (!inited_) return Status::BadState;

    // 槽位用尽：整片擦除后从头开始
    if (next_offset_ + BL_META_SLOT_SIZE > BL_META_SIZE) {
        if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) return Status::FlashFail;
        next_offset_ = 0;
    }

    Slot slot = s;
    slot.seq    = ++seq_;
    slot.magic  = kSlotMagic;
    for (uint32_t& r : slot.reserved) r = 0;
    slot.slot_crc32 = Crc32::compute(reinterpret_cast<const uint8_t*>(&slot), kCrcSpan);

    const uint32_t off = next_offset_;
    if (!ok(flash_write(BL_META_BASE + off, &slot, sizeof(slot)))) return Status::FlashFail;

    // 回读校验，确认真的落盘
    Slot verify{};
    if (!ok(flash_read(BL_META_BASE + off, &verify, sizeof(verify)))) return Status::FlashFail;
    if (std::memcmp(&verify, &slot, sizeof(slot)) != 0) return Status::FlashFail;

    next_offset_ = off + BL_META_SLOT_SIZE;
    meta_        = slot;
    return Status::Ok;
}

Status Meta::mark_download() noexcept
{
    Slot s = meta_;
    s.state    = FwState::Download;
    s.fw_size  = 0;
    s.fw_crc32 = 0;
    return write_slot(s);
}

Status Meta::commit(uint32_t size, uint32_t crc32, uint32_t version) noexcept
{
    Slot s = meta_;
    s.state      = FwState::Valid;
    s.fw_size    = size;
    s.fw_crc32   = crc32;
    s.fw_version = version;
    return write_slot(s);
}

Status Meta::erase_all() noexcept
{
    if (!inited_) return Status::BadState;
    if (!ok(flash_erase(BL_META_BASE, BL_META_SIZE))) return Status::FlashFail;
    meta_        = Slot{};
    meta_.state  = FwState::Invalid;
    next_offset_ = 0;
    seq_         = 0;
    return Status::Ok;
}

void Meta::dump() const noexcept
{
    BL_LOG("[meta] state=%s seq=%lu size=%lu crc=0x%08lX ver=%lu\r\n",
           to_string(meta_.state),
           static_cast<unsigned long>(meta_.seq),
           static_cast<unsigned long>(meta_.fw_size),
           static_cast<unsigned long>(meta_.fw_crc32),
           static_cast<unsigned long>(meta_.fw_version));
}

Meta& meta() noexcept
{
    static Meta inst;
    return inst;
}

/* --------------------------------------------------------------------------
 * ④ 校验：向量表 / 整镜像 CRC32
 * -------------------------------------------------------------------------- */




/**
 * @brief 校验 APP 向量表是否合法
 *
 * 判据：
 *   - 初始栈顶 SP 必须落在 SRAM 范围内
 *   - 复位入口必须落在 APP 区内，且最低位为 1（Thumb 状态）
 *
 * @param app_base APP 基址
 * @param out      校验明细（可为 nullptr）
 */
Status verify_vector_table(uint32_t app_base, VectorCheck* out = nullptr) noexcept;

/**
 * @brief 完整校验：向量表 + 整镜像 CRC32
 *
 * @param app_base   APP 基址
 * @param size       固件有效字节数
 * @param expect_crc 期望的 CRC32
 * @param out_crc    实际算出的 CRC32（可为 nullptr）
 */
Status verify_image(uint32_t app_base, uint32_t size,
                    uint32_t expect_crc, uint32_t* out_crc = nullptr) noexcept;

/* --------------------------------------------------------------------------
 * ④ 校验：向量表 / 整镜像 CRC32
 * -------------------------------------------------------------------------- */



/* ========================================================================
 * 向量表校验
 *
 * 这一检查极其廉价（两次 32 位读），却能挡住最常见的两类「坏固件」：
 *   - 空片：全 0xFF → SP=0xFFFFFFFF 不在 SRAM 范围
 *   - 擦到一半的扇区：向量表首字可能已被擦成 0xFF 或残值
 * ======================================================================*/
Status verify_vector_table(uint32_t app_base, VectorCheck* out) noexcept
{
    uint32_t vec[2] = {0, 0};

    if (!ok(flash_read(app_base, vec, sizeof(vec)))) {
        return Status::FlashFail;
    }

    const uint32_t initial_sp    = vec[0];
    const uint32_t reset_handler = vec[1];

    const bool sp_ok =
        (initial_sp >= BL_SRAM_BASE) && (initial_sp <= BL_SRAM_END);

    /* 复位入口须落在 APP 区内，且为 Thumb 地址（最低位为 1） */
    const bool entry_ok =
        (reset_handler >= BL_APP_BASE) &&
        (reset_handler <  (BL_APP_BASE + BL_APP_SIZE)) &&
        ((reset_handler & 0x1U) != 0U);

    if (out != nullptr) {
        out->initial_sp      = initial_sp;
        out->reset_handler   = reset_handler;
        out->sp_in_sram      = sp_ok;
        out->entry_is_thumb  = entry_ok;
    }

    return (sp_ok && entry_ok) ? Status::Ok : Status::CrcFail;
}

/* ========================================================================
 * 完整校验
 * ======================================================================*/
Status verify_image(uint32_t app_base, uint32_t size,
                    uint32_t expect_crc, uint32_t* out_crc) noexcept
{
    if (size == 0U || size > BL_APP_SIZE) {
        return Status::BadParam;
    }

    /* 先做廉价的向量表检查，不通过就没必要算 CRC */
    Status s = verify_vector_table(app_base, nullptr);
    if (!ok(s)) {
        return s;
    }

    /* CRC32 只覆盖固件原始长度 size，不做任何对齐取整。
     *
     * 这一点必须与上位机严格一致，否则校验永远失败：
     * YMODEM 按 1024 字节分包，最后一包不足时发送方会用填充字节补满
     * （不同工具的填充值可能是 0x00 / 0x1A / 0xFF），
     * 这些填充字节同样会被写进 Flash。
     * 若上位机按「原始文件字节」算 CRC、而本端按「对齐后长度」算，
     * 结果必然对不上。因此双方统一约定：CRC 只覆盖前 size 字节。 */
    const uint32_t actual = crc32_flash(app_base, size);
    if (out_crc != nullptr) {
        *out_crc = actual;
    }

    if (actual != expect_crc) {
        BL_LOG("[verify] crc mismatch: calc=0x%08lX expect=0x%08lX size=%lu\r\n",
               static_cast<unsigned long>(actual),
               static_cast<unsigned long>(expect_crc),
               static_cast<unsigned long>(size));
        return Status::CrcFail;
    }
    return Status::Ok;
}

/* --------------------------------------------------------------------------
 * ⑤ YMODEM 接收端
 * -------------------------------------------------------------------------- */




/* ========================================================================
 * 接收结果通知接口
 *
 * 以抽象类而非 std::function：不引入动态内存与额外开销，
 * 且调用点可静态解析，适合嵌入式。
 * ======================================================================*/
class YmodemSink {
public:
    virtual ~YmodemSink() = default;

    /**
     * @brief 解析出首包的文件名与大小后调用
     *
     * 这是决定「要不要接受这次升级」的唯一时机：
     * 可在此校验大小是否超出 APP 区，并完成 Flash 擦除。
     * 擦除应在此处一次性做完——此刻 PC 端正等待 ACK，
     * 数秒的擦除耗时不会触发上位机超时。
     *
     * @return true 接受，继续接收；false 拒绝，本类将发 CA 中止
     */
    virtual bool on_file_start(const char* filename, uint32_t size) = 0;

    /**
     * @brief 一个数据块到达
     * @param offset 该块在固件中的字节偏移
     * @param data   数据指针（指向内部缓冲，回调返回后即失效）
     * @param len    字节数（末尾包可能小于 1024）
     * @return true 写入成功；false 失败，本类将发 CA 中止
     */
    virtual bool on_file_data(uint32_t offset, const uint8_t* data, uint32_t len) = 0;

    /// 收到 EOT 并回 ACK 之后调用
    virtual void on_file_end(uint32_t total_bytes) = 0;
};

/* ========================================================================
 * YMODEM 接收端
 * ======================================================================*/
class Ymodem {
public:
    /* 编译期常量须先于使用它们的嵌套类型声明 */
    static constexpr uint32_t kFilenameMax = 64U;
    static constexpr uint32_t kBlockMax    = BL_YMODEM_BLOCK_SIZE;   ///< 1024

    /// 协议控制字节
    enum class Code : uint8_t {
        Soh  = 0x01,   ///< 128 字节包起始
        Stx  = 0x02,   ///< 1024 字节包起始
        Eot  = 0x04,   ///< 传输结束
        Ack  = 0x06,   ///< 肯定应答
        Nak  = 0x15,   ///< 否定应答（请求重传）
        Ca   = 0x18,   ///< 取消（连发两次生效）
        ReqC = 0x43,   ///< 'C'，请求 CRC 模式 / 请求下一包
    };

    struct Config {
        uint32_t packet_timeout_ms    = BL_YMODEM_PACKET_TIMEOUT_MS;
        uint32_t handshake_timeout_ms = BL_YMODEM_HANDSHAKE_MS;
        uint32_t max_errors           = BL_YMODEM_MAX_RETRY;   ///< 连续超时上限
        uint32_t max_nak              = BL_YMODEM_MAX_NAK;     ///< 连续 NAK 上限
    };

    struct Stats {
        char     filename[kFilenameMax] = {0};
        uint32_t file_size   = 0;   ///< 首包声明的总大小
        uint32_t received    = 0;   ///< 实际写入的字节数
        uint32_t packets     = 0;   ///< 收到的数据包个数
        uint32_t retries     = 0;   ///< 重传请求次数
        uint32_t crc_errors  = 0;   ///< 帧 CRC 错误次数
    };

    struct Outcome {
        Status status = Status::Ok;
        Stats  stats;
    };

    /**
     * 注意：这里不用「默认实参 Config{}」而拆成两个重载。
     * 原因是 C++ 的一条限制——类的默认实参不处于 complete-class context，
     * 而 Config 含默认成员初始化器，写 `= Config{}` 会编译报错
     * （在类定义内部无法求值这些初始化器）。委托构造则没有这个问题。
     */
    explicit Ymodem(YmodemSink& sink) noexcept
        : Ymodem(sink, Config{}) {}

    Ymodem(YmodemSink& sink, const Config& cfg) noexcept
        : sink_(sink), cfg_(cfg) {}

    /// 阻塞式接收一个固件文件（返回即会话结束）
    Outcome receive() noexcept;

private:
    /* 帧内索引 */
    static constexpr uint32_t kNumIndex  = 2U;
    static constexpr uint32_t kCnumIndex = 3U;
    static constexpr uint32_t kDataIndex = 4U;
    static constexpr uint32_t kBufSize   = kDataIndex + kBlockMax + 2U;   ///< 1030

    /// 单次收包结果
    enum class RecvRc : uint8_t {
        Ok,        ///< 收到完整帧，长度存入 len
        Timeout,   ///< 超时或无应答
        BadFrame,  ///< 帧头非法 / 序号反码错 / CRC 错
        Abort,     ///< 上位机发来 ABORT 字符
        Ca,        ///< 收到 CA CA（len 置 2）
        Eot,       ///< 收到 EOT（len 置 0）
    };

    RecvRc recv_packet(uint32_t& len) noexcept;
    bool   wait_end_packet() noexcept;
    bool   send_byte(Code c) noexcept;
    bool   send_abort() noexcept;
    void   parse_header(const uint8_t* data, char* name_out, uint32_t& size_out) noexcept;

    YmodemSink& sink_;
    Config      cfg_;
    Stats       stats_;
    uint8_t     buf_[kBufSize] = {0};
};

/* --------------------------------------------------------------------------
 * ⑤ YMODEM 接收端
 * -------------------------------------------------------------------------- */



/* ========================================================================
 * 底层收发
 * ======================================================================*/
bool Ymodem::send_byte(Code c) noexcept
{
    const auto b = static_cast<uint8_t>(c);
    return ok(uart_write(&b, 1));
}

bool Ymodem::send_abort() noexcept
{
    /* CAN 必须连发两次才被上位机识别为取消 */
    return send_byte(Code::Ca) && send_byte(Code::Ca);
}

/* ========================================================================
 * 解析首包中的「文件名 + 大小」
 *
 * 首包数据区格式："文件名\0" + "十进制大小" + ' ' + 填充
 * ======================================================================*/
void Ymodem::parse_header(const uint8_t* data, char* name_out, uint32_t& size_out) noexcept
{
    uint32_t i = 0;

    /* 文件名：以 '\0' 结束 */
    while (i < (kFilenameMax - 1U) && data[i] != 0x00U) {
        name_out[i] = static_cast<char>(data[i]);
        ++i;
    }
    name_out[i] = '\0';

    /* 跳过文件名终止符 */
    if (i < kBlockMax) {
        ++i;
    }

    /* 大小：十进制数字串，遇空格或非数字结束 */
    uint32_t size   = 0;
    uint32_t digits = 0;
    while (i < kBlockMax && digits < 10U &&
           data[i] >= '0' && data[i] <= '9') {
        size = size * 10U + static_cast<uint32_t>(data[i] - '0');
        ++i;
        ++digits;
    }
    size_out = size;
}

/* ========================================================================
 * 收一帧
 * ======================================================================*/
Ymodem::RecvRc Ymodem::recv_packet(uint32_t& len) noexcept
{
    len = 0;

    uint8_t  start = 0;
    uint32_t got   = 0;
    if (!ok(uart_read(&start, 1, cfg_.packet_timeout_ms, &got)) || got == 0) {
        return RecvRc::Timeout;
    }

    uint32_t pkt_size = 0;
    switch (start) {
        case static_cast<uint8_t>(Code::Soh):
            pkt_size = 128U;                 /* 128 字节小包 */
            break;

        case static_cast<uint8_t>(Code::Stx):
            pkt_size = kBlockMax;            /* 1024 字节大包 */
            break;

        case static_cast<uint8_t>(Code::Eot):
            return RecvRc::Eot;              /* len 保持 0 */

        case static_cast<uint8_t>(Code::Ca): {
            /* 取消必须成对出现 */
            uint8_t  second = 0;
            uint32_t got2   = 0;
            if (!ok(uart_read(&second, 1, cfg_.packet_timeout_ms, &got2)) ||
                second != static_cast<uint8_t>(Code::Ca)) {
                return RecvRc::BadFrame;
            }
            len = 2;
            return RecvRc::Ca;
        }

        case 0x41U:   /* 'A' 上位机主动放弃 */
        case 0x61U:   /* 'a' */
            return RecvRc::Abort;

        default:
            return RecvRc::BadFrame;
    }

    /* 读 包序号 + 序号反码 + 数据 + CRC16(2字节) */
    const uint32_t want = pkt_size + 4U;
    if (!ok(uart_read(&buf_[kNumIndex], want, cfg_.packet_timeout_ms, &got)) ||
        got != want) {
        return RecvRc::Timeout;
    }

    /* 序号反码自校验 */
    if (buf_[kNumIndex] != static_cast<uint8_t>(buf_[kCnumIndex] ^ 0xFFU)) {
        return RecvRc::BadFrame;
    }

    /* CRC16 校验（大端存放） */
    const uint16_t rx_crc =
        static_cast<uint16_t>((static_cast<uint16_t>(buf_[pkt_size + kDataIndex]) << 8) |
                              static_cast<uint16_t>(buf_[pkt_size + kDataIndex + 1U]));
    if (Crc16::compute(&buf_[kDataIndex], pkt_size) != rx_crc) {
        ++stats_.crc_errors;
        return RecvRc::BadFrame;
    }

    len = pkt_size;
    return RecvRc::Ok;
}

/* ========================================================================
 * 等待批次结束帧（全零首包）
 *
 * 若上位机不实现结束帧，超时后仍按成功处理——数据此时已经收完。
 * ======================================================================*/
bool Ymodem::wait_end_packet() noexcept
{
    for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
        if (!send_byte(Code::ReqC)) {
            break;
        }

        uint32_t     len = 0;
        const RecvRc rc  = recv_packet(len);

        if (rc == RecvRc::Ok && buf_[kNumIndex] == 0U && buf_[kDataIndex] == 0U) {
            send_byte(Code::Ack);
            return true;
        }
        if (rc == RecvRc::Timeout) {
            continue;
        }
        if (rc == RecvRc::Ok) {
            send_byte(Code::Nak);
            continue;
        }
        break;
    }
    /* 数据已完整接收，结束帧缺失不影响升级结果 */
    return true;
}

/* ========================================================================
 * 主接收流程
 * ======================================================================*/
Ymodem::Outcome Ymodem::receive() noexcept
{
    Outcome out;

    uart_flush_rx();

    /* 主动发起握手，请求 CRC 模式（比等着超时再发更规范、更快） */
    if (!send_byte(Code::ReqC)) {
        out.status = Status::Timeout;
        return out;
    }

    /* 握手阶段总超时的计时起点。软件复位唤回 Bootloader 时，只给上位机
     * 一个有限窗口（handshake_timeout_ms，默认 15s）：窗口内没等到
     * YMODEM 首包就超时退出，让调用方跳回 APP；否则会永远卡在 IAP。
     * 置 0 表示关闭（握手无限等，恢复传统行为）。 */
    const uint32_t handshake_start = tick_ms();

    uint32_t offset       = 0;
    bool     session_done = false;

    while (!session_done && ok(out.status)) {

        uint32_t seq_expected = 0;      /* 期望的包序号，0-255 循环 */
        bool     file_done    = false;
        bool     session_begun = false;
        uint32_t errors       = 0;      /* 连续超时计数 */
        uint32_t naks         = 0;      /* 连续 NAK 计数 */

        while (!file_done && ok(out.status)) {
            uint32_t     len = 0;
            const RecvRc rc  = recv_packet(len);

            switch (rc) {

            /* ---------------- 收到合法帧 ---------------- */
            case RecvRc::Ok: {
                errors = 0;

                if (buf_[kNumIndex] != static_cast<uint8_t>(seq_expected)) {
                    /* 序号不符：请求重传，期望序号不变 */
                    send_byte(Code::Nak);
                    ++out.stats.retries;
                    if (++naks > cfg_.max_nak) {
                        send_abort();
                        out.status = Status::Protocol;
                    }
                    break;
                }
                naks = 0;

                if (seq_expected == 0U) {
                    /* ========== 首包 ========== */
                    if (buf_[kDataIndex] == 0x00U) {
                        /* 空首包 → 批次结束 */
                        send_byte(Code::Ack);
                        file_done    = true;
                        session_done = true;
                        break;
                    }

                    char     name[kFilenameMax] = {0};
                    uint32_t size = 0;
                    parse_header(&buf_[kDataIndex], name, size);

                    /* 记录文件名供上层展示 */
                    for (uint32_t k = 0; k < kFilenameMax; ++k) {
                        out.stats.filename[k] = name[k];
                        if (name[k] == '\0') {
                            break;
                        }
                    }
                    out.stats.file_size = size;

                    /* 交由上层决定是否接受（校验大小 + 擦除 Flash） */
                    if (!sink_.on_file_start(name, size)) {
                        send_abort();
                        out.status = Status::NoSpace;
                        file_done  = true;
                        break;
                    }

                    offset = 0;
                    send_byte(Code::Ack);
                    send_byte(Code::ReqC);   /* 请求第一个数据包 */
                } else {
                    /* ========== 数据包 ========== */
                    if (!sink_.on_file_data(offset, &buf_[kDataIndex], len)) {
                        send_abort();
                        out.status = Status::FlashFail;
                        file_done  = true;
                        break;
                    }
                    offset            += len;
                    out.stats.received = offset;
                    ++out.stats.packets;
                    send_byte(Code::Ack);
                }

                ++seq_expected;
                session_begun = true;
                break;
            }

            /* ---------------- 传输结束 ---------------- */
            case RecvRc::Eot: {
                send_byte(Code::Ack);
                sink_.on_file_end(offset);
                file_done    = true;
                session_done = wait_end_packet();
                break;
            }

            /* ---------------- 上位机取消 ---------------- */
            case RecvRc::Ca: {
                send_byte(Code::Ack);
                out.status = Status::Cancelled;
                session_done = true;
                break;
            }

            /* ---------------- 上位机主动放弃 ---------------- */
            case RecvRc::Abort: {
                send_abort();
                out.status = Status::Cancelled;
                session_done = true;
                break;
            }

            /* ---------------- 帧错误 / 超时 ---------------- */
            case RecvRc::BadFrame:
            case RecvRc::Timeout:
            default: {
                if (session_begun) {
                    /* 已在收数据途中：累计连续超时，超限中止 */
                    if (++errors > cfg_.max_errors) {
                        send_abort();
                        out.status = Status::Timeout;
                        file_done  = true;
                        break;
                    }
                } else if (cfg_.handshake_timeout_ms != 0U &&
                           (tick_ms() - handshake_start) >= cfg_.handshake_timeout_ms) {
                    /* 握手总超时：窗口内没等到首包，超时退出 */
                    out.status = Status::Timeout;
                    file_done  = true;
                    break;
                }
                /* 请求重发：发 'C' 会被当成请求下一包，
                 * 仅对 CRC 模式有效；乱序/损坏时用 NAK 更准确 */
                send_byte(Code::ReqC);
                ++out.stats.retries;
                break;
            }

            } /* switch */
        } /* 内层 while */
    } /* 外层 while */

    return out;
}

/* --------------------------------------------------------------------------
 * ⑥ 升级会话：擦除 → 收 → 校验 → 提交
 * -------------------------------------------------------------------------- */




class Session final : public YmodemSink {
public:
    struct Config {
        uint32_t       app_base = BL_APP_BASE;
        uint32_t       app_size = BL_APP_SIZE;
        Ymodem::Config ymodem{};
    };

    struct Result {
        IapResult outcome     = IapResult::Idle;
        uint32_t  fw_size     = 0;   ///< 首包声明的固件大小
        uint32_t  fw_crc32    = 0;   ///< 本端算出的整镜像 CRC32
        uint32_t  written     = 0;   ///< 实际写入 Flash 的字节数（含末包填充）
        uint32_t  version     = 0;   ///< 从文件名解析，解析不到则为 0
        char      filename[Ymodem::kFilenameMax] = {0};
        Status    error       = Status::Ok;
    };

    /**
     * 注意：构造函数把 *this 交给 Ymodem 保存为 YmodemSink 引用。
     * 此时 Session 的 vtable 尚未建立，但 Ymodem 构造期不会调用 sink 的
     * 虚函数，因此安全；后续 receive() 调用时才发生动态绑定。
     *
     * 拆成两个重载而非默认实参 Config{}：参见 bl_ymodem.hpp 中的说明
     * （类的默认实参不在 complete-class context 中）。
     */
    Session() noexcept
        : Session(Config{}) {}

    explicit Session(const Config& cfg) noexcept
        : cfg_(cfg), ymodem_(*this, cfg.ymodem) {}

    /// 执行一次完整升级会话（阻塞直到结束）
    Result run() noexcept;

    /* ---------------- YmodemSink 实现 ---------------- */
    bool on_file_start(const char* filename, uint32_t size) noexcept override;
    bool on_file_data(uint32_t offset, const uint8_t* data, uint32_t len) noexcept override;
    void on_file_end(uint32_t total) noexcept override;

private:
    /// 擦除 [app_base, app_base + bytes) 覆盖到的所有扇区
    bool erase_region(uint32_t bytes) noexcept;

    /// 从文件名解析版本号（形如 v1.2.3 → 0x000100020003），失败返回 0
    static uint32_t parse_version(const char* name) noexcept;

    Config  cfg_;
    Ymodem  ymodem_;

    Result   result_;
    Crc32    crc_;
    uint32_t declared_size_ = 0;   ///< 首包声明大小
    uint32_t write_total_   = 0;   ///< 实际写入总量（含填充）
    bool     accepted_      = false;
};

/* --------------------------------------------------------------------------
 * ⑥ 升级会话：擦除 → 收 → 校验 → 提交
 * -------------------------------------------------------------------------- */



/* ========================================================================
 * 从小包装解析版本号
 *
 * 约定："任意名_v<主>.<次>.<修订>.bin"，例如 lumos_app_v1.2.3.bin
 * 解析失败返回 0（不影响升级，只是版本信息缺失）。
 * ======================================================================*/
uint32_t Session::parse_version(const char* name) noexcept
{
    if (name == nullptr) {
        return 0;
    }

    /* 找到 'v' 或 'V' 后紧跟数字的位置 */
    const char* p = nullptr;
    for (const char* q = name; *q != '\0'; ++q) {
        if ((*q == 'v' || *q == 'V') &&
            q[1] >= '0' && q[1] <= '9') {
            p = q + 1;
        }
    }
    if (p == nullptr) {
        return 0;
    }

    uint32_t parts[3] = {0, 0, 0};
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; ++i) {
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10U + static_cast<uint32_t>(*p - '0');
            if (v > 9999U) {
                v = 9999U;
            }
            ++p;
        }
        parts[i] = v;
        if (*p == '.') {
            ++p;
        }
    }

    /* 打包成 0x00MMmmRR（每段 8 bit，够用且紧凑） */
    const uint32_t packed = ((parts[0] & 0xFFU) << 16) |
                            ((parts[1] & 0xFFU) << 8)  |
                             (parts[2] & 0xFFU);
    return packed;
}

/* ========================================================================
 * 按扇区擦除
 *
 * 逐个查询扇区大小后擦除，以适配 F4 这类「扇区大小不等」的 Flash
 * （S0-S3 各 16KB、S4 为 64KB、S5 以上各 128KB）。
 * 只擦到覆盖范围，不整片擦除——避免无谓的等待与寿命消耗。
 * ======================================================================*/
bool Session::erase_region(uint32_t bytes) noexcept
{
    uint32_t addr = cfg_.app_base;
    const uint32_t end = cfg_.app_base + bytes;

    while (addr < end) {
        const uint32_t sector_size = flash_sector_size(addr);
        if (sector_size == 0U) {
            BL_LOG("[session] erase: bad sector at 0x%08lX\r\n",
                   static_cast<unsigned long>(addr));
            return false;
        }
        if (!ok(flash_erase(addr, sector_size))) {
            BL_LOG("[session] erase failed at 0x%08lX\r\n",
                   static_cast<unsigned long>(addr));
            return false;
        }
        addr += sector_size;
    }
    return true;
}

/* ========================================================================
 * 首包：校验大小 → 置 Download → 擦除
 * ======================================================================*/
bool Session::on_file_start(const char* filename, uint32_t size) noexcept
{
    accepted_ = false;

    if (size == 0U) {
        BL_LOG("[session] reject: zero size\r\n");
        return false;
    }
    if (size > cfg_.app_size) {
        BL_LOG("[session] reject: size %lu > app area %lu\r\n",
               static_cast<unsigned long>(size),
               static_cast<unsigned long>(cfg_.app_size));
        return false;
    }

    /* 实际写入量要按 YMODEM 包长向上取整——最后一包不足时，
     * 发送方会用填充字节补满，这些字节同样会被写进 Flash。 */
    const uint32_t write_len =
        ((size + Ymodem::kBlockMax - 1U) / Ymodem::kBlockMax) * Ymodem::kBlockMax;

    BL_LOG("[session] file=%s size=%lu (write=%lu)\r\n",
           filename,
           static_cast<unsigned long>(size),
           static_cast<unsigned long>(write_len));

    /* ① 先置「不可信」，再做破坏性操作 —— 顺序不可颠倒 */
    if (!ok(meta().mark_download())) {
        BL_LOG("[session] mark_download failed\r\n");
        return false;
    }

    /* ② 擦除所需扇区 */
    if (!erase_region(write_len)) {
        BL_LOG("[session] erase region failed\r\n");
        return false;
    }

    /* ③ 准备接收 */
    declared_size_ = size;
    write_total_   = 0;
    crc_.reset();
    accepted_      = true;

    /* 供 run() 填充结果 */
    result_.fw_size = size;
    result_.version = parse_version(filename);
    for (uint32_t i = 0; i < Ymodem::kFilenameMax; ++i) {
        result_.filename[i] = filename[i];
        if (filename[i] == '\0') {
            break;
        }
    }

    return true;
}

/* ========================================================================
 * 数据块：写 Flash + 增量累加 CRC32
 * ======================================================================*/
bool Session::on_file_data(uint32_t offset, const uint8_t* data, uint32_t len) noexcept
{
    if (!accepted_ || data == nullptr || len == 0U) {
        return false;
    }
    if (offset + len > cfg_.app_size) {
        BL_LOG("[session] write overflow at off=%lu len=%lu\r\n",
               static_cast<unsigned long>(offset),
               static_cast<unsigned long>(len));
        return false;
    }

    /* 整包写入 Flash（含末包的填充字节） */
    if (!ok(flash_write(cfg_.app_base + offset, data, len))) {
        BL_LOG("[session] flash write failed at off=%lu\r\n",
               static_cast<unsigned long>(offset));
        return false;
    }

    /* CRC 只覆盖固件原始长度：末包的填充字节不计入，
     * 这样 CRC 的含义就是「固件内容指纹」，与填充值无关。 */
    uint32_t crc_len = 0;
    if (offset < declared_size_) {
        const uint32_t remain = declared_size_ - offset;
        crc_len = (len < remain) ? len : remain;
    }
    if (crc_len > 0U) {
        crc_.update(data, crc_len);
    }

    write_total_ = offset + len;
    return true;
}

/* ========================================================================
 * 结束：记录本端算出的 CRC32
 * ======================================================================*/
void Session::on_file_end(uint32_t total) noexcept
{
    result_.written  = (total > write_total_) ? total : write_total_;
    result_.fw_crc32 = crc_.value();

    BL_LOG("[session] received %lu bytes, crc32=0x%08lX\r\n",
           static_cast<unsigned long>(result_.written),
           static_cast<unsigned long>(result_.fw_crc32));
}

/* ========================================================================
 * 执行一次会话
 * ======================================================================*/
Session::Result Session::run() noexcept
{
    result_ = Result{};

    const Ymodem::Outcome out = ymodem_.receive();

    if (!ok(out.status)) {
        /* 失败：配置区停留在 Download 状态，
         * 下次上电会判为「不可跳转」并留在 IAP，可重刷。 */
        result_.outcome = (out.status == Status::Cancelled)
                              ? IapResult::Aborted
                              : IapResult::Failed;
        result_.error   = out.status;

        BL_LOG("[session] aborted: status=%d, received=%lu\r\n",
               static_cast<int>(out.status),
               static_cast<unsigned long>(out.stats.received));
        return result_;
    }

    if (!accepted_) {
        /* 会话正常结束但从未接受过文件（例如空首包直接结束） */
        result_.outcome = IapResult::Idle;
        return result_;
    }

    /* 确认固件大小与声明一致，防止半截文件被当成完整固件 */
    if (out.stats.received < declared_size_) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::Protocol;
        BL_LOG("[session] short file: got %lu want %lu\r\n",
               static_cast<unsigned long>(out.stats.received),
               static_cast<unsigned long>(declared_size_));
        return result_;
    }

    /* 校验向量表，确保刷进去的东西确实能启动 */
    if (!ok(verify_vector_table(cfg_.app_base, nullptr))) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::CrcFail;
        BL_LOG("[session] vector table invalid after write\r\n");
        return result_;
    }

    /* 提交：置 Valid 并记录 size / crc32 / 版本 */
    if (!ok(meta().commit(result_.fw_size, result_.fw_crc32, result_.version))) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::FlashFail;
        BL_LOG("[session] commit failed\r\n");
        return result_;
    }

    result_.outcome = IapResult::Done;
    result_.error   = Status::Ok;

    BL_LOG("[session] done: size=%lu crc=0x%08lX ver=0x%06lX\r\n",
           static_cast<unsigned long>(result_.fw_size),
           static_cast<unsigned long>(result_.fw_crc32),
           static_cast<unsigned long>(result_.version));

    return result_;
}

/* --------------------------------------------------------------------------
 * ⑦ 启动决策：跳 APP 还是留在 IAP
 * -------------------------------------------------------------------------- */




class Boot {
public:
    enum class Action : uint8_t {
        JumpToApp,     ///< 跳转应用
        EnterIap,      ///< 留在 IAP 等待升级（无限等待）
        EnterIapTimed, ///< 软件复位唤回的限时窗口：等上位机，超时跳回 APP
    };

    struct Config {
        bool     verify_crc_on_boot = (BL_BOOT_VERIFY_CRC32 != 0);
        uint32_t app_base           = BL_APP_BASE;
    };

    struct Decision {
        Action      action = Action::EnterIap;
        const char* reason = "";

        Decision() = default;
        constexpr Decision(Action a, const char* r) noexcept : action(a), reason(r) {}
    };

    static Decision decide() noexcept;
    static Decision decide(const Config& cfg) noexcept;

    /// 跳转到 APP（内部调用 port 层，正常不返回）
    static void jump(uint32_t app_base) noexcept;
};

/* --------------------------------------------------------------------------
 * ⑦ 启动决策：跳 APP 还是留在 IAP
 * -------------------------------------------------------------------------- */



Boot::Decision Boot::decide() noexcept
{
    return decide(Config{});
}

Boot::Decision Boot::decide(const Config& cfg) noexcept
{
    Meta& m = meta();

    // 复位原因必须每次启动都读（read-and-clear），否则旧标志会累积到下次启动
    const ResetCause cause = reset_cause();
    BL_LOG("[boot] reset cause = %lu (0=unk 1=por 2=pin 3=sft)\r\n",
           static_cast<unsigned long>(cause));

    // 1. 固件状态不可跳转 → 进 IAP
    if (!m.should_boot()) {
        return { Action::EnterIap, "no bootable firmware" };
    }

    // 2. 软件复位唤回：APP 运行中软复位，进限时升级窗口
    if (cause == ResetCause::Software) {
        return { Action::EnterIapTimed, "soft reset -> upgrade window" };
    }

    // 3. 向量表校验
    if (!ok(verify_vector_table(cfg.app_base, nullptr))) {
        return { Action::EnterIap, "invalid vector table" };
    }

    // 4. 整镜像 CRC32 校验（可选）
    if (cfg.verify_crc_on_boot) {
        const Meta::Slot& s = m.current();
        if (!ok(verify_image(cfg.app_base, s.fw_size, s.fw_crc32, nullptr))) {
            return { Action::EnterIap, "image crc mismatch" };
        }
    }

    // 5. 全部通过 → 跳 APP
    return { Action::JumpToApp, "ok" };
}

void Boot::jump(uint32_t app_base) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n",
           static_cast<unsigned long>(app_base));
    jump_to_app(app_base);
    BL_LOG("[boot] jump failed!\r\n");
}

/* --------------------------------------------------------------------------
 * ⑧ 入口：平台自检 → 分区检查 → 决策 → 主循环
 * -------------------------------------------------------------------------- */

#include "bl_config.h"          /* 必须最先：芯片参数 */



namespace {

const char* action_name(Boot::Action a) noexcept
{
    switch (a) {
        case Boot::Action::JumpToApp:     return "JUMP";
        case Boot::Action::EnterIap:      return "IAP";
        case Boot::Action::EnterIapTimed: return "IAP_TIMED";
        default:                          return "?";
    }
}

const char* outcome_name(IapResult r) noexcept
{
    switch (r) {
        case IapResult::Idle:    return "IDLE";
        case IapResult::Done:    return "DONE";
        case IapResult::Failed:  return "FAILED";
        case IapResult::Aborted: return "ABORTED";
        case IapResult::NoSpace: return "NOSPACE";
        default:                 return "?";
    }
}

/// 无法继续时停在这里（Bootloader 本身永不被自己擦掉，停在原地等调试）
[[noreturn]] void fatal(const char* why) noexcept
{
    for (;;) {
        BL_LOG("[main] FATAL: %s\r\n", why);
        for (volatile uint32_t i = 0; i < 8000000U; ++i) {}
    }
}

/// 分区基址必须落在扇区边界（Flash 只能整扇区擦，差一个字节会毁邻近区）
bool partition_aligned(const char* name, uint32_t base) noexcept
{
    const uint32_t unit = flash_sector_size(base);
    if (unit == 0U) {
        BL_LOG("[cfg] %s base 0x%08lX is outside flash!\r\n",
               name, static_cast<unsigned long>(base));
        return false;
    }
    if (flash_bytes_to_sector_end(base) != unit) {
        BL_LOG("[cfg] %s base 0x%08lX is not on a sector boundary (sector=%lu B)\r\n",
               name, static_cast<unsigned long>(base), static_cast<unsigned long>(unit));
        return false;
    }
    return true;
}

bool layout_check() noexcept
{
    bool all = true;
    if (!partition_aligned("BOOT", BL_BOOT_BASE)) all = false;
    if (!partition_aligned("APP",  BL_APP_BASE))  all = false;
    if (!partition_aligned("META", BL_META_BASE)) all = false;
    return all;
}

void banner() noexcept
{
    BL_LOG("\r\n===== LUMOS-bootloader =====\r\n");
    BL_LOG("[main] chip  : %s\r\n", BL_CHIP_NAME);
    BL_LOG("[main] flash : 0x%08lX + %lu KB\r\n",
           static_cast<unsigned long>(BL_FLASH_BASE), static_cast<unsigned long>(BL_FLASH_SIZE / 1024U));
    BL_LOG("[main] boot  : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_BOOT_BASE), static_cast<unsigned long>(BL_BOOT_SIZE / 1024U));
    BL_LOG("[main] app   : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_APP_BASE), static_cast<unsigned long>(BL_APP_SIZE / 1024U));
    BL_LOG("[main] meta  : 0x%08lX (%lu KB)\r\n",
           static_cast<unsigned long>(BL_META_BASE), static_cast<unsigned long>(BL_META_SIZE / 1024U));
}

} // namespace

[[noreturn]] void bl_entry() noexcept
{
    if (!ok(platform_init())) fatal("platform init failed");
    if (!ok(uart_init(BL_UART_BAUDRATE))) fatal("uart init failed");
    if (!ok(flash_init())) fatal("flash init failed");
    if (!ok(crc_selftest())) fatal("crc selftest failed (check poly/init)");
    if (!ok(meta().init())) fatal("meta init failed");

    banner();
    meta().dump();

    // 分区校验：地址错了会毁邻近区，宁可停在报错也不进 IAP 做破坏性擦除
    if (!layout_check()) {
        BL_LOG("[main] FATAL: partition layout invalid\r\n");
        if (meta().should_boot()) {
            BL_LOG("[main] firmware state is bootable, attempting jump anyway\r\n");
            Boot::jump(BL_APP_BASE);
        }
        fatal("partition layout invalid");
    }

    const Boot::Decision decision = Boot::decide();
    BL_LOG("[main] decision: %s (%s)\r\n", action_name(decision.action), decision.reason);

    if (decision.action == Boot::Action::JumpToApp) {
        Boot::jump(BL_APP_BASE);
        BL_LOG("[main] jump failed, fallback to IAP\r\n");
    }

    // 软件复位唤回的限时窗口：窗口内没等到 YMODEM 首包就跳回 APP
    bool timed_window = (decision.action == Boot::Action::EnterIapTimed);

    for (;;) {
        BL_LOG("[main] waiting for YMODEM transfer...\r\n");

        Session session;
        const Session::Result r = session.run();

        BL_LOG("[main] outcome=%s size=%lu crc=0x%08lX file=%s\r\n",
               outcome_name(r.outcome),
               static_cast<unsigned long>(r.fw_size),
               static_cast<unsigned long>(r.fw_crc32),
               r.filename);

        if (r.outcome == IapResult::Done) {
            // 升级成功，直接跳新固件（session 里已校验过向量表 + CRC）
            BL_LOG("[main] upgrade done, jumping...\r\n");
            delay_ms(300);
            Boot::jump(BL_APP_BASE);
            BL_LOG("[main] jump failed, keep IAP\r\n");
        }

        // 限时窗口超时（握手没等到首包）→ 跳回 APP
        if (timed_window &&
            r.outcome == IapResult::Failed &&
            r.error == Status::Timeout &&
            r.fw_size == 0U) {
            BL_LOG("[main] upgrade window timeout, jumping to app\r\n");
            Boot::jump(BL_APP_BASE);
            BL_LOG("[main] jump failed, keep IAP\r\n");
            timed_window = false;
        }

        delay_ms(200);
    }
}


#if BL_PROVIDE_MAIN
int main(void)
{
    bl::bl_entry();
}
#endif


} // namespace bl


/* ==========================================================================
 * 对外入口（C 链接：C / C++ 工程都能直接调）
 *   —— bl_config.h 里 BL_PROVIDE_MAIN=1（默认）时，本文件自带 main()
 * ==========================================================================*/
extern "C" void bl_run(void)
{
    bl::bl_entry();
}

#if BL_PROVIDE_MAIN
int main(void)
{
    bl::bl_entry();
}
#endif
