// Bootloader-Everywhere 全部实现（单文件库）。
// 只调 bl_port.h 声明的函数，不认识任何芯片厂商头文件 —— 这是它能跨芯片的原因。
// 约束：C++11，无异常、无 RTTI、无动态内存。

#include "bl.h"
#include "bl_port.h"
#include "bl_config.h"
#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace bl { [[noreturn]] void blEntry() noexcept; }

namespace bl {

// ① 日志：轻量串口输出（不依赖 stdio，省 ROM）

namespace log {

namespace {

#if BL_DEBUG_LOG

/* 行缓冲：攒满一行再一次性发出，避免逐字符调用串口带来的开销 */
constexpr uint32_t kLineBufSize = 128U;
char     gBuf[kLineBufSize];
uint32_t gLen = 0;

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

// 整数输出：只支持十进制与十六进制 + 可选补零宽度
// （覆盖全库实际用到的 %lu / %08lX / %06lX / %d 四种写法）
void putNumber(uint32_t v, bool hex, uint32_t width, bool zeroPad) noexcept
{
    const uint32_t base   = hex ? 16U : 10U;
    const char*    digits = hex ? "0123456789ABCDEF" : "0123456789";
    char     tmp[11];
    uint32_t n = 0;

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
        put(tmp[--n]);          // 数字是逆序生成的，倒着吐出来
    }
}

#endif /* BL_DEBUG_LOG */

} // namespace

// 支持 %u %d %X %s %%，可带 0 与宽度修饰（如 %08lX）。不支持浮点。
void printf(const char* fmt, ...) noexcept
{
#if BL_DEBUG_LOG
    if (fmt == nullptr) {
        return;
    }

    gLen = 0;

    va_list ap;
    va_start(ap, fmt);

    for (const char* p = fmt; *p != '\0'; ++p) {

        if (*p != '%') {
            put(*p);
            continue;
        }

        ++p;                                    // 跳过 '%'

        bool zeroPad = false;
        if (*p == '0') {
            zeroPad = true;
            ++p;
        }

        uint32_t width = 0;
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
        } else if (*p == 'u') {
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
            break;                              // 格式串以 '%' 结尾
        } else {
            put('%');
            put(*p);                            // 未知转换符原样输出，便于发现写错的格式串
        }
    }

    va_end(ap);
    flush();
#else
    (void)fmt;
#endif
}

} // namespace log

// ② CRC16 / CRC32

// CRC16 / XMODEM（YMODEM 帧校验）
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

// CRC32 / ISO-HDLC —— 整镜像校验
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
uint32_t crc32Flash(uint32_t addr, uint32_t len) noexcept;

// 自检
//
// CRC 参数一旦写错，现象是「PC 算的值和板子算的对不上」，极难排查。
// 上电时跑一次自检，能在最早时刻暴露问题。
Status crcSelftest() noexcept;

// ② CRC16 / CRC32

// CRC16 / XMODEM —— poly 0x1021，逐位实现
//
// 选逐位而非查表：YMODEM 每帧 1KB，逐位在 168MHz 上约 0.25ms，
// 完全可以接受；换来的是代码更小、更容易核对正确性。
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

// CRC32 / ISO-HDLC（半字节查表）
//
// 表项为反向多项式 0xEDB88320 的 4 位查表，
// 一次处理 4 bit，比纯逐位快约 4 倍，而表只占 64 字节。
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

// 直接对 Flash 区域算 CRC32
uint32_t crc32Flash(uint32_t addr, uint32_t len) noexcept
{
    uint8_t buf[256];
    Crc32   crc;
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (!ok(flashRead(addr + done, buf, chunk))) {
            /* 读失败时返回哨兵值，正常 CRC 结果不可能等于它 */
            return 0xDEADBEEFU;
        }
        crc.update(buf, chunk);
        done += chunk;
    }
    return crc.value();
}

// 自检
Status crcSelftest() noexcept
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

