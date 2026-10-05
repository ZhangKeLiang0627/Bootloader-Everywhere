/**
 * @file    bl_session.hpp
 * @brief   升级会话编排 —— 把 YMODEM、配置区、镜像校验串成一次原子升级
 *
 * 职责：实现「防变砖」的原子提交流程。顺序是安全的核心：
 *
 *   ① meta.mark_download()      置状态为 Download（不可信）
 *   ② 擦除 APP 区所需扇区
 *   ③ 接收 YMODEM 数据并写入
 *   ④ 边收边算整镜像 CRC32
 *   ⑤ meta.commit()             置状态为 Valid，记录 size / crc32
 *
 * ①必须在②之前，否则擦除中途掉电会留下「Valid 但 APP 已擦空」的必砖组合。
 * CRC32 由本端边收边算（帧级 CRC16 已保证传输正确），存下来供启动时重算比对。
 */
#ifndef BL_SESSION_HPP
#define BL_SESSION_HPP

#include "core/bl_ymodem.hpp"
#include "core/bl_crc.hpp"

namespace bl {

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

} // namespace bl

#endif /* BL_SESSION_HPP */
