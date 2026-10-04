/**
 * @file    bl_config.h
 * @brief   LUMOS-bootloader 编译期配置 —— 移植时唯一需要修改的文件
 *
 * 本文件是「库的唯一配置面」。整个 Bootloader/ 目录可以原样拷进任何
 * 芯片的工程，只需要动这一个文件（外加写一份 target/ 适配）。
 *
 * 结构（移植时从上往下看）：
 *   一、目标芯片   —— 选一个现成预置，或照格式自己填
 *   二、Flash 分区 —— 由「一」自动派生，通常不用动
 *   三、启动行为   —— backdoor、回滚、看门狗
 *   四、通信参数   —— 波特率、YMODEM 超时
 *   五、调试输出
 *
 * 硬约束：BL_BOOT_BASE / BL_APP_BASE / BL_META_BASE 必须正好落在目标芯片
 *         Flash 的扇区（页）起始边界上 —— Flash 只能整扇区擦除，
 *         差一个字节就会把相邻区域一起擦掉。文件末尾有编译期自检。
 */
#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/* ============================================================================
 * 一、目标芯片
 *
 * 三种用法，任选一种：
 *   A. 什么都不用做（推荐）—— 构建系统为 HAL 定义的那个 CMSIS 器件宏
 *      （STM32F401xE / STM32F405xx / …）在这里被复用，自动挑出对应预置。
 *      Keil 里就是 Options → C/C++ → Define 里已有的那一项，无需新增。
 *   B. 显式指定 —— 取消下面某一行的注释，优先于 A。
 *   C. 没有预置的新芯片 —— 保持注释，直接在「手填区」把值填上。
 *
 * 预置里只有数字（容量、时钟树），没有任何代码；
 * 换芯片时它是和 target/ 适配配套的两件事，别只改一半。
 * ==========================================================================*/

/* ---- A/B. 预置（手动指定优先，取消注释即生效） ---- */
// #include "presets/stm32f401xe.h"
// #include "presets/stm32f405xx.h"
// #include "presets/stm32f407xx.h"
// #include "presets/gd32f303re.h"

/* ---- 自动跟随构建系统的器件宏（已手动指定或没有匹配项时跳过） ---- */
#if !defined(BL_CHIP_NAME)
    #if defined(STM32F401xE)
        #include "presets/stm32f401xe.h"
    #elif defined(STM32F405xx)
        #include "presets/stm32f405xx.h"
    #elif defined(STM32F407xx)
        #include "presets/stm32f407xx.h"
    #elif defined(GD32F30X)
        #include "presets/gd32f303re.h"
    #endif
#endif

/* ---------------------------------------------------------------------------
 * 手填区（用预置时无需理会；预置没覆盖到的项可在这里补）
 * -------------------------------------------------------------------------*/

/** 芯片名，只用于启动时打印标识 */
#ifndef BL_CHIP_NAME
#error "未配置目标芯片：请取消 config/presets/ 中某一行的注释，或在此定义 BL_CHIP_NAME"
#endif

/** Flash 起始地址与容量 —— 分区地址全部由它派生 */
#ifndef BL_FLASH_BASE
#define BL_FLASH_BASE               0x08000000UL
#endif
#ifndef BL_FLASH_SIZE
#error "未配置 BL_FLASH_SIZE：参考数据手册的 Flash 容量（如 512KB 写 (512UL*1024UL)）"
#endif

/**
 * 片内 SRAM 范围（半开区间 [BASE, END)）
 *
 * 用途：启动时校验 APP 向量表的「初始栈顶」是否落在合法 RAM 内。
 * 只填主 SRAM 连续段；CCM/TCM 之类不参与。
 */
#ifndef BL_SRAM_BASE
#define BL_SRAM_BASE                0x20000000UL
#endif
#ifndef BL_SRAM_END
#error "未配置 BL_SRAM_END：填 SRAM 结束地址（不含），例如 128KB@0x20000000 写 0x20020000"
#endif