// ③ 校验：向量表
//
// 向量表是否像个能跑的东西：SP 落在 SRAM 内、入口落在 APP 区内且为 Thumb 地址。
// 两次 32 位读而已，却能挡住最常见的两类「坏固件」：
//   - 空片：全 0xFF → SP=0xFFFFFFFF 不在 SRAM 范围
//   - 擦到一半的扇区：向量表首字可能已被擦成 0xFF 或残值
static bool vectorsSane(uint32_t sp, uint32_t pc) noexcept
{
    return (sp >= BL_SRAM_BASE) && (sp <= BL_SRAM_END) &&
           (pc >= BL_APP_BASE)  && (pc <  BL_APP_END) && ((pc & 1U) != 0U);
}

// 上电时读 APP 区前两个字判断能不能启动。
// 这是唯一的判据 —— 提交阶段把这两个字留到最后写，
// 所以「它们合法」等价于「整份固件已完整写入并校验过」。
static Status checkVectors(uint32_t appBase) noexcept
{
    uint32_t vec[2] = {0, 0};
    if (!ok(flashRead(appBase, vec, sizeof(vec)))) {
        return Status::FlashFail;
    }
    if (!vectorsSane(vec[0], vec[1])) {
        BL_LOG("[verify] bad vectors: sp=0x%08lX pc=0x%08lX\r\n",
               static_cast<unsigned long>(vec[0]),
               static_cast<unsigned long>(vec[1]));
        return Status::CrcFail;
    }
    return Status::Ok;
}

// ④ YMODEM 接收端

// 接收结果通知接口
//
// 以抽象类而非 std::function：不引入动态内存与额外开销，
// 且调用点可静态解析，适合嵌入式。
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
    virtual bool onFileStart(const char* filename, uint32_t size) = 0;

    /**
     * @brief 一个数据块到达
     * @param offset 该块在固件中的字节偏移
     * @param data   数据指针（指向内部缓冲，回调返回后即失效）
     * @param len    字节数（末尾包可能小于 1024）
     * @return true 写入成功；false 失败，本类将发 CA 中止
     */
    virtual bool onFileData(uint32_t offset, const uint8_t* data, uint32_t len) = 0;

    /// 收到 EOT 并回 ACK 之后调用
    virtual void onFileEnd(uint32_t totalBytes) = 0;
};

// YMODEM 接收端
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
        uint32_t packetTimeoutMs    = BL_YMODEM_PACKET_TIMEOUT_MS;
        uint32_t handshakeTimeoutMs = BL_YMODEM_HANDSHAKE_MS;
        uint32_t maxErrors           = BL_YMODEM_MAX_RETRY;   ///< 连续超时上限
        uint32_t maxNak              = BL_YMODEM_MAX_NAK;     ///< 连续 NAK 上限
    };

    struct Stats {
        char     filename[kFilenameMax] = {0};
        uint32_t fileSize   = 0;   ///< 首包声明的总大小
        uint32_t received    = 0;   ///< 实际写入的字节数
        uint32_t packets     = 0;   ///< 收到的数据包个数
        uint32_t retries     = 0;   ///< 重传请求次数
        uint32_t crcErrors  = 0;   ///< 帧 CRC 错误次数
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

    RecvRc recvPacket(uint32_t& len) noexcept;
    bool   waitEndPacket() noexcept;
    bool   sendByte(Code c) noexcept;
    bool   sendAbort() noexcept;
    void   parseHeader(const uint8_t* data, char* nameOut, uint32_t& sizeOut) noexcept;

    YmodemSink& sink_;
    Config      cfg_;
    Stats       stats_;
    uint8_t     buf_[kBufSize] = {0};
};

// ④ YMODEM 接收端

// 底层收发
bool Ymodem::sendByte(Code c) noexcept
{
    const auto b = static_cast<uint8_t>(c);
    return ok(uartWrite(&b, 1));
}

bool Ymodem::sendAbort() noexcept
{
    /* CAN 必须连发两次才被上位机识别为取消 */
    return sendByte(Code::Ca) && sendByte(Code::Ca);
}

