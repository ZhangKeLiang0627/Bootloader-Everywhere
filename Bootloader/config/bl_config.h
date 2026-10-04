/**
 * @file    bl_config.h
 * @brief   Bootloader 编译期配置 —— 移植新平台时主要修改本文件
 *
 * 约束：BL_BOOT_BASE / BL_APP_BASE / BL_META_BASE 三个地址
 *       必须落在目标芯片 Flash 的扇区（页）起始边界上，
 *       否则擦除操作会误伤相邻区域。
 */
#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/* ========================================================================
 * 一、Flash 分区（默认目标：STM32F405RGT6，1MB）
 * ======================================================================*/

/** Flash 整体信息 */
#define BL_FLASH_BASE               0x08000000UL
#define BL_FLASH_SIZE               (1024UL * 1024UL)

/**
 * Bootloader 区：S0，16KB
 * 常驻，升级流程永不擦写。
 */
#define BL_BOOT_BASE                0x08000000UL
#define BL_BOOT_SIZE                (16UL * 1024UL)

/**
 * APP 区：S1 - S10，880KB
 * Keil 的 IROM1 起始地址应设为 BL_APP_BASE，大小 BL_APP_SIZE。
 */
#define BL_APP_BASE                 0x08004000UL
#define BL_APP_SIZE                 (880UL * 1024UL)

/**
 * 配置区：S11，128KB
 * 存放固件状态机、长度、CRC32、启动计数。
 * 只用到前部若干槽位，整扇区归它独占（擦除的最小单位）。
 */
#define BL_META_BASE                0x080E0000UL
#define BL_META_SIZE                (128UL * 1024UL)

/* ---- 编译期自检：分区不重叠且铺满整个 Flash ---- */
#if (BL_BOOT_BASE + BL_BOOT_SIZE) > BL_APP_BASE
#error "分区重叠：Bootloader 区与 APP 区"
#endif
#if (BL_APP_BASE + BL_APP_SIZE) > BL_META_BASE
#error "分区重叠：APP 区与配置区"
#endif
#if (BL_META_BASE + BL_META_SIZE) > (BL_FLASH_BASE + BL_FLASH_SIZE)
#error "分区越界：配置区超出 Flash 末尾"
#endif

/** APP 向量表基址（供 APP 侧引用，Bootloader 侧用它计算跳转地址） */
#define BL_APP_VTOR                 (BL_APP_BASE)

/* ========================================================================
 * 二、配置区槽位参数
 * ======================================================================*/

/** 单个槽位字节数（须为 Flash 编程单位的整数倍） */
#define BL_META_SLOT_SIZE           64UL

/** 槽位总数 = 128KB / 64B = 2048 */
#define BL_META_SLOT_COUNT          (BL_META_SIZE / BL_META_SLOT_SIZE)

/* ========================================================================
 * 三、启动行为
 * ======================================================================*/

/** 上电后监听 backdoor 字符的时间窗（毫秒）；设为 0 表示关闭该功能 */
#define BL_BACKDOOR_WINDOW_MS       300UL

/** backdoor 触发字符（默认 DEL，串口工具里好按且不冲突） */
#define BL_BACKDOOR_CHAR            0x7Fu

/** 最大连续启动尝试次数；超过则判定固件有运行时缺陷并回滚 */
#define BL_BOOT_MAX_ATTEMPTS        3UL

/** 启动时是否额外做整镜像 CRC32 校验（更安全，代价是每次上电多花些时间） */
#define BL_BOOT_VERIFY_CRC32        1

/** 是否启用看门狗 */
#define BL_USE_WATCHDOG             1

/* ========================================================================
 * 四、通信参数
 * ======================================================================*/

/** IAP 阶段串口波特率 */
#define BL_UART_BAUDRATE            115200UL

/** YMODEM 单帧数据最大字节数（1K 模式为 1024） */
#define BL_YMODEM_BLOCK_SIZE        1024U

/** 单个数据包接收超时（毫秒） */
#define BL_YMODEM_PACKET_TIMEOUT_MS 3000UL

/** 等 'C' 握手的最长时间（毫秒）；超时则放弃升级并尝试启动 APP */
#define BL_YMODEM_HANDSHAKE_MS      15000UL

/** 连续重传请求次数上限，超过则中止本次升级 */
#define BL_YMODEM_MAX_RETRY         10U

/** 单帧内允许的最大连续重试（NAK）次数 */
#define BL_YMODEM_MAX_NAK           5U

/* ========================================================================
 * 五、调试输出
 * ======================================================================*/

/** 是否通过串口输出调试信息（注意：会占用 IAP 的串口带宽） */
#define BL_DEBUG_LOG                1

/** 调试输出宏 */
#if BL_DEBUG_LOG
    #include <stdio.h>
    #define BL_LOG(...)             do { bl_port_uart_printf(__VA_ARGS__); } while (0)
#else
    #define BL_LOG(...)             do { } while (0)
#endif

#endif /* BL_CONFIG_H */
