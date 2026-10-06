// Bootloader-Everywhere 主体实现：CRC32、向量表校验、IAP 会话、启动决策、入口.

#include "bl.h"
#include "bl_config.h"
#include "bl_log.h"
#include "bl_port.h"
#include "protocol.h"

#include <cstdint>
#include <cstring>

namespace bl { [[noreturn]] void blEntry() noexcept; }

namespace bl {

// ------------------------------------------------------------------ CRC32

// 整镜像校验用 CRC32 / ISO-HDLC（poly 0x04C11DB7，反射 → 反向多项式 0xEDB88320）。
// 与载体层的 CRC8 位序方向相反：这个 LSB-first，那个 MSB-first。
class Crc32 {
public:
    static constexpr uint32_t kInit = 0xFFFFFFFFU;

    constexpr Crc32() noexcept : value_(kInit) {}

    // 不能标 constexpr：C++11 下 constexpr 成员函数隐含 const，改不了成员
    void reset() noexcept { value_ = kInit; }

    void update(const void* data, uint32_t len) noexcept;

    // 取最终结果（内部做最后一次异或）
    constexpr uint32_t value() const noexcept { return value_ ^ 0xFFFFFFFFU; }

private:
    uint32_t value_;
};

namespace {

// 半字节查表：一次 4 bit，比逐位快约 4 倍，表只占 64 字节
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

    for (uint32_t i = 0U; i < len; ++i) {
        value_ ^= static_cast<uint32_t>(p[i]);
        value_ = (value_ >> 4) ^ kNibbleTable[value_ & 0x0FU];
        value_ = (value_ >> 4) ^ kNibbleTable[value_ & 0x0FU];
    }
}

// 分块读 Flash 累加，不占大缓冲
static bool crcOfFlash(uint32_t addr, uint32_t len, Crc32& crc) noexcept
{
    uint8_t  buf[64];
    uint32_t done = 0U;

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (!ok(flashRead(addr + done, buf, chunk))) {
            return false;
        }
        crc.update(buf, chunk);
        done += chunk;
    }
    return true;
}

// 读失败返回哨兵值（正常 CRC 结果不可能等于它）
static uint32_t crc32Flash(uint32_t addr, uint32_t len) noexcept
{
    Crc32 crc;
    return crcOfFlash(addr, len, crc) ? crc.value() : 0xDEADBEEFU;
}

// ------------------------------------------------------------------ 向量表校验

// SP 只做「宽范围 + 8 字节对齐」，不需要知道 SRAM 容量：
// 0x20000000 是 Cortex-M 的 SRAM 区基址（架构约定），1MB 宽覆盖 F1/F4/GD32/CH32；
// 8 字节对齐是 AAPCS 对栈的硬要求。SRAM 不在此处的芯片（如 H7 的 0x24000000）改这两行。
static bool vectorsSane(uint32_t sp, uint32_t pc) noexcept
{
    constexpr uint32_t kSramBase = 0x20000000U;
    constexpr uint32_t kSramSpan = 0x00100000U;

    return (sp >= kSramBase) && (sp < (kSramBase + kSramSpan)) && ((sp & 7U) == 0U) &&
           (pc >= BL_APP_BASE) && (pc < BL_APP_END) && ((pc & 1U) != 0U);
}