/* ============================================================================
 * 二、Flash 分区
 *
 * 三块：Bootloader（常驻）/ APP（被升级）/ 配置区（记录固件状态）。
 * 这里只做地址派生 —— 改容量就能自动得到正确布局：
 *
 *   F401 512KB : BOOT 16KB @0x08000000 | APP 368KB @0x08004000 | META 128KB @0x08060000
 *   F405 1MB   : BOOT 16KB @0x08000000 | APP 880KB @0x08004000 | META 128KB @0x080E0000
 *
 * 为什么配置区放在末尾而不是开头：F4 的扇区是「小而少 + 大而多」，
 * 末尾一定是整块大扇区；而开头几个小扇区拼在一起也凑不出一个干净的区域，
 * APP 会被割裂成两段。
 * ==========================================================================*/

/** Bootloader 区大小；16KB 对串口 IAP 足够，且只占一个扇区（S0） */
#ifndef BL_BOOT_SIZE
#define BL_BOOT_SIZE                (16UL * 1024UL)
#endif

/**
 * 配置区大小 —— 必须等于目标 Flash「最后一个扇区」的大小
 * 这样 BL_META_BASE 才自然落在扇区起点上，且 APP 区保持连续。
 */
#ifndef BL_META_SIZE
#define BL_META_SIZE                (128UL * 1024UL)
#endif

#define BL_BOOT_BASE                (BL_FLASH_BASE)
#define BL_APP_BASE                 (BL_BOOT_BASE + BL_BOOT_SIZE)
#define BL_META_BASE                (BL_FLASH_BASE + BL_FLASH_SIZE - BL_META_SIZE)
#define BL_APP_SIZE                 (BL_META_BASE - BL_APP_BASE)
#define BL_FLASH_END                (BL_FLASH_BASE + BL_FLASH_SIZE)

/** APP 向量表基址：APP 侧工程把它写进 SCB->VTOR（或 VECT_TAB_OFFSET） */
#define BL_APP_VTOR                 (BL_APP_BASE)

/* ---- 编译期自检：分区重叠 / 越界 / 未对齐 ---- */
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

/* ============================================================================
 * 二·补充、配置区槽位
 *
 * 配置区采用「日志式槽位轮转」：每次状态变更顺序追加一个新槽而不擦除，
 * 读的时候取序号最大的有效槽，写满整片后才擦一次。
 *
 * 为什么不用原地擦写：一次状态写入只有几十字节、耗时几十微秒；
 * 而擦一个 128KB 扇区要 1 秒左右并消耗一次寿命 —— 每次上电记录启动
 * 计数都这么干，既卡顿又短命。
 * ==========================================================================*/

/** 单个槽位字节数；必须是 Flash 编程单位的整数倍（F4 是 4 字节） */
#ifndef BL_META_SLOT_SIZE
#define BL_META_SLOT_SIZE           64UL
#endif

/** 槽位总数。128KB / 64B = 2048 个，够用很久 */
#define BL_META_SLOT_COUNT          (BL_META_SIZE / BL_META_SLOT_SIZE)

/* ============================================================================
 * 三、启动行为
 * ==========================================================================*/

/**
 * 「进入 Bootloader」的软件请求通道：APP 写 magic 到固定 RAM 地址后软复位。
 *
 * RAM 内容在软件复位（SYSRESETREQ）后保留，所以这个标志能穿透复位，
 * Bootloader 上电立刻就能看到「APP 请求过升级」。
 *
 * 相比上电 backdoor 时间窗，它的好处是：
 *   - Bootloader 上电**零等待**立即决策，APP 启动没有 300ms 拖累
 *   - APP 运行中也能随时唤回（不必抢上电那几百毫秒，正常人本来就卡不准）
 *   - APP 侧只需两行：写 magic、软件复位（见 bl_app.h 的 bl_request_update）
 *
 * 冷上电时 RAM 内容随机，故用 32 位 magic 压低误判概率，读到后立即清除。
 * APP 侧对应接口见 Bootloader/bl_app.h（同一个宏，两处必须一致）。
 */
