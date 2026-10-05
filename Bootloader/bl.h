/**
 * @file    bl.h
 * @brief   LUMOS-bootloader 唯一的对外头文件
 *
 * ============================================================================
 *  怎么用（三步）
 * ============================================================================
 *
 *   1. 把整个 Bootloader/ 目录拷进你的工程，加一个 include 路径：
 *          -I <工程>/Bootloader
 *
 *   2. 把这几个文件加入编译：
 *          bl.cpp                       库本体（不用改）
 *          bl_port_stm32f4.cpp          移植实现（换芯片就换这一份）
 *          <你的启动文件> + <芯片 HAL/CMSIS>
 *
 *   3. 在你的 main 里调一句：
 *          #include "bl.h"
 *          int main(void) { bl_run(); }        // 不会返回
 *      或者让库自带 main()：什么都不用写（bl_config.h 里 BL_PROVIDE_MAIN=1）
 *
 * ============================================================================
 *  想改功能去哪儿改
 * ============================================================================
 *
 *   改分区 / 波特率 / 芯片型号  →  bl_config.h
 *   换芯片 / 换串口引脚        →  bl_port_stm32f4.cpp（照它再写一份）
 *   改协议 / 改升级流程        →  bl.cpp
 *
 * ============================================================================
 *  本文件提供什么
 * ============================================================================
 *
 *   bl_run()             Bootloader 主入口（调了就不返回）
 *   bl_request_update()  APP 侧用：请求回到 Bootloader 刷机（可选）
 *   Status / FwState / IapResult   公共类型
 *   BL_LOG(...)          轻量日志宏（定义在 bl_config.h）
 */
#ifndef BL_H
#define BL_H

#include "bl_config.h"

#include <cstddef>
#include <cstdint>

namespace bl {

/* ==========================================================================
 * 返回码
 * ==========================================================================*/
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

/* ==========================================================================
 * 固件状态（存在配置区里）
 *
 *   Invalid ──(开始升级)──► Download ──(写完且校验通过)──► Valid
 *
 *   Invalid  无固件 / 无有效记录      → 留在 IAP
 *   Download 正在写 / 写到一半掉电    → 留在 IAP（可重刷，不会变砖）
 *   Valid    写入完成且校验通过        → 跳转 APP
 *
 * 用 ASCII 魔数而不是 0/1/2，是为了和 Flash 擦除态（全 0xFF）天然区分开。
 * ==========================================================================*/
enum class FwState : uint32_t {
    Invalid  = 0x494E564CUL,   ///< "INVL"
    Download = 0x444C4F41UL,   ///< "DLOA"
    Valid    = 0x56414C44UL,   ///< "VALD"
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

/* ==========================================================================
 * 一次升级会话的结果
 * ==========================================================================*/
enum class IapResult : uint8_t {
    Idle    = 0,   ///< 尚未开始
    Done    = 1,   ///< 传输并校验成功
    Failed  = 2,   ///< 失败（超时 / CRC / 协议）
    Aborted = 3,   ///< 被上位机取消
    NoSpace = 4,   ///< 固件超出 APP 区
};

/* ==========================================================================
 * 日志（BL_LOG 宏的后端）
 *
 * bl_config.h 里的 BL_LOG(...) 就是它。故意不用 stdio：标准 vsnprintf 会
 * 连带格式化与浮点支持吃掉约 6.5KB ROM，而 Bootloader 只有 16KB 可用。
 * 格式符与 C 的 printf 一致（%s %c %d %u %x %l…），不支持浮点。
 * ==========================================================================*/
namespace log {

void printf(const char* fmt, ...) noexcept;

} // namespace log

/* ==========================================================================
 * 向量表检查结果（判断 APP 到底能不能启动）
 * ==========================================================================*/
struct VectorCheck {
    uint32_t initial_sp;      ///< 初始栈顶（向量表第 0 个字）
    uint32_t reset_handler;   ///< 复位入口（向量表第 1 个字）
    bool     sp_in_sram;      ///< 栈顶是否落在合法 RAM 内
    bool     entry_is_thumb;  ///< 入口是否为 Thumb 地址（最低位为 1）
};

} // namespace bl

/* ==========================================================================
 * 库的入口（C 链接：C / C++ 工程都能直接调）
 * ==========================================================================*/
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 跑 Bootloader。内部永不返回。
 *
 * 做三件事：
 *   1. 初始化平台、串口、Flash，检查分区布局（不对就直接停在报错）
 *   2. 决定「跳 APP」还是「留在 IAP」
 *   3. 留在 IAP 时循环等 YMODEM 刷机；升级成功后直接跳新固件
 */
void bl_run(void);

#ifdef __cplusplus
}
#endif

/* ==========================================================================
 * APP 侧接口（可选）
 *
 * 想让「运行中的 APP」被网页一键唤回刷机，就在你的 APP 里：
 *   1. 串口逐字节匹配关键字 BL_BOOT_MAGIC_STRING（状态机，匹配完整才算）
 *   2. 匹配成功就调 bl_request_update()
 *
 * Bootloader 靠「复位原因 = 软件复位 + 固件 Valid」识别这次唤回，
 * 然后开一个限时窗口等上位机。不需要这个功能就整段忽略。
 * ==========================================================================*/
#ifndef BL_NO_APP_API

/** 上位机发给 APP 的唤回关键字（网页端发同一个字符串） */
#ifndef BL_BOOT_MAGIC_STRING
#define BL_BOOT_MAGIC_STRING    "#Bootloader-Everywhere"
#endif

/**
 * @brief 请求进入 Bootloader：只做一次软件复位，不写任何标志。
 *
 * 为什么不用 NVIC_SystemReset()：那需要包含 CMSIS 头，会让这个内联函数
 * 依赖芯片头文件。直接写 SCB->AIRCR 效果完全等价，且零依赖 ——
 * 所有 Cortex-M（M0/M0+/M3/M4/M7）的 AIRCR 布局一致。
 */
static inline void bl_request_update(void)
{
    /* AIRCR = VECTKEY(0x05FA) | SYSRESETREQ(bit2) */
    *(volatile uint32_t*)0xE000ED0CUL = 0x05FA0004UL;
    for (;;) { }          /* 兜底：复位是异步的，这里不会往下走 */
}

#endif /* BL_NO_APP_API */

#endif /* BL_H */