// 解析首包中的「文件名 + 大小」
//
// 首包数据区格式："文件名\0" + "十进制大小" + ' ' + 填充
void Ymodem::parseHeader(const uint8_t* data, char* nameOut, uint32_t& sizeOut) noexcept
{
    uint32_t i = 0;

    /* 文件名：以 '\0' 结束 */
    while (i < (kFilenameMax - 1U) && data[i] != 0x00U) {
        nameOut[i] = static_cast<char>(data[i]);
        ++i;
    }
    nameOut[i] = '\0';

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
    sizeOut = size;
}

// 收一帧
Ymodem::RecvRc Ymodem::recvPacket(uint32_t& len) noexcept
{
    len = 0;

    uint8_t  start = 0;
    uint32_t got   = 0;
    if (!ok(uartRead(&start, 1, cfg_.packetTimeoutMs, &got)) || got == 0) {
        return RecvRc::Timeout;
    }

    uint32_t pktSize = 0;
    switch (start) {
        case static_cast<uint8_t>(Code::Soh):
            pktSize = 128U;                 /* 128 字节小包 */
            break;

        case static_cast<uint8_t>(Code::Stx):
            pktSize = kBlockMax;            /* 1024 字节大包 */
            break;

        case static_cast<uint8_t>(Code::Eot):
            return RecvRc::Eot;              /* len 保持 0 */

        case static_cast<uint8_t>(Code::Ca): {
            /* 取消必须成对出现 */
            uint8_t  second = 0;
            uint32_t got2   = 0;
            if (!ok(uartRead(&second, 1, cfg_.packetTimeoutMs, &got2)) ||
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
    const uint32_t want = pktSize + 4U;
    if (!ok(uartRead(&buf_[kNumIndex], want, cfg_.packetTimeoutMs, &got)) ||
        got != want) {
        return RecvRc::Timeout;
    }

    /* 序号反码自校验 */
    if (buf_[kNumIndex] != static_cast<uint8_t>(buf_[kCnumIndex] ^ 0xFFU)) {
        return RecvRc::BadFrame;
    }

    /* CRC16 校验（大端存放） */
    const uint16_t rxCrc =
        static_cast<uint16_t>((static_cast<uint16_t>(buf_[pktSize + kDataIndex]) << 8) |
                              static_cast<uint16_t>(buf_[pktSize + kDataIndex + 1U]));
    if (Crc16::compute(&buf_[kDataIndex], pktSize) != rxCrc) {
        ++stats_.crcErrors;
        return RecvRc::BadFrame;
    }

    len = pktSize;
    return RecvRc::Ok;
}

// 等待批次结束帧（全零首包）
//
// 若上位机不实现结束帧，超时后仍按成功处理——数据此时已经收完。
bool Ymodem::waitEndPacket() noexcept
{
    for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
        if (!sendByte(Code::ReqC)) {
            break;
        }

        uint32_t     len = 0;
        const RecvRc rc  = recvPacket(len);

        if (rc == RecvRc::Ok && buf_[kNumIndex] == 0U && buf_[kDataIndex] == 0U) {
            sendByte(Code::Ack);
            return true;
        }
        if (rc == RecvRc::Timeout) {
            continue;
        }
        if (rc == RecvRc::Ok) {
            sendByte(Code::Nak);
            continue;
        }
        break;
    }
    /* 数据已完整接收，结束帧缺失不影响升级结果 */
    return true;
}

// 主接收流程
Ymodem::Outcome Ymodem::receive() noexcept
{
    Outcome out;

    uartFlushRx();

    /* 主动发起握手，请求 CRC 模式（比等着超时再发更规范、更快） */
    if (!sendByte(Code::ReqC)) {
        out.status = Status::Timeout;
        return out;
    }

    /* 握手阶段总超时的计时起点。软件复位唤回 Bootloader 时，只给上位机
     * 一个有限窗口（handshakeTimeoutMs，默认 15s）：窗口内没等到
     * YMODEM 首包就超时退出，让调用方跳回 APP；否则会永远卡在 IAP。
     * 置 0 表示关闭（握手无限等，恢复传统行为）。 */
    const uint32_t handshakeStart = tickMs();

    uint32_t offset       = 0;
    bool     sessionDone = false;

    while (!sessionDone && ok(out.status)) {

        uint32_t seqExpected = 0;      /* 期望的包序号，0-255 循环 */
        bool     fileDone    = false;
        bool     sessionBegun = false;
        uint32_t errors       = 0;      /* 连续超时计数 */
        uint32_t naks         = 0;      /* 连续 NAK 计数 */

        while (!fileDone && ok(out.status)) {
            uint32_t     len = 0;
            const RecvRc rc  = recvPacket(len);

            switch (rc) {

            /* ---------------- 收到合法帧 ---------------- */
            case RecvRc::Ok: {
                errors = 0;

                if (buf_[kNumIndex] != static_cast<uint8_t>(seqExpected)) {
                    /* 序号不符：请求重传，期望序号不变 */
                    sendByte(Code::Nak);
                    ++out.stats.retries;
                    if (++naks > cfg_.maxNak) {
                        sendAbort();
                        out.status = Status::Protocol;
                    }
                    break;
                }
                naks = 0;

                if (seqExpected == 0U) {
                    /* ========== 首包 ========== */
                    if (buf_[kDataIndex] == 0x00U) {
                        /* 空首包 → 批次结束 */
                        sendByte(Code::Ack);
                        fileDone    = true;
                        sessionDone = true;
                        break;
                    }

                    char     name[kFilenameMax] = {0};
                    uint32_t size = 0;
                    parseHeader(&buf_[kDataIndex], name, size);

                    /* 记录文件名供上层展示 */
                    for (uint32_t k = 0; k < kFilenameMax; ++k) {
                        out.stats.filename[k] = name[k];
                        if (name[k] == '\0') {
                            break;
                        }
                    }
                    out.stats.fileSize = size;

                    /* 交由上层决定是否接受（校验大小 + 擦除 Flash） */
                    if (!sink_.onFileStart(name, size)) {
                        sendAbort();
                        out.status = Status::NoSpace;
                        fileDone  = true;
                        break;
                    }

                    offset = 0;
                    sendByte(Code::Ack);
                    sendByte(Code::ReqC);   /* 请求第一个数据包 */
                } else {
                    /* ========== 数据包 ========== */
                    if (!sink_.onFileData(offset, &buf_[kDataIndex], len)) {
                        sendAbort();
                        out.status = Status::FlashFail;
                        fileDone  = true;
                        break;
                    }
                    offset            += len;
                    out.stats.received = offset;
                    ++out.stats.packets;
                    sendByte(Code::Ack);
                }

                ++seqExpected;
                sessionBegun = true;
                break;
            }

            /* ---------------- 传输结束 ---------------- */
            case RecvRc::Eot: {
                sendByte(Code::Ack);
                sink_.onFileEnd(offset);
                fileDone    = true;
                sessionDone = waitEndPacket();
                break;
            }

            /* ---------------- 上位机取消 ---------------- */
            case RecvRc::Ca: {
                sendByte(Code::Ack);
                out.status = Status::Cancelled;
                sessionDone = true;
                break;
            }

            /* ---------------- 上位机主动放弃 ---------------- */
            case RecvRc::Abort: {
                sendAbort();
                out.status = Status::Cancelled;
                sessionDone = true;
                break;
            }

            /* ---------------- 帧错误 / 超时 ---------------- */
            case RecvRc::BadFrame:
            case RecvRc::Timeout:
            default: {
                if (sessionBegun) {
                    /* 已在收数据途中：累计连续超时，超限中止 */
                    if (++errors > cfg_.maxErrors) {
                        sendAbort();
                        out.status = Status::Timeout;
                        fileDone  = true;
                        break;
                    }
                } else if (cfg_.handshakeTimeoutMs != 0U &&
                           (tickMs() - handshakeStart) >= cfg_.handshakeTimeoutMs) {
                    /* 握手总超时：窗口内没等到首包，超时退出 */
                    out.status = Status::Timeout;
                    fileDone  = true;
                    break;
                }
                /* 请求重发：发 'C' 会被当成请求下一包，
                 * 仅对 CRC 模式有效；乱序/损坏时用 NAK 更准确 */
                sendByte(Code::ReqC);
                ++out.stats.retries;
                break;
            }

            } /* switch */
        } /* 内层 while */
    } /* 外层 while */

    return out;
}

// ⑤ 升级会话：擦除 → 收 → 回读校验 → 提交

enum class IapResult : uint8_t {
    Idle    = 0,
    Done    = 1,
    Failed  = 2,
    Aborted = 3,
};

// 提交点 = APP 区的前两个字（SP、PC）。
// 接收时把这两个字扣在 RAM 里不写，等整片回读校验通过后才写回。
// 于是任何时刻断电，这两个字要么还是擦除态 0xFF、要么只写了一半，
// 上电的向量表检查都判它非法 → 留在 IAP 可重刷，绝不会跳进半截固件。
// 这样一来就不需要任何「配置区 / 状态标志」了。
class Session final : public YmodemSink {
public:
    struct Config {
        uint32_t       appBase = BL_APP_BASE;
        uint32_t       appSize = BL_APP_SIZE;
        Ymodem::Config ymodem{};
    };

    struct Result {
        IapResult outcome = IapResult::Idle;
        uint32_t  fwSize  = 0;    // 首包声明的固件大小
        uint32_t  fwCrc32 = 0;    // 写入区（前 8 字节之后）的 CRC32
        uint32_t  written = 0;    // 实际写入 Flash 的字节数（含末包填充）
        char      filename[Ymodem::kFilenameMax] = {0};
        Status    error   = Status::Ok;
        bool      erased  = false;    // 是否动过 Flash（决定超时后能不能直接跳回 APP）
    };

    // 构造函数把 *this 交给 Ymodem 保存为 YmodemSink 引用；Ymodem 构造期不调
    // sink 的虚函数，所以此刻 vtable 未建立是安全的，receive() 时才发生绑定。
    Session() noexcept : Session(Config{}) {}
    explicit Session(const Config& cfg) noexcept
        : cfg_(cfg), ymodem_(*this, cfg.ymodem) {}

    Result run() noexcept;                      // 阻塞直到本次会话结束

    // YmodemSink 实现
    bool onFileStart(const char* filename, uint32_t size) noexcept override;
    bool onFileData(uint32_t offset, const uint8_t* data, uint32_t len) noexcept override;
    void onFileEnd(uint32_t total) noexcept override;

private:
    bool eraseRegion(uint32_t bytes) noexcept;  // 擦除覆盖到的所有扇区
    bool verifyWritten() noexcept;              // 回读 [8,size) 与接收时算的 CRC 比对
    bool commit() noexcept;                     // 写回扣住的 SP/PC —— 提交点

    Config  cfg_;
    Ymodem  ymodem_;

    Result   result_;
    Crc32    crc_;                              // 只覆盖 [8, size)
    uint32_t declaredSize_ = 0;                 // 首包声明大小
    uint32_t writeTotal_   = 0;                 // 实际写入总量（含填充）
    uint8_t  entry_[8]     = {0};               // 扣住的 SP/PC
    bool     entryHeld_    = false;
    bool     accepted_     = false;
};

static uint32_t le32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8)  |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// 按扇区擦除：查表拿每个扇区的起点和大小，适配 F4 这类「扇区大小不等」的 Flash。
// 只擦到覆盖范围，不整片擦 —— 避免无谓的等待与寿命消耗。
bool Session::eraseRegion(uint32_t bytes) noexcept
{
    uint32_t addr = cfg_.appBase;
    const uint32_t end = cfg_.appBase + bytes;

    while (addr < end) {
        const FlashSector* s = flashSectorAt(addr);
        if (s == nullptr || s->base != addr || !ok(flashErase(addr, s->size))) {
            BL_LOG("[session] erase failed at 0x%08lX\r\n",
                   static_cast<unsigned long>(addr));
            return false;
        }
        addr += s->size;
    }
    return true;
}

bool Session::onFileStart(const char* filename, uint32_t size) noexcept
{
    accepted_  = false;
    entryHeld_ = false;

    if (size < 8U || size > cfg_.appSize) {
        BL_LOG("[session] reject: size=%lu (app area=%lu)\r\n",
               static_cast<unsigned long>(size),
               static_cast<unsigned long>(cfg_.appSize));
        return false;
    }

    // 实际写入量按 YMODEM 包长向上取整：末包不足时发送方会填充，
    // 这些填充字节同样会写进 Flash。
    const uint32_t writeLen =
        ((size + Ymodem::kBlockMax - 1U) / Ymodem::kBlockMax) * Ymodem::kBlockMax;

    BL_LOG("[session] file=%s size=%lu\r\n",
           filename, static_cast<unsigned long>(size));

    // 擦除即等于「作废现有固件」：擦完 SP/PC 变 0xFF，上电自然停在 IAP。
    if (!eraseRegion(writeLen)) {
        return false;
    }

    declaredSize_ = size;
    writeTotal_   = 0;
    crc_.reset();
    accepted_     = true;

    result_.fwSize = size;
    result_.erased = true;
    for (uint32_t i = 0; i < Ymodem::kFilenameMax; ++i) {
        result_.filename[i] = filename[i];
        if (filename[i] == '\0') {
            break;
        }
    }
    return true;
}

bool Session::onFileData(uint32_t offset, const uint8_t* data, uint32_t len) noexcept
{
    if (!accepted_ || data == nullptr || len == 0U) {
        return false;
    }
    if (offset + len > cfg_.appSize) {
        BL_LOG("[session] write overflow at off=%lu len=%lu\r\n",
               static_cast<unsigned long>(offset),
               static_cast<unsigned long>(len));
        return false;
    }

    uint32_t skipped = 0;
    if (offset == 0U) {
        // 开头这 8 个字节扣在 RAM 里，留到提交时再写
        const uint32_t hold = (len < 8U) ? len : 8U;
        for (uint32_t i = 0; i < hold; ++i) {
            entry_[i] = data[i];
        }
        entryHeld_ = (hold == 8U);
        skipped    = hold;
    }

    const uint32_t writeLen = len - skipped;
    if (writeLen > 0U &&
        !ok(flashWrite(cfg_.appBase + offset + skipped, data + skipped, writeLen))) {
        BL_LOG("[session] flash write failed at off=%lu\r\n",
               static_cast<unsigned long>(offset + skipped));
        return false;
    }

    // CRC 只覆盖原始长度里的 [8, size) —— 正好是「写进 Flash 的那部分」。
    // 提交前回读同一区间重算比对，就能确认 Flash 真的写对了。
    const uint32_t crcOff = offset + skipped;
    if (crcOff < declaredSize_) {
        const uint32_t remain = declaredSize_ - crcOff;
        const uint32_t crcLen = (writeLen < remain) ? writeLen : remain;
        if (crcLen > 0U) {
            crc_.update(data + skipped, crcLen);
        }
    }

    writeTotal_ = offset + len;
    return true;
}

void Session::onFileEnd(uint32_t total) noexcept
{
    result_.written = (total > writeTotal_) ? total : writeTotal_;

    BL_LOG("[session] received %lu bytes, crc32=0x%08lX\r\n",
           static_cast<unsigned long>(result_.written),
           static_cast<unsigned long>(crc_.value()));
}

bool Session::verifyWritten() noexcept
{
    if (declaredSize_ <= 8U) {
        return true;
    }
    const uint32_t actual = crc32Flash(cfg_.appBase + 8U, declaredSize_ - 8U);
    if (actual != crc_.value()) {
        BL_LOG("[session] readback mismatch: rx=0x%08lX flash=0x%08lX\r\n",
               static_cast<unsigned long>(crc_.value()),
               static_cast<unsigned long>(actual));
        return false;
    }
    return true;
}

bool Session::commit() noexcept
{
    if (!ok(flashWrite(cfg_.appBase, entry_, sizeof(entry_)))) {
        BL_LOG("[session] commit failed\r\n");
        return false;
    }
    return true;
}

Session::Result Session::run() noexcept
{
    result_ = Result{};

    const Ymodem::Outcome out = ymodem_.receive();

    if (!ok(out.status)) {
        // 失败退出：SP/PC 没被写过，固件区自然「不可启动」，上电停在 IAP 可重刷
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
        result_.outcome = IapResult::Idle;
        return result_;
    }
    if (!entryHeld_) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::Protocol;
        BL_LOG("[session] bad first block\r\n");
        return result_;
    }
    if (out.stats.received < declaredSize_) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::Protocol;
        BL_LOG("[session] short file: got %lu want %lu\r\n",
               static_cast<unsigned long>(out.stats.received),
               static_cast<unsigned long>(declaredSize_));
        return result_;
    }

    // ① 回读校验：先确认 Flash 里真的写对了，再谈提交
    if (!verifyWritten()) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::FlashFail;
        return result_;
    }

    // ② 待写入的向量表本身得像样
    if (!vectorsSane(le32(entry_), le32(entry_ + 4U))) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::CrcFail;
        BL_LOG("[session] bad vectors in file\r\n");
        return result_;
    }

    // ③ 写回 SP/PC —— 提交点，过了这里固件才可启动
    if (!commit()) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::FlashFail;
        return result_;
    }

    result_.fwCrc32 = crc_.value();
    result_.outcome = IapResult::Done;

    BL_LOG("[session] done: size=%lu crc=0x%08lX\r\n",
           static_cast<unsigned long>(result_.fwSize),
           static_cast<unsigned long>(result_.fwCrc32));
    return result_;
}

