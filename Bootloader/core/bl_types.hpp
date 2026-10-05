/**
 * @file    bl_types.hpp
 * @brief   公共类型定义
 *
 * 约束：不使用异常、动态内存、RTTI（编译器已关闭），失败通过 Status 返回。
 */
#ifndef BL_TYPES_HPP
#define BL_TYPES_HPP

#include <cstdint>
#include <cstddef>

namespace bl {

/// 返回码
enum class Status : int32_t {
    Ok        =  0,   ///< 成功
    Error     = -1,   ///< 通用失败
    BadParam  = -2,   ///< 参数非法
    Timeout   = -3,   ///< 超时
    FlashFail = -4,   ///< Flash 操作失败
    CrcFail   = -5,   ///< CRC 校验失败
    Protocol  = -6,   ///< 协议帧错误
    NoSpace   = -7,   ///< 空间不足
    BadState  = -8,   ///< 状态不允许该操作
    Cancelled = -9,   ///< 被上位机取消
};

constexpr bool ok(Status s) noexcept { return s == Status::Ok; }

/// 固件状态。用魔数而非顺序值，以区别于 Flash 擦除态（0xFFFFFFFF）
enum class FwState : uint32_t {
    Invalid  = 0x494E564CUL,   ///< 无有效固件
    Download = 0x444C4F41UL,   ///< 升级中，绝不可跳转
    Valid    = 0x56414C44UL,   ///< 已确认可用
    Erased   = 0xFFFFFFFFUL,   ///< 擦除态，等同 Invalid
};

constexpr bool bootable(FwState s) noexcept { return s == FwState::Valid; }

constexpr const char* to_string(FwState s) noexcept
{
    return (s == FwState::Invalid)  ? "INVALID"  :
           (s == FwState::Download) ? "DOWNLOAD" :
           (s == FwState::Valid)    ? "VALID"    :
           (s == FwState::Erased)   ? "ERASED"   : "UNKNOWN";
}

/// 升级会话结果
enum class IapResult : uint8_t {
    Idle    = 0,   ///< 尚未开始
    Done    = 1,   ///< 传输并校验成功
    Failed  = 2,   ///< 失败（超时/CRC/协议）
    Aborted = 3,   ///< 被上位机取消
    NoSpace = 4,   ///< 固件超出 APP 区
};

/// 向量表合法性判据（供镜像校验使用）
struct VectorCheck {
    uint32_t initial_sp;      ///< 初始栈顶
    uint32_t reset_handler;   ///< 复位入口
    bool     sp_in_sram;      ///< 栈顶是否落在 SRAM 内
    bool     entry_is_thumb;  ///< 入口是否为 Thumb 地址（最低位为 1）
};

} // namespace bl

#endif /* BL_TYPES_HPP */
