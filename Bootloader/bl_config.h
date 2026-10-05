#ifndef BL_CONFIG_H
#define BL_CONFIG_H

// Flash 分区。换芯片只改 2 个数（照数据手册填），其余都是派生值：
//   STM32F401xE  512KB → BOOT 16KB | APP 496KB
//   STM32F405/7  1MB   → BOOT 16KB | APP 1008KB
// APP 区必须落在扇区起点上：Flash 只能整扇区擦，差一个字节会连邻近区一起擦掉。
#define BL_FLASH_BASE           0x08000000UL   // 架构常量，不配置：Flash 别名区（ARMv7-M 约定）

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

// ---- 升级协议 ----

// 从机地址（载体帧第 2 字节）。单机场景用 0x01；多从机时每台一个地址。
#ifndef BL_DEVICE_ID
#define BL_DEVICE_ID            0x01U
#endif

// 每帧携带的固件数据量。载体 Data 区上限 1024，减去 DATA 头部 14 字节 → 最大 1010。
#ifndef BL_BLOCK_SIZE
#define BL_BLOCK_SIZE           512U
#endif

// 单次 uartRead 的等待切片。决定空闲超时的检查间隔。
#ifndef BL_READ_SLICE_MS
#define BL_READ_SLICE_MS        20UL
#endif

// 升级途中（已 START）连续多久收不到有效帧就判定主机放弃，退回等 START
#ifndef BL_IDLE_TIMEOUT_MS
#define BL_IDLE_TIMEOUT_MS      30000UL
#endif

// 软件复位唤回窗口：APP 软复位后等上位机的时长；超时且未擦除过则跳回 APP
#ifndef BL_RECALL_WINDOW_MS
#define BL_RECALL_WINDOW_MS     15000UL
#endif

// 日志开关（BL_DEBUG_LOG / BL_LOG_DURING_TRANSFER）与 BL_LOG 宏在 bl_log.h，
// 它们自带默认值，需要覆盖时在那里改或从编译选项 -D 传入。

#endif /* BL_CONFIG_H */