// ⑥ 启动决策：跳 APP 还是留在 IAP

class Boot {
public:
    enum class Action : uint8_t {
        JumpToApp,     ///< 跳转应用
        EnterIap,      ///< 留在 IAP 等待升级（无限等待）
        EnterIapTimed, ///< 软件复位唤回的限时窗口：等上位机，超时跳回 APP
    };

    struct Config {
        uint32_t appBase = BL_APP_BASE;
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
    static void jump(uint32_t appBase) noexcept;
};

// ⑥ 启动决策：跳 APP 还是留在 IAP

Boot::Decision Boot::decide() noexcept
{
    return decide(Config{});
}

Boot::Decision Boot::decide(const Config& cfg) noexcept
{
    // 复位原因必须每次启动都读（read-and-clear），否则旧标志会累积到下次启动
    const ResetCause cause = resetCause();
    BL_LOG("[boot] reset cause = %lu\r\n", static_cast<unsigned long>(cause));

    // 1. 按住硬件按钮上电 → 强制留在 IAP。
    //    人在板子旁边、意图明确，所以是无限等（握手超时后本循环会重新握手，不会跳走）。
    if (bootPinHeld()) {
        return { Action::EnterIap, "boot pin held" };
    }

    // 2. 向量表非法 → 没有可启动的固件（空片 / 传输中途掉电），留在 IAP
    if (!ok(checkVectors(cfg.appBase))) {
        return { Action::EnterIap, "no bootable firmware" };
    }

    // 3. 软件复位 → 进限时升级窗口（APP 唤回通道）
    if (cause == ResetCause::Software) {
        return { Action::EnterIapTimed, "soft reset -> upgrade window" };
    }

    // 4. 可以跳了
    return { Action::JumpToApp, "ok" };
}

void Boot::jump(uint32_t appBase) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n",
           static_cast<unsigned long>(appBase));
    jumpToApp(appBase);
    BL_LOG("[boot] jump failed!\r\n");
}

