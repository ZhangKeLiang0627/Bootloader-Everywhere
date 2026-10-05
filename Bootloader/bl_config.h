#ifndef BL_CONFIG_H
#define BL_CONFIG_H

// Flash 分区。换芯片只改这几个数（照数据手册填），其余全是派生值。
//
//   STM32F401xE  512KB → BOOT 16KB | APP 496KB
//   STM32F405/7  1MB   → BOOT 16KB | APP 1008KB
//
// APP 区必须落在扇区起点上：Flash 只能整扇区擦，差一个字节会连邻近区一起擦掉。
// 上电还会用 flashSectorAt() 查扇区表精查一次（表由 port 提供）。
#ifndef BL_FLASH_BASE
#define BL_FLASH_BASE           0x08000000UL
#endif

#ifndef BL_FLASH_SIZE
#define BL_FLASH_SIZE           (512UL * 1024UL)     // F401xE；F405/407 改 1024
#endif

#ifndef BL_BOOT_SIZE
#define BL_BOOT_SIZE            (16UL * 1024UL)      // 正好一个扇区
#endif

#define BL_BOOT_BASE            (BL_FLASH_BASE)
#define BL_APP_BASE             (BL_BOOT_BASE + BL_BOOT_SIZE)
#define BL_APP_END              (BL_FLASH_BASE + BL_FLASH_SIZE)
#define BL_APP_SIZE             (BL_APP_END - BL_APP_BASE)

#if (BL_BOOT_BASE + BL_BOOT_SIZE) > BL_APP_BASE
#error "分区重叠：Bootloader 区与 APP 区"
#endif
#if (BL_APP_SIZE < (64UL * 1024UL))
#error "APP 区不足 64KB：请减小 BL_BOOT_SIZE，或确认 BL_FLASH_SIZE"
#endif

// YMODEM 参数
#ifndef BL_YMODEM_BLOCK_SIZE
#define BL_YMODEM_BLOCK_SIZE    1024U                // 1K 模式；上位机不支持时自动退回 128
#endif
#ifndef BL_YMODEM_PACKET_TIMEOUT_MS
#define BL_YMODEM_PACKET_TIMEOUT_MS 3000UL           // 单帧接收超时
#endif
#ifndef BL_YMODEM_HANDSHAKE_MS
#define BL_YMODEM_HANDSHAKE_MS  15000UL              // 握手总超时，也是「APP 唤回窗口」长度
#endif
#ifndef BL_YMODEM_MAX_RETRY
#define BL_YMODEM_MAX_RETRY     10U                  // 传输途中允许的连续超时次数
#endif
#ifndef BL_YMODEM_MAX_NAK
#define BL_YMODEM_MAX_NAK       5U                   // 单帧内允许的连续 NAK 次数
#endif

// 调试输出。这是 ROM 占用最大的开关（开 ≈3.6KB、关 ≈0）。发布版可用
// -DBL_DEBUG_LOG=0 量体积；不要改用 stdio，标准 vsnprintf 会连带浮点吃掉约 6.5KB。
#ifndef BL_DEBUG_LOG
#define BL_DEBUG_LOG            1
#endif

#if BL_DEBUG_LOG
    #define BL_LOG(...)         do { ::bl::log::printf(__VA_ARGS__); } while (0)
#else
    #define BL_LOG(...)         do { } while (0)
#endif

#endif /* BL_CONFIG_H */
