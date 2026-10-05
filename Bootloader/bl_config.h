/**
 * @file    bl_config.h
 * @brief   编译期配置 —— 移植时改这一个文件
 *
 * 目录：
 *   一、目标芯片    选一个预置（或自己填）
 *   二、Flash 分区  由「一」自动派生，一般不用动
 *   三、配置区槽位  记录固件状态的方式
 *   四、启动与通信  波特率、超时、CRC 校验开关
 *   五、调试输出
 *   六、集成方式
 *
 * 硬约束：分区地址必须落在 Flash 扇区（页）的起始边界上 —— Flash 只能整
 *         扇区擦除，差一个字节就会连相邻区域一起擦掉。文件末尾有编译期自检。
 */
#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/* ==========================================================================
 * 一、目标芯片
 *
 * 两种用法：
 *   A. 什么都不做（推荐）—— 构建系统为 HAL 定义的那个 CMSIS 器件宏
 *      （STM32F401xE / STM32F405xx / STM32F407xx）在这里被自动识别。
 *      Keil 里就是 Options → C/C++ → Define 里已有那一项，不用新增。
 *   B. 新芯片 —— 在下面的「手填区」把数字填上即可（照现有预置的格式）。
 * ==========================================================================*/

/* ---- 预置：STM32F401xD/E（512KB Flash / 96KB SRAM / 最高 84MHz） ---- */
#if defined(STM32F401xE) || defined(STM32F401xD) || defined(BL_CHIP_STM32F401XE)
    #define BL_CHIP_NAME            "STM32F401xE"
    #define BL_FLASH_BASE           0x08000000UL
    #define BL_FLASH_SIZE           (512UL * 1024UL)
    #define BL_SRAM_BASE            0x20000000UL
    #define BL_SRAM_END             0x20018000UL      /* 96KB，不含 */
    #define BL_META_SIZE            (128UL * 1024UL)  /* 最后一个扇区 S7 */

    /* 25MHz --(PLLM)--> 1MHz --(PLLN)--> 336MHz --(PLLP)--> 84MHz */
    #define BL_HSE_HZ               25000000UL
    #define BL_SYSCLK_HZ            84000000UL
    #define BL_PLL_M                25UL
    #define BL_PLL_N                336UL
    #define BL_PLL_P                4UL               /* 只允许 2/4/6/8 */
    #define BL_PLL_Q                7UL
    #define BL_AHB_DIV              1UL
    #define BL_APB1_DIV             2UL               /* PCLK1 = 42MHz */
    #define BL_APB2_DIV             1UL               /* PCLK2 = 84MHz */
    #define BL_FLASH_LATENCY        2UL

/* ---- 预置：STM32F405xx / STM32F407xx（1MB Flash / 128KB SRAM / 168MHz） ----
 * 注意：这两颗默认按 8MHz 晶振算。若你的板子是 25MHz（某些 F407 板），
 *       把 BL_HSE_HZ / BL_PLL_M 改成 25 / 25，其余不变。 */
#elif defined(STM32F405xx) || defined(STM32F407xx) || defined(BL_CHIP_STM32F405XX) || defined(BL_CHIP_STM32F407XX)
    #if defined(STM32F405xx) || defined(BL_CHIP_STM32F405XX)
        #define BL_CHIP_NAME        "STM32F405xx"
    #else
        #define BL_CHIP_NAME        "STM32F407xx"
    #endif
    #define BL_FLASH_BASE           0x08000000UL
    #define BL_FLASH_SIZE           (1024UL * 1024UL)
    #define BL_SRAM_BASE            0x20000000UL
    #define BL_SRAM_END             0x20020000UL      /* 128KB，不含 */
    #define BL_META_SIZE            (128UL * 1024UL)  /* 最后一个扇区 */

    /* 8MHz --(PLLM)--> 1MHz --(PLLN)--> 336MHz --(PLLP)--> 168MHz */
    #define BL_HSE_HZ               8000000UL
    #define BL_SYSCLK_HZ            168000000UL
    #define BL_PLL_M                8UL
    #define BL_PLL_N                336UL
    #define BL_PLL_P                2UL
    #define BL_PLL_Q                7UL
    #define BL_AHB_DIV              1UL
    #define BL_APB1_DIV             4UL               /* PCLK1 = 42MHz */
    #define BL_APB2_DIV             2UL               /* PCLK2 = 84MHz */
    #define BL_FLASH_LATENCY        5UL

