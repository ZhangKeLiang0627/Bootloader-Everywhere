// Bootloader-Everywhere 唯一的对外头文件。用法见 README.md，移植接口见 bl_port.h

#ifndef BL_H
#define BL_H

#include "bl_config.h"

#include <stdint.h>         

#ifdef __cplusplus

namespace bl {

enum class Status : int32_t {
    Ok        =  0,
    BadParam  = -2,
    Timeout   = -3,
    FlashFail = -4,
    CrcFail   = -5,
};

constexpr bool ok(Status s) noexcept { return s == Status::Ok; }

} // namespace bl

#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

// 跑 Bootloader：决定跳 APP 还是留在 IAP 收固件。永不返回
// 调用前宿主必须初始化好：时钟、串口（8N1）、Flash 接口时钟，以及按钮引脚
void blRun(void);

// 库的串口接收入口。在宿主的 USARTx_IRQHandler 里调用它
void blUartRx(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_H */
