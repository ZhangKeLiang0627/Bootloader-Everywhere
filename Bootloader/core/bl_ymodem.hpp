/**
 * @file    bl_ymodem.hpp
 * @brief   YMODEM-1K 接收端（Bootloader 侧）
 *
 * 协议行为按公开规范实现，帧格式与 CRC16 参数已与 ST 官方实现和
 * SecureCRT 等上位机逐字节核对一致：
 *   SOH(0x01)=128 字节包  STX(0x02)=1024 字节包  EOT(0x04) 结束
 *   ACK(0x06)  NAK(0x15)  CA(0x18) 中止  'C'(0x43) 请求 CRC 模式
 *
 * 帧内缓冲布局（沿用 ST 的 4 字节数据偏移，使数据区天然 4 字节对齐，
 * 便于直接作为 Flash 编程源地址）：
 *
 *   [0] start(SOH/STX/EOT/CA)   [1] 保留未用（对齐用）
 *   [2] 包序号                   [3] 包序号反码
 *   [4..4+n-1] 数据(n=128 或 1024)
 *   [4+n],[4+n+1] CRC16（大端）
 *
 * 本类只负责「协议」；数据落到哪里由 YmodemSink 决定，
 * 因此 core 层不依赖具体存储器。
 */
#ifndef BL_YMODEM_HPP
#define BL_YMODEM_HPP

#include "bl_types.hpp"
#include "bl_config.h"

namespace bl {

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

    /**
     * @brief 阻塞式接收一个固件文件
     *
     * 返回即表示会话已结束（成功或失败）。
     * 全过程内部喂狗，调用方无需关心。
     */
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

} // namespace bl

#endif /* BL_YMODEM_HPP */
