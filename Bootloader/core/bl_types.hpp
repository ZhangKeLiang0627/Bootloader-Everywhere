/**
 * @file    bl_types.hpp
 * @brief   公共类型定义（C++ 版）
 *
 * 编译环境：ARM Compiler 6，C++14，RTTI 关闭。
 * 约束：不使用异常、不使用动态内存、不使用 RTTI。
 *      所有失败通过 bl::Status 返回码表达。
 */
#ifndef BL_TYPES_HPP
#define BL_TYPES_HPP

#include <cstdint>
#include <cstddef>

namespace bl {

/* ========================================================================
 * 返回码
 * ======================================================================*/
enum class Status : int32_t {
    Ok          =  0,   ///< 成功
    Error       = -1,   ///< 通用失败
    BadParam    = -2,   ///< 参数非法
    Timeout     = -3,   ///< 超时
    FlashFail   = -4,   ///< Flash 操作失败
    CrcFail     = -5,   ///< CRC 校验失败
    Protocol    = -6,   ///< 协议帧错误
    NoSpace     = -7,   ///< 空间不足
    BadState    = -8,   ///< 状态不允许该操作
    Cancelled   = -9,   ///< 被上位机取消
};

/// 判断是否成功
constexpr bool ok(Status s) noexcept { return s == Status::Ok; }

/* ========================================================================
 * 固件状态机
 *
 * 用魔数而非 0/1/2 顺序值，以区别于 Flash 擦除态（0xFFFFFFFF）
 * 以及误写入的随机数据。
 * ======================================================================*/
enum class FwState : uint32_t {
    Invalid  = 0x494E564CUL,   ///< "INVL" 无有效固件
    Download = 0x444C4F41UL,   ///< "DLOA" 升级中，绝不可跳转
    Testing  = 0x54455354UL,   ///< "TEST" 已过校验，待 APP 自检确认
    Valid    = 0x56414C44UL,   ///< "VALD" 已确认可用
    Revoked  = 0x5245564BUL,   ///< "REVK" 已回滚作废
    Erased   = 0xFFFFFFFFUL,   ///< 擦除态，等同无效
};

/// 该状态下是否允许尝试启动 APP
constexpr bool bootable(FwState s) noexcept
{
    return s == FwState::Valid || s == FwState::Testing;
}

/// 状态名（调试输出用）
constexpr const char* to_string(FwState s) noexcept
{
    return (s == FwState::Invalid)  ? "INVALID"  :
           (s == FwState::Download) ? "DOWNLOAD" :
           (s == FwState::Testing)  ? "TESTING"  :
           (s == FwState::Valid)    ? "VALID"    :
           (s == FwState::Revoked)  ? "REVOKED"  :
           (s == FwState::Erased)   ? "ERASED"   : "UNKNOWN";
}

/* ========================================================================
 * 升级会话结果
 * ======================================================================*/
enum class IapResult : uint8_t {
    Idle     = 0,   ///< 尚未开始
    Running  = 1,   ///< 正在传输
    Done     = 2,   ///< 传输并校验成功
    Failed   = 3,   ///< 失败（超时 / CRC / 协议）
    Aborted  = 4,   ///< 被上位机主动取消
    NoSpace  = 5,   ///< 固件超出 APP 区
};

/* ========================================================================
 * 向量表合法性判据（供镜像校验使用）
 * ======================================================================*/
struct VectorCheck {
    uint32_t initial_sp;      ///< 向量表首字：初始栈顶
    uint32_t reset_handler;   ///< 向量表次字：复位入口
    bool     sp_in_sram;      ///< 栈顶是否落在 SRAM 范围
    bool     entry_is_thumb;  ///< 入口是否为合法 Thumb 地址（最低位为 1）
};

} // namespace bl

#endif /* BL_TYPES_HPP */
