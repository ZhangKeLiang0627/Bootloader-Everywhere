// YMODEM 串口固件传输协议（含它专用的 CRC16/XMODEM）。
//
// 与 bl.cpp 同属库内部：对外只暴露 bl.h 的 blRun()，APP 不需要看到本文件。
// 拆出来只为可读性 —— 协议层与「决策 / 会话」逻辑分开，改哪块看哪块。
//
// 串口收发通过 bl_port.h 的 uartRead/uartWrite 完成，本文件不认识任何芯片头文件。

#ifndef BL_PROTOCOL_H
#define BL_PROTOCOL_H

#include <stdint.h>

#include "bl.h"          // Status
#include "bl_config.h"   // BL_YMODEM_* 参数

namespace bl {

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

} // namespace bl

#endif // BL_PROTOCOL_H