#ifndef BL_UPDATE_REQ_ADDR
#define BL_UPDATE_REQ_ADDR          (BL_SRAM_END - 4UL)
#endif

#ifndef BL_UPDATE_REQ_MAGIC
#define BL_UPDATE_REQ_MAGIC         0xB007B007UL
#endif

/**
 * 上电后监听 backdoor 字符的时间窗（毫秒）；0 表示关闭
 *
 * 已被上面的 RAM 标志方案取代，默认关闭。保留此开关是为兼容：
 * 若某产品既不方便改 APP、又想保留「上电连按 DEL 进 IAP」的旧习惯，
 * 可手动打开。但代价是每次正常启动都要空等这段时间。
 */
#ifndef BL_BACKDOOR_WINDOW_MS
#define BL_BACKDOOR_WINDOW_MS       0UL
#endif

/** backdoor 触发字符（0x7F = DEL，串口工具里好按且不与文本冲突） */
#ifndef BL_BACKDOOR_CHAR
#define BL_BACKDOOR_CHAR            0x7FU
#endif

/**
 * 固件连续启动尝试次数上限，超过则判定「能过校验但跑不起来」并回滚
 *
 * 这是防变砖里最难防的一类：CRC 全对，一跑就 HardFault 或死机。
 *
 * 具体用什么作判据由 BL_BOOT_SELF_CONFIRM 决定。默认策略下只有
 * 「被看门狗拉回来」才算一次失败尝试，所以这个数字的实际含义是
 * 「最多容忍连续几次看门狗复位」。
 */
#ifndef BL_BOOT_MAX_ATTEMPTS
#define BL_BOOT_MAX_ATTEMPTS        3UL
#endif

/**
 * 固件「自确认」策略 —— Bootloader 如何判断新固件能不能跑起来
 *
 *   1 = 看门狗自确认（默认，推荐）
 *       Bootloader 读复位原因（port 层的 reset_cause()）：
 *         · 看门狗复位 → APP 没喂狗，判定它跑不起来 → 计数 +1，超限回滚
 *         · 上电 / 按复位 → APP 上次活下来了 → 自动转为 Valid
 *
 *       **APP 侧零侵入**：不必包含本库的任何头文件，不必知道配置区地址、
 *       槽位格式、CRC 算法，只需要做它本来就该做的事 —— 喂狗。
 *       这也把「IWDG 启动后无法关闭、APP 必须喂狗」这条硬件约束
 *       从负担变成了判据来源。
 *       前提：BL_USE_WATCHDOG = 1（否则没有判据可用）。
 *
 *   0 = 不做自确认
 *       跳转前只做向量表 + 整镜像 CRC 校验，不计数、不回滚。
 *       适合 APP 不便喂狗，或者项目里已由上位机负责「升级后人工确认」的场景。
 *       代价：挡不住「能过校验却一跑就崩」的固件 —— 此时靠 Backdoor 人工救回。
 *       （Backdoor 永远可用，所以仍然不会变砖，只是需要人动手。）
 *
 * 另外：无论选哪种策略，APP 都可以主动调 bl::meta().confirm_app() 提前
 * 声明自己健康。这是可选的，不调也不影响默认策略工作。
 */
#ifndef BL_BOOT_SELF_CONFIRM
#define BL_BOOT_SELF_CONFIRM        1
#endif

/** 启动时是否额外做整镜像 CRC32 校验（更稳，代价是每次上电多花点时间） */
#ifndef BL_BOOT_VERIFY_CRC32
#define BL_BOOT_VERIFY_CRC32        1
#endif

/** 是否启用独立看门狗（IWDG） */
#ifndef BL_USE_WATCHDOG
#define BL_USE_WATCHDOG             1
#endif

