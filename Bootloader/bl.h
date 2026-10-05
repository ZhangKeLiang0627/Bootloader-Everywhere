// Bootloader-Everywhere 唯一的对外头文件。
// 用法见 Bootloader/README.md；移植接口见 bl_port.h。
#ifndef BL_H
#define BL_H

#include "bl_config.h"

#include <stdint.h>          /* C / C++ 通用，方便 C 工程直接调 blRun() */

#ifdef __cplusplus

namespace bl {

enum class Status : int32_t {
    Ok        =  0,
    BadParam  = -2,
    Timeout   = -3,
    FlashFail = -4,
    CrcFail   = -5,
    Protocol  = -6,
    NoSpace   = -7,
    Cancelled = -9,
};

constexpr bool ok(Status s) noexcept { return s == Status::Ok; }

namespace log {
void printf(const char* fmt, ...) noexcept;
} // namespace log

} // namespace bl

#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

// 跑 Bootloader：决定跳 APP 还是留在 IAP 收固件。内部永不返回。
//
// 调用前宿主必须已经初始化好芯片：时钟、串口（8N1）、Flash 接口时钟。
// 库自己不做任何初始化，也不带 main()。
void blRun(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_H */
