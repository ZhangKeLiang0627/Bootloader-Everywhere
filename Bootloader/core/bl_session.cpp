/**
 * @file    bl_session.cpp
 * @brief   升级会话编排的实现
 */
#include "core/bl_session.hpp"
#include "core/bl_meta.hpp"
#include "core/bl_crc.hpp"
#include "core/bl_verify.hpp"
#include "port/bl_port.hpp"
#include "core/bl_log.hpp"

namespace bl {

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

} // namespace bl