/* ---- 手填区：新芯片在这里填（填漏会编译报错，不会带错值上板） ---- */
#else
    /* #define BL_CHIP_NAME         "你的芯片名" */
    /* #define BL_FLASH_BASE        0x08000000UL */
    /* #define BL_FLASH_SIZE        (512UL * 1024UL) */
    /* #define BL_SRAM_BASE         0x20000000UL */
    /* #define BL_SRAM_END          0x20010000UL */
    /* #define BL_META_SIZE         (16UL * 1024UL)   最后一个扇区的大小 */
    /* 时钟参数只有用 bl_port_stm32f4.cpp 那份移植实现时才需要 */
#endif

/* ==========================================================================
 * 二、Flash 分区（由上面自动派生）
 *
 *   F401 512KB : BOOT 16KB @0x08000000 | APP 368KB @0x08004000 | META 128KB @0x08060000
 *   F405/F407  : BOOT 16KB @0x08000000 | APP 880KB @0x08004000 | META 128KB @0x080E0000
 *
 * 配置区为什么放末尾：F4 的扇区是「开头小而密、末尾大而整」，末尾一定是一块
 * 完整大扇区；放开头则几个小扇区拼不出干净区域，还会把 APP 区割裂成两段。
 * ==========================================================================*/

/** Bootloader 区大小。16KB 对串口 IAP 足够，且正好占一个扇区 */
#ifndef BL_BOOT_SIZE
#define BL_BOOT_SIZE            (16UL * 1024UL)
#endif

/** 配置区大小 —— 必须等于目标 Flash「最后一个扇区」的大小 */
#ifndef BL_META_SIZE
#error "未配置 BL_META_SIZE：请在第一节选用某个预置，或填上最后一个扇区的大小"
#endif

#define BL_BOOT_BASE            (BL_FLASH_BASE)
#define BL_APP_BASE             (BL_BOOT_BASE + BL_BOOT_SIZE)
#define BL_META_BASE            (BL_FLASH_BASE + BL_FLASH_SIZE - BL_META_SIZE)
#define BL_APP_SIZE             (BL_META_BASE - BL_APP_BASE)
#define BL_FLASH_END            (BL_FLASH_BASE + BL_FLASH_SIZE)

/** APP 向量表基址：APP 工程要把它写进 SCB->VTOR（或 Keil 的 VECT_TAB_OFFSET） */
#define BL_APP_VTOR             (BL_APP_BASE)

/* ---- 编译期自检：分区重叠 / 越界 / 未落在扇区边界 ---- */
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
 * 三、配置区槽位
 *
 * 「日志式追加」：每次状态变更顺序写一个新槽而不擦除，读的时候取序号最大
 * 的有效槽，写满整片后才擦一次。
 *
 * 为什么不原地擦写：一次状态写入只有几十字节、几十微秒；而擦一个 128KB
 * 扇区要 1 秒左右并消耗一次寿命。
 * ==========================================================================*/

/** 单个槽位字节数；必须是 Flash 编程单位的整数倍（F4 是 4 字节） */
#ifndef BL_META_SLOT_SIZE
#define BL_META_SLOT_SIZE       64UL
#endif

#define BL_META_SLOT_COUNT      (BL_META_SIZE / BL_META_SLOT_SIZE)

/* ==========================================================================
 * 四、启动与通信
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
 * 五、调试输出
 *
 * 这是 ROM 占用最大的开关：链接标准 vsnprintf 会连带格式化与浮点支持吃掉
 * 约 6.5KB，而 Bootloader 只有 16KB 可用。所以不用 stdio，改用 bl.cpp 里的
 * 轻量格式化（约 2KB）。可用 -DBL_DEBUG_LOG=0 量「发布版」体积。
 * 用 BL_LOG 的文件只需 include "bl.h"（声明在那里）。
 * ==========================================================================*/
#ifndef BL_DEBUG_LOG
#define BL_DEBUG_LOG            1
#endif

#if BL_DEBUG_LOG
    #define BL_LOG(...)         do { ::bl::log::printf(__VA_ARGS__); } while (0)
#else
    #define BL_LOG(...)         do { } while (0)
#endif

/* ==========================================================================
 * 六、集成方式
 * ==========================================================================*/

/**
 * 库是否自带 main()
 *   1 = 自带（默认）。Bootloader 是一份独立固件时用这个；
 *       宿主工程里自带的 main.c 记得移出编译。
 *   0 = 不自带。把库接进已有工程时用，在你自己的 main() 里调 bl_run()。
 */
#ifndef BL_PROVIDE_MAIN
#define BL_PROVIDE_MAIN         1
#endif

#endif /* BL_CONFIG_H */