// APP 区前两个字是否像个能跑的东西。这是唯一的启动判据：
// 提交阶段把这两个字留到最后写，所以「它们合法」等价于「整份固件已完整写入并校验过」。
static Status checkVectors(uint32_t appBase) noexcept
{
    uint32_t vec[2] = {0U, 0U};

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

// ------------------------------------------------------------------ IAP 命令

enum class IapResult : uint8_t {
    Idle   = 0,
    Done   = 1,
    Failed = 2,
};

namespace {

// 命令码（CMD 的低 7 位，应答时或上 proto::kDirReply）
constexpr uint8_t kCmdStart  = 0x01U;
constexpr uint8_t kCmdData   = 0x02U;
constexpr uint8_t kCmdEnd    = 0x03U;
constexpr uint8_t kCmdStatus = 0x04U;

// 应答码，与设计文档 §0.5.7 一致。
// 0x01 / 0x02 由载体层静默丢弃（重同步失败、长度越界），从机不会主动上报。
enum class Code : uint8_t {
    Ok         = 0x00,
    Crc8       = 0x01,
    Len        = 0x02,
    Addr       = 0x03,
    UnknownCmd = 0x04,
    Param      = 0x05,
    State      = 0x06,
    AddrGap    = 0x07,
    CumCrc     = 0x08,
    EraseFail  = 0x09,
    WriteFail  = 0x0A,
    VerifyFail = 0x0B,
    Vectors    = 0x0C,
    TotalCrc   = 0x0D,
    Redundant  = 0x0E,
    Readback   = 0x0F,
};

constexpr uint16_t kStartLen  = 16U;      // size4 + crc32_4 + sp4 + pc4
constexpr uint16_t kDataHead  = 14U;      // addr4 + total2 + index2 + vlen2 + cumCrc32_4
constexpr uint16_t kBlockSize = BL_BLOCK_SIZE;

static_assert(kBlockSize > 0U && kBlockSize <= (proto::kDataMax - kDataHead),
              "BL_BLOCK_SIZE 放不进载体 Data 区（上限 1024 - 14）");

// 总包数字段是 2 字节，编译期挡一下：APP 区分区变大到超过 65535 个块时，
// 这个字段会静默截断（主机与从机对不上，升级必然失败）。
static_assert(((BL_APP_SIZE / kBlockSize) + 1U) <= 0xFFFFU,
              "BL_APP_SIZE / BL_BLOCK_SIZE 超过 uint16，总包数字段会截断");

} // namespace

// 一次升级会话：等 START → 逐帧收 → END 提交。
// 状态与 Flash 的对应关系靠 nextAddr_ 唯一确定，任何失败都不推进它。
class Session {
public:
    struct Result {
        IapResult outcome = IapResult::Idle;
        uint32_t  fwSize  = 0U;
        uint32_t  fwCrc32 = 0U;
        Status    error   = Status::Ok;
        bool      erased  = false;      // 本次是否动过 Flash（决定能不能直接跳回 APP）
    };

    // 返回即本次会话结束。waitStartMs 是等第一帧的超时；0 表示无限等。
    Result run(uint32_t waitStartMs) noexcept;

private:
    enum class State : uint8_t { Idle, Receiving };

    bool readFrame(uint32_t timeoutMs) noexcept;
    void handle(const proto::Frame& f) noexcept;
    void onStart(const proto::Frame& f) noexcept;
    void onData(const proto::Frame& f) noexcept;
    void onEnd() noexcept;
    void onStatus() noexcept;

    void reply(uint8_t cmd, uint8_t code, const void* body, uint16_t bodyLen) noexcept;
    void replyCode(uint8_t cmd, uint8_t code) noexcept;
    void replyData(uint8_t code) noexcept;

    State         state_ = State::Idle;
    proto::Parser parser_;
    proto::Frame  frame_;
    uint8_t       tx_[32];           // 应答帧上限 32 字节（设计文档 §0.1）

    uint32_t declaredSize_ = 0U;     // START 声明的镜像大小
    uint32_t declaredCrc_  = 0U;     // START 声明的整镜像 CRC32
    uint32_t nextAddr_     = 0U;     // 期望的下一个写入地址
    uint16_t totalPkts_    = 0U;
    Crc32    crc_;                   // 只覆盖 [appBase+8, nextAddr_)，且只由「读回 Flash」推进
    uint8_t  entry_[8]     = {0U};   // START 带来的 SP / PC
    bool     erased_       = false;
    bool     done_         = false;
    Result   result_;
};

// 按扇区擦除 [from, end)。from 必须是扇区起点（分区布局已保证）。
static bool eraseRegion(uint32_t from, uint32_t end) noexcept
{
    uint32_t addr = from;

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

bool Session::readFrame(uint32_t timeoutMs) noexcept
{
    const uint32_t start = tickMs();

    for (;;) {
        uint8_t  b   = 0U;
        uint32_t got = 0U;
        const Status st = uartRead(&b, 1U, BL_READ_SLICE_MS, &got);

        if (got == 1U && parser_.feed(b, frame_)) {
            // 一帧到来了，先过长度自检：帧长必须在 [kFrameMin, kFrameMax] 内。
            // 解析器已保证，这里是第二道防线 —— 长度可疑的帧直接丢，让主机重传。
            if (!frame_.lenOk()) {
                continue;
            }
            return true;
        }
        if (st != Status::Ok && st != Status::Timeout) {
            return false;
        }
        if (timeoutMs != 0U && (tickMs() - start) >= timeoutMs) {
            return false;                           // 自上一帧起静默超时
        }
    }
}

// bodyLen 上限 24：应答总长 = 7（外壳）+ 1（状态码）+ body ≤ 32，见设计文档 §0.1
void Session::reply(uint8_t cmd, uint8_t code, const void* body, uint16_t bodyLen) noexcept
{
    uint8_t pay[25];

    if (bodyLen > (sizeof(pay) - 1U)) {
        return;                                 // 越界的应答宁可不发，也不能踩栈
    }
    pay[0] = code;
    if (bodyLen > 0U && body != nullptr) {
        std::memcpy(&pay[1], body, bodyLen);
    }

    const uint32_t n = proto::encode(BL_DEVICE_ID,
                                     static_cast<uint8_t>(cmd | proto::kDirReply),
                                     pay, static_cast<uint16_t>(1U + bodyLen),
                                     tx_, sizeof(tx_));
    if (n == 0U) {
        return;
    }
    (void)uartWrite(tx_, n);
}

void Session::replyCode(uint8_t cmd, uint8_t code) noexcept
{
    reply(cmd, code, nullptr, 0U);
}

// 数据应答：把从机侧的累积 CRC32 与期望地址交给主机比对
void Session::replyData(uint8_t code) noexcept
{
    uint8_t body[8];

    proto::putLe32(&body[0], crc_.value());
    proto::putLe32(&body[4], nextAddr_);
    reply(kCmdData, code, body, 8U);
}

void Session::onStart(const proto::Frame& f) noexcept
{
    if (f.len < kStartLen) {
        replyCode(kCmdStart, static_cast<uint8_t>(Code::Param));
        return;
    }

    const uint32_t size  = proto::getLe32(&f.data[0]);
    const uint32_t crc32 = proto::getLe32(&f.data[4]);
    const uint32_t sp    = proto::getLe32(&f.data[8]);
    const uint32_t pc    = proto::getLe32(&f.data[12]);

    if (size < 16U || size > BL_APP_SIZE) {
        replyCode(kCmdStart, static_cast<uint8_t>(Code::Param));
        return;
    }
    if (!vectorsSane(sp, pc)) {
        replyCode(kCmdStart, static_cast<uint8_t>(Code::Vectors));
        return;
    }

    // 擦除即等于作废现有固件：APP 区前两个字变 0xFF，上电自然停在 IAP
    if (!eraseRegion(BL_APP_BASE, BL_APP_BASE + size)) {
        replyCode(kCmdStart, static_cast<uint8_t>(Code::EraseFail));
        return;
    }

    proto::putLe32(&entry_[0], sp);
    proto::putLe32(&entry_[4], pc);
    declaredSize_ = size;
    declaredCrc_  = crc32;
    nextAddr_     = BL_APP_BASE + 8U;           // SP/PC 由 START 带来，数据流从 +8 开始
    totalPkts_    = static_cast<uint16_t>((size - 8U + (kBlockSize - 1U)) / kBlockSize);
    crc_.reset();
    erased_ = true;
    state_  = State::Receiving;

    uint8_t body[2];
    proto::putLe16(&body[0], kBlockSize);
    reply(kCmdStart, static_cast<uint8_t>(Code::Ok), body, 2U);
}

void Session::onData(const proto::Frame& f) noexcept
{
    if (state_ != State::Receiving) {
        replyCode(kCmdData, static_cast<uint8_t>(Code::State));
        return;
    }
    if (f.len < kDataHead) {
        replyCode(kCmdData, static_cast<uint8_t>(Code::Param));
        return;
    }

    const uint8_t* p  = f.data;
    const uint32_t addr  = proto::getLe32(&p[0]);
    const uint16_t total = proto::getLe16(&p[4]);
    const uint16_t index = proto::getLe16(&p[6]);
    const uint16_t vlen  = proto::getLe16(&p[8]);
    const uint32_t cum   = proto::getLe32(&p[10]);
    const uint8_t* payload = p + kDataHead;

    // 冗余字段交叉校验：任何一条不符即拒绝，绝不「挑一个相信」
    if (vlen != static_cast<uint16_t>(f.len - kDataHead) ||
        vlen == 0U || vlen > kBlockSize || total != totalPkts_) {
        replyCode(kCmdData, static_cast<uint8_t>(Code::Redundant));
        return;
    }

    if (addr < nextAddr_) {                     
        replyData(static_cast<uint8_t>(Code::Ok));
        return;
    }
    if (addr != nextAddr_) {                              // 跳号：告诉主机从哪里续发
        replyData(static_cast<uint8_t>(Code::AddrGap));   // 与其它 DATA 应答同格式
        return;
    }
    if (index != static_cast<uint16_t>((addr - BL_APP_BASE - 8U) / kBlockSize)) {
        replyCode(kCmdData, static_cast<uint8_t>(Code::Redundant));
        return;
    }
    if ((addr + vlen) > (BL_APP_BASE + declaredSize_)) {
        replyCode(kCmdData, static_cast<uint8_t>(Code::Addr));
        return;
    }

    // 规则一：先校验、后写入。Flash 只能 1→0，先写坏数据就要整扇区擦除才能纠正。
    // 前面每一帧都已验证过，所以一旦不符，错的一定是本帧。
    Crc32 candidate = crc_;
    candidate.update(payload, vlen);
    if (candidate.value() != cum) {
        replyData(static_cast<uint8_t>(Code::CumCrc));
        return;
    }

    if (!ok(flashWrite(addr, payload, vlen))) {
        replyCode(kCmdData, static_cast<uint8_t>(Code::WriteFail));
        return;
    }

    // 规则四：读回 Flash 重算。只对「收到的字节」累加是没意义的 —— 那是同一串字节
    // 在两端各算一次，必然相同；读回才算认证了真正落盘的内容（顺带覆盖 CRC8 漏检
    // 与 Flash 静默写失败两类问题）。
    Crc32 written = crc_;
    if (!crcOfFlash(addr, vlen, written) || written.value() != candidate.value()) {
        replyData(static_cast<uint8_t>(Code::Readback));
        return;                                 // crc_ 未推进 → 主机重传可重写
    }

    crc_      = written;
    nextAddr_ = addr + vlen;
    replyData(static_cast<uint8_t>(Code::Ok));
}

void Session::onEnd() noexcept
{
    if (state_ != State::Receiving) {
        replyCode(kCmdEnd, static_cast<uint8_t>(Code::State));
        return;
    }
    if (crc_.value() != declaredCrc_) {         // 逐帧累积值与 START 声明的比对
        replyCode(kCmdEnd, static_cast<uint8_t>(Code::TotalCrc));
        return;
    }
    if (crc32Flash(BL_APP_BASE + 8U, declaredSize_ - 8U) != declaredCrc_) {
        replyCode(kCmdEnd, static_cast<uint8_t>(Code::VerifyFail));
        return;
    }

    // 提交：写回 SP/PC。过了这里固件才可启动（这是唯一会写 APP 区前 8 字节的地方）。
    if (!ok(flashWrite(BL_APP_BASE, entry_, sizeof(entry_)))) {
        replyCode(kCmdEnd, static_cast<uint8_t>(Code::WriteFail));
        return;
    }

    uint8_t body[4];
    proto::putLe32(&body[0], declaredSize_);
    reply(kCmdEnd, static_cast<uint8_t>(Code::Ok), body, 4U);

    result_.fwSize  = declaredSize_;
    result_.fwCrc32 = declaredCrc_;
    done_           = true;
}

// 仅用于异常恢复：从机复位后主机重新对表
void Session::onStatus() noexcept
{
    const bool recv = (state_ == State::Receiving);
    uint8_t body[8];

    proto::putLe32(&body[0], recv ? (nextAddr_ - BL_APP_BASE - 8U) : 0U);
    proto::putLe32(&body[4], recv ? nextAddr_ : 0U);
    reply(kCmdStatus, static_cast<uint8_t>(Code::Ok), body, 8U);
}

void Session::handle(const proto::Frame& f) noexcept
{
    if (f.isReply()) {
        return;                                 // 从机不会收到应答帧
    }
    if (f.id != BL_DEVICE_ID) {
        return;                                 // 不是给本机的：静默丢弃，连错误也不回
    }

    switch (f.code()) {
    case kCmdStart:  onStart(f);  break;
    case kCmdData:   onData(f);   break;
    case kCmdEnd:    onEnd();     break;
    case kCmdStatus: onStatus();  break;
    default:
        replyCode(static_cast<uint8_t>(f.code()), static_cast<uint8_t>(Code::UnknownCmd));
        break;
    }
}

Session::Result Session::run(uint32_t waitStartMs) noexcept
{
    result_     = Result{};
    state_      = State::Idle;
    erased_     = false;
    done_       = false;
    crc_.reset();
    parser_.reset();
    uartFlushRx();                              // 清接收路径并打开接收中断

    log::mute(true);

    for (;;) {
        const uint32_t waitMs = (state_ == State::Receiving) ? BL_IDLE_TIMEOUT_MS : waitStartMs;

        if (!readFrame(waitMs)) {
            result_.outcome = IapResult::Failed;
            result_.error   = Status::Timeout;
            if (state_ == State::Receiving) {
                BL_LOG("[session] idle timeout, back to idle\r\n");
                state_ = State::Idle;
            }
            break;
        }

        handle(frame_);
        if (done_) {
            result_.outcome = IapResult::Done;
            break;
        }
    }

    log::mute(false);
    result_.erased = erased_;
    return result_;
}

// ------------------------------------------------------------------ 启动决策

class Boot {
public:
    enum class Action : uint8_t {
        JumpToApp,
        EnterIap,          // 留在 IAP 无限等
        EnterIapTimed,     // 软件复位唤回的限时窗口
    };

    struct Decision {
        Action      action;
        const char* reason;

        Decision() = default;
        constexpr Decision(Action a, const char* r) noexcept : action(a), reason(r) {}
    };

    static Decision decide() noexcept;
    static void     jump(uint32_t appBase) noexcept;
};

Boot::Decision Boot::decide() noexcept
{
    // 复位原因必须每次启动都读（read-and-clear），否则旧标志会累积到下次启动
    const ResetCause cause = resetCause();
    BL_LOG("[boot] reset cause = %lu\r\n", static_cast<unsigned long>(cause));

    // 按住硬件按钮上电：人就在板子旁边，意图明确 → 无限等
    if (bootPinHeld()) {
        return { Action::EnterIap, "boot pin held" };
    }

    // 向量表非法 → 没有可启动的固件（空片 / 传输中途掉电）
    if (!ok(checkVectors(BL_APP_BASE))) {
        return { Action::EnterIap, "no bootable firmware" };
    }

    // 软件复位 → APP 在唤回，进限时窗口
    if (cause == ResetCause::Software) {
        return { Action::EnterIapTimed, "soft reset -> recall window" };
    }

    return { Action::JumpToApp, "ok" };
}

void Boot::jump(uint32_t appBase) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n", static_cast<unsigned long>(appBase));
    jumpToApp(appBase);
    BL_LOG("[boot] jump failed!\r\n");
}

// ------------------------------------------------------------------ 入口

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
        case IapResult::Idle:   return "IDLE";
        case IapResult::Done:   return "DONE";
        case IapResult::Failed: return "FAILED";
        default:                return "?";
    }
}