/**
 * 看门狗超时（毫秒）
 *
 * 取值受限于「最长的单次阻塞操作」—— 中间没机会喂狗的那一段：
 *   - 擦除一个 128KB 扇区：典型 1 秒，数据手册最坏情况 4 秒
 *   - YMODEM 收帧等待：3 秒（实现里已切成小片并喂狗，不再受此限）
 * 所以默认取 6 秒，留一倍余量。
 *
 * 想收紧这个值就往 target 的 flash 驱动里改：不要用 HAL 的整扇区擦除
 * （它是一口气阻塞到底的），改成轮询 FLASH_SR 并在循环里喂狗，
 * 那样 2 秒也能撑得住。
 */
#ifndef BL_WATCHDOG_TIMEOUT_MS
#define BL_WATCHDOG_TIMEOUT_MS      6000UL
#endif

/* ============================================================================
 * 四、通信参数
 * ==========================================================================*/

/** IAP 阶段串口波特率 */
#ifndef BL_UART_BAUDRATE
#define BL_UART_BAUDRATE            115200UL
#endif

/** YMODEM 单帧数据上限（1K 模式为 1024，兼容工具会自动选） */
#ifndef BL_YMODEM_BLOCK_SIZE
#define BL_YMODEM_BLOCK_SIZE        1024U
#endif

/** 单个数据帧接收超时（毫秒） */
#ifndef BL_YMODEM_PACKET_TIMEOUT_MS
#define BL_YMODEM_PACKET_TIMEOUT_MS 3000UL
#endif

/** 等 'C' 握手的最长时间（毫秒）；超时则放弃升级并尝试启动 APP */
#ifndef BL_YMODEM_HANDSHAKE_MS
#define BL_YMODEM_HANDSHAKE_MS      15000UL
#endif

/** 重传请求总次数上限，超过则中止本次升级 */
#ifndef BL_YMODEM_MAX_RETRY
#define BL_YMODEM_MAX_RETRY         10U
#endif

/** 单帧内允许的连续 NAK 次数 */
#ifndef BL_YMODEM_MAX_NAK
#define BL_YMODEM_MAX_NAK           5U
#endif

/* ============================================================================
 * 五、调试输出
 * ==========================================================================*/

/**
 * 是否通过串口输出调试信息
 *
 * 这是 ROM 占用最大的变量：实测链接标准 vsnprintf 会连带格式化与浮点
 * 支持代码一起吃掉约 6.5KB，而 Bootloader 只有 16KB 可用。
 * 因此不走 stdio，改用 core/bl_log 里的轻量格式化（约 2KB）。
 *
 * 允许用 -DBL_DEBUG_LOG=0 覆盖，便于量「发布版」体积。
 */
#ifndef BL_DEBUG_LOG
#define BL_DEBUG_LOG                1
#endif

/**
 * 调试输出宏
 *
 * 走 bl::log::printf，不依赖 stdio。
 * 使用本宏的 .cpp 需先 #include "core/bl_log.hpp"。
 */
#if BL_DEBUG_LOG
    #define BL_LOG(...)             do { ::bl::log::printf(__VA_ARGS__); } while (0)
#else
    #define BL_LOG(...)             do { } while (0)
#endif

/* ============================================================================
 * 六、集成方式
 * ==========================================================================*/

/**
 * 库是否自带 main()
 *
 *   1 = 自带（默认）。适合「Bootloader 是一份独立固件」的常规做法：
 *       Bootloader/ 直接就是程序，宿主工程里自带的 main.c 要排除掉。
 *   0 = 不自带。适合把库接进一个已经存在的工程：在你自己的 main() 里
 *       调用 bl::bl_entry() 即可，库不会和你的入口打架。
 *       bl::bl_entry() 内部做幂等初始化，宿主先初始化过也不会冲突。
 */
#ifndef BL_PROVIDE_MAIN
#define BL_PROVIDE_MAIN             1
#endif

#endif /* BL_CONFIG_H */
