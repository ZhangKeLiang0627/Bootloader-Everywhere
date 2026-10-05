/**
 * @file    bl_config.h
 * @brief   编译期配置：Flash 分区 + 通信参数
 *
 * 只描述「这块 Flash 怎么切」和「串口怎么约定」，不涉及任何芯片外设细节。
 * 时钟怎么配、串口用哪一路、引脚是哪几个 —— 那些是板级事实，在
 * bl_port_<平台>.cpp 里；时钟本身交给宿主工程（CubeMX 生成的
 * SystemClock_Config），库不重复配置一遍。
 */
#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/* ==========================================================================
 * 一、Flash 分区
 *
 * 换芯片就改这 4 个数（照数据手册填），其余全是派生值：
 *
 *   BL_FLASH_SIZE  Flash 容量
 *   BL_META_SIZE   最后一个扇区的大小（配置区就放在那里）
 *   BL_SRAM_BASE / BL_SRAM_END   校验 APP 栈顶是否落在 SRAM 内
 *
 *   STM32F401xE  512KB → BOOT 16KB | APP 368KB | META 128KB
 *   STM32F405/7  1MB   → BOOT 16KB | APP 880KB | META 128KB
 *
 * 配置区为什么放末尾：F4 的扇区「开头小而密、末尾大而整」，末尾一定是一块
 * 完整大扇区；放开头则几个小扇区拼不出干净区域，还会把 APP 区割裂成两段。
 *
 * 硬约束：分区地址必须落在扇区起始边界上 —— Flash 只能整扇区擦除，差一个
 * 字节就会连相邻区域一起擦掉。上方是编译期粗查，上电还会用
 * flashBytesToSectorEnd() 精查一次（见 bl.cpp 的 layoutCheck）。
 * ==========================================================================*/

#ifndef BL_FLASH_BASE
#define BL_FLASH_BASE           0x08000000UL
#endif

#ifndef BL_FLASH_SIZE
#define BL_FLASH_SIZE           (512UL * 1024UL)    /* F401xE；F405/F407 改 1024 */
#endif

#ifndef BL_META_SIZE
#define BL_META_SIZE            (128UL * 1024UL)    /* = 最后一个扇区的大小 */
#endif

#ifndef BL_SRAM_BASE
#define BL_SRAM_BASE            0x20000000UL
#endif

#ifndef BL_SRAM_END
#define BL_SRAM_END             0x20018000UL        /* 96KB，不含；F405/F407 改 0x20020000 */
#endif

/** Bootloader 区大小。16KB 对串口 IAP 足够，且正好占一个扇区 */
#ifndef BL_BOOT_SIZE
#define BL_BOOT_SIZE            (16UL * 1024UL)
#endif

#define BL_BOOT_BASE            (BL_FLASH_BASE)
#define BL_APP_BASE             (BL_BOOT_BASE + BL_BOOT_SIZE)
#define BL_META_BASE            (BL_FLASH_BASE + BL_FLASH_SIZE - BL_META_SIZE)
#define BL_APP_SIZE             (BL_META_BASE - BL_APP_BASE)
#define BL_FLASH_END            (BL_FLASH_BASE + BL_FLASH_SIZE)

/* ---- 编译期自检：重叠 / 越界 / 配置区基址能否落在扇区边界 ---- */
#if (BL_BOOT_BASE + BL_BOOT_SIZE) > BL_APP_BASE
#error "分区重叠：Bootloader 区与 APP 区"
#endif
#if (BL_APP_BASE + BL_APP_SIZE) > BL_META_BASE
#error "分区重叠：APP 区与配置区"
#endif
#if (BL_META_BASE + BL_META_SIZE) > BL_FLASH_END
#error "分区越界：配置区超出 Flash 末尾"
#endif
#if ((BL_FLASH_SIZE % BL_META_SIZE) != 0UL)
#error "Flash 容量不是配置区大小的整数倍：配置区基址无法保证落在扇区边界，请调整 BL_META_SIZE"
#endif
#if (BL_APP_SIZE < (64UL * 1024UL))
#error "APP 区不足 64KB：请减小 BL_BOOT_SIZE / BL_META_SIZE，或确认 BL_FLASH_SIZE"
#endif

/* ==========================================================================
 * 二、配置区记录
 * ==========================================================================*/

/** 单条记录的字节数；必须是 Flash 编程单位的整数倍（F4 是 4 字节） */
#ifndef BL_META_SLOT_SIZE
#define BL_META_SLOT_SIZE       64UL
#endif

/* ==========================================================================
 * 三、启动与通信
 * ==========================================================================*/

/**
 * 启动时是否额外做整镜像 CRC32 校验。
 *   1 = 做（推荐）：挡住 Flash 位翻转 / 擦写不完整
 *   0 = 只做向量表检查，上电更快
 */
#ifndef BL_BOOT_VERIFY_CRC32
#define BL_BOOT_VERIFY_CRC32    1
#endif

/** IAP 阶段串口波特率 */
#ifndef BL_UART_BAUDRATE
#define BL_UART_BAUDRATE        115200UL
#endif

/** YMODEM 单帧数据上限（1024 = 1K 模式；上位机不支持时会自动退回 128） */
#ifndef BL_YMODEM_BLOCK_SIZE
#define BL_YMODEM_BLOCK_SIZE    1024U
#endif

/** 单个数据帧的接收超时（毫秒） */
#ifndef BL_YMODEM_PACKET_TIMEOUT_MS
#define BL_YMODEM_PACKET_TIMEOUT_MS 3000UL
#endif

/**
 * 握手总超时（毫秒）—— 也是「APP 唤回窗口」的长度。
 *
 * APP 软复位唤回 Bootloader 后，Bootloader 只给上位机这么长时间来发第一个
 * YMODEM 包；超时就跳回 APP 继续跑。置 0 = 无限等（传统行为，不推荐）。
 */
#ifndef BL_YMODEM_HANDSHAKE_MS
#define BL_YMODEM_HANDSHAKE_MS  15000UL
#endif

/** 传输途中允许的连续超时次数，超过则中止本次升级 */
#ifndef BL_YMODEM_MAX_RETRY
#define BL_YMODEM_MAX_RETRY     10U
#endif

/** 单帧内允许的连续 NAK 次数 */
#ifndef BL_YMODEM_MAX_NAK
#define BL_YMODEM_MAX_NAK       5U
#endif

/* ==========================================================================
 * 四、调试输出
 *
 * 这是 ROM 占用最大的开关：链接标准 vsnprintf 会连带格式化与浮点支持吃掉
 * 约 6.5KB，而 Bootloader 只有 16KB 可用。所以不用 stdio，改用 bl.cpp 里的
 * 轻量格式化。可用 -DBL_DEBUG_LOG=0 量「发布版」体积。
 * ==========================================================================*/
#ifndef BL_DEBUG_LOG
#define BL_DEBUG_LOG            1
#endif

#if BL_DEBUG_LOG
    #define BL_LOG(...)         do { ::bl::log::printf(__VA_ARGS__); } while (0)
#else
    #define BL_LOG(...)         do { } while (0)
#endif

#endif /* BL_CONFIG_H */