// 无法继续时停在这里（Bootloader 自己永不被擦，停在原地等调试）
[[noreturn]] void fatal(const char* why) noexcept
{
    for (;;) {
        BL_LOG("[main] FATAL: %s\r\n", why);
        for (volatile uint32_t i = 0U; i < 8000000U; ++i) {
        }
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
    if (!partitionAligned("BOOT", BL_BOOT_BASE)) {
        all = false;
    }
    if (!partitionAligned("APP", BL_APP_BASE)) {
        all = false;
    }
    return all;
}

} // namespace

[[noreturn]] void blEntry() noexcept
{
    BL_LOG("\r\n== Bootloader-Everywhere == flash %lu KB, app 0x%08lX + %lu KB\r\n",
           static_cast<unsigned long>(BL_FLASH_SIZE / 1024U),
           static_cast<unsigned long>(BL_APP_BASE),
           static_cast<unsigned long>(BL_APP_SIZE / 1024U));
    
    // 检查BL和APP的基地址分区布局是否合法
    if (!layoutCheck()) {
        fatal("partition layout invalid");
    }

    // BL启动决策
    const Boot::Decision decision = Boot::decide();
    BL_LOG("[main] decision: %s (%s)\r\n", actionName(decision.action), decision.reason);

    // 若决策出是跳转到APP，则跳转到APP
    if (decision.action == Boot::Action::JumpToApp) {
        Boot::jump(BL_APP_BASE);
    }

    const bool timedWindow = (decision.action == Boot::Action::EnterIapTimed);
    bool       everErased  = false;

    static Session session; 

    for (;;) {
        // 一旦擦除过，固件区已不可启动 —— 此后不能再跳 APP，只能无限等下一次 START
        const uint32_t waitMs =
            everErased ? 0U : (timedWindow ? BL_RECALL_WINDOW_MS : 0U);

        // 运行 IAP 会话循环，等待主机指令
        const Session::Result r = session.run(waitMs);
        everErased = everErased || r.erased;

        BL_LOG("[main] outcome=%s size=%lu\r\n", outcomeName(r.outcome), static_cast<unsigned long>(r.fwSize));

        // IAP下载完成，跳转到 APP
        if (r.outcome == IapResult::Done) {
            BL_LOG("[main] download done, jumping to app\r\n");
            delayMs(50);             
            Boot::jump(BL_APP_BASE);
        }

        // 软件复位唤回窗口内没人来，且没动过 Flash → 跳回 APP
        if (!everErased && r.error == Status::Timeout) {
            BL_LOG("[main] no host, jumping to app\r\n");
            Boot::jump(BL_APP_BASE);
        }

        delayMs(20);
    }
}

} // namespace bl

// 对外入口
extern "C" void blRun(void)
{
    bl::blEntry();
}

extern "C" void blUartRx(void)
{
    bl::uartRxIrqHandler();
}