// ⑦ 入口

namespace {

const char* actionName(Boot::Action a) noexcept
{
    switch (a) {
        case Boot::Action::JumpToApp:     return "JUMP";
        case Boot::Action::EnterIap:      return "IAP";
        case Boot::Action::EnterIapTimed: return "IAP_TIMED";
        default:                          return "?";
    }
}

const char* outcomeName(IapResult r) noexcept
{
    switch (r) {
        case IapResult::Idle:    return "IDLE";
        case IapResult::Done:    return "DONE";
        case IapResult::Failed:  return "FAILED";
        case IapResult::Aborted: return "ABORTED";
        default:                 return "?";
    }
}

// 无法继续时停在这里（Bootloader 自己永不被擦，停在原地等调试）
[[noreturn]] void fatal(const char* why) noexcept
{
    for (;;) {
        BL_LOG("[main] FATAL: %s\r\n", why);
        for (volatile uint32_t i = 0; i < 8000000U; ++i) {}
    }
}

// 分区基址必须落在扇区边界：Flash 只能整扇区擦，差一个字节会毁邻近区
bool partitionAligned(const char* name, uint32_t base) noexcept
{
    const FlashSector* s = flashSectorAt(base);
    if (s == nullptr || s->base != base) {
        BL_LOG("[cfg] %s base 0x%08lX is not a sector start\r\n",
               name, static_cast<unsigned long>(base));
        return false;
    }
    return true;
}

bool layoutCheck() noexcept
{
    bool all = true;
    if (!partitionAligned("BOOT", BL_BOOT_BASE)) all = false;
    if (!partitionAligned("APP",  BL_APP_BASE))  all = false;
    return all;
}

} // namespace

