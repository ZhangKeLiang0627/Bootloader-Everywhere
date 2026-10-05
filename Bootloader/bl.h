// Bootloader-Everywhere 唯一的对外头文件。用法见 README.md，移植接口见 bl_port.h。
#ifndef BL_H
#define BL_H

#include "bl_config.h"

#include <stdint.h>          // C / C++ 通用，方便 C 工程直接调 blRun()

#ifdef __cplusplus

namespace bl {

enum class Status : int32_t {
    Ok        =  0,
    BadParam  = -2,
    Timeout   = -3,
    FlashFail = -4,
    CrcFail   = -5,
    Protocol  = -6,
};

constexpr bool ok(Status s) noexcept { return s == Status::Ok; }

namespace log {
void printf(const char* fmt, ...) noexcept;   // 轻量格式化，只支持 %u %d %X %s %%
} // namespace log

} // namespace bl

#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

// 跑 Bootloader：决定跳 APP 还是留在 IAP 收固件。永不返回。
//
// 调用前宿主必须初始化好：时钟、串口（8N1）、Flash 接口时钟，以及按钮引脚
// （连到 PC0 那类「按住上电进 IAP」的按钮）。库自己不做外设初始化，也不带 main()。
void blRun(void);

// 库的串口接收入口。在宿主的 USARTx_IRQHandler 里调用它，**不要**再调
// HAL_UART_IRQHandler —— 那会把字节收进 HAL 自己的状态机，库里就收不到了。
void blUartRx(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_H */