[[noreturn]] void blEntry() noexcept
{
    // 芯片已由宿主初始化好（时钟 / 串口 / Flash 接口时钟），这里只做自检
    if (!ok(crcSelftest())) {
        fatal("crc selftest failed");
    }

    BL_LOG("\r\n== Bootloader-Everywhere == flash %lu KB, app 0x%08lX + %lu KB\r\n",
           static_cast<unsigned long>(BL_FLASH_SIZE / 1024U),
           static_cast<unsigned long>(BL_APP_BASE),
           static_cast<unsigned long>(BL_APP_SIZE / 1024U));

    if (!layoutCheck()) {
        fatal("partition layout invalid");
    }

    const Boot::Decision decision = Boot::decide();
    BL_LOG("[main] decision: %s (%s)\r\n", actionName(decision.action), decision.reason);

    if (decision.action == Boot::Action::JumpToApp) {
        Boot::jump(BL_APP_BASE);
    }

    // 软件复位唤回的限时窗口：窗口内没等到首包（也就没动过 Flash）就跳回 APP
    bool timedWindow = (decision.action == Boot::Action::EnterIapTimed);

    for (;;) {
        BL_LOG("[main] waiting for YMODEM transfer...\r\n");

        Session session;
        const Session::Result r = session.run();

        BL_LOG("[main] outcome=%s size=%lu file=%s\r\n",
               outcomeName(r.outcome),
               static_cast<unsigned long>(r.fwSize),
               r.filename);

        if (r.outcome == IapResult::Done) {
            delayMs(200);                       // 让最后几行日志发完
            Boot::jump(BL_APP_BASE);
        }

        if (timedWindow && r.outcome == IapResult::Failed &&
            r.error == Status::Timeout && !r.erased) {
            BL_LOG("[main] window timeout, jumping to app\r\n");
            Boot::jump(BL_APP_BASE);
            timedWindow = false;
        }

        delayMs(200);
    }
}

} // namespace bl

// 对外入口（C 链接：C / C++ 工程都能直接调）
extern "C" void blRun(void)
{
    bl::blEntry();
}
