/**
 * @file    bl_port_stm32f4.cpp
 * @brief   STM32F4 的移植实现 —— 换芯片时照这份再写一个
 *
 * 这是「库与芯片之间唯一的接触面」。bl.cpp 只调用 bl_port.h 声明的函数，
 * 一行都不会碰这里的硬件细节。
 *
 * 换到别的芯片：
 *   1. 复制本文件为 bl_port_<平台>.cpp
 *   2. 改下面「板级配置」区（串口用哪一路、哪几个脚）
 *   3. 照 bl_port.h 的注释逐项实现那几个函数
 *   4. bl.cpp 一个字都不用改
 *
 * 本实现基于 ST HAL，需要标准 CubeMX 工程的 Drivers/ 与 CMSIS。
 * 你也可以改用寄存器直写，只要满足 bl_port.h 的语义。
 */

#include "bl_port.h"
#include "stm32f4xx_hal.h"
#include <cstring>

/* --------------------------------------------------------------------------
 * ★ 板级配置：改这几行就能换板子
 * -------------------------------------------------------------------------- */

/* ---- 调试 / IAP 串口 ---- */
#define BL_UART_INSTANCE        USART1
#define BL_UART_GPIO_PORT       GPIOA
#define BL_UART_TX_PIN          GPIO_PIN_9
#define BL_UART_RX_PIN          GPIO_PIN_10
#define BL_UART_GPIO_AF         GPIO_AF7_USART1

#define BL_UART_CLK_ENABLE()    __HAL_RCC_USART1_CLK_ENABLE()
#define BL_UART_GPIO_CLK_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()

/* 中断向量名（HardFault 直写寄存器时用不到，留给需要中断收发的场景） */
#define BL_UART_IRQn            USART1_IRQn

namespace bl {

/* --------------------------------------------------------------------------
 * 移植层内部声明（本文件内共享）
 * -------------------------------------------------------------------------- */

namespace stm32f4 {

/// 等发送移位寄存器空（跳转前调用，避免最后几行日志被打断）
void consoleTxFlush(uint32_t timeoutMs) noexcept;

} // namespace stm32f4

/* --------------------------------------------------------------------------
 * ① 系统底座：时钟树 / 时基 / 异常兜底
 * -------------------------------------------------------------------------- */

/* ============================================================================
 * 编译期自检：参数齐不齐、自不自洽
 * ==========================================================================*/
#if !defined(BL_HSE_HZ) || !defined(BL_PLL_M) || !defined(BL_PLL_N) || \
    !defined(BL_PLL_P) || !defined(BL_PLL_Q) || !defined(BL_AHB_DIV) || \
    !defined(BL_APB1_DIV) || !defined(BL_APB2_DIV) || !defined(BL_FLASH_LATENCY)
#error "target/stm32f4 需要时钟参数：请在 bl_config.h 第一节选用 presets 里的某个 STM32F4 预置，或手填 BL_HSE_HZ / BL_PLL_* / BL_AHB_DIV / BL_APB1_DIV / BL_APB2_DIV / BL_FLASH_LATENCY"
#endif

#if defined(HSE_VALUE) && (HSE_VALUE != BL_HSE_HZ)
#error "HSE_VALUE 与 bl_config.h 的 BL_HSE_HZ 不一致。HAL 会按 HSE_VALUE 反算系统频率，导致串口波特率全错（现象是乱码）。请在工程选项里加宏 HSE_VALUE=<BL_HSE_HZ>，或改掉 bl_config.h 里的 BL_HSE_HZ。"
#endif

namespace {

/* ============================================================================
 * 把 bl_config.h 里的「分频数字」翻译成 HAL 的枚举
 *
 * 用数字而不是直接写 HAL 枚举，是为了让 bl_config.h 不必包含任何平台头文件
 * —— 那份配置对 GD32/CH32 也照样能看懂。
 * ==========================================================================*/

static_assert(BL_PLL_P == 2 || BL_PLL_P == 4 || BL_PLL_P == 6 || BL_PLL_P == 8,
              "BL_PLL_P 只允许 2 / 4 / 6 / 8");

static_assert(BL_AHB_DIV == 1 || BL_AHB_DIV == 2 || BL_AHB_DIV == 4 ||
                  BL_AHB_DIV == 8 || BL_AHB_DIV == 16 || BL_AHB_DIV == 64 ||
                  BL_AHB_DIV == 128 || BL_AHB_DIV == 256 || BL_AHB_DIV == 512,
              "BL_AHB_DIV 取值非法");

static_assert(BL_APB1_DIV == 1 || BL_APB1_DIV == 2 || BL_APB1_DIV == 4 ||
                  BL_APB1_DIV == 8 || BL_APB1_DIV == 16,
              "BL_APB1_DIV 取值非法");

static_assert(BL_APB2_DIV == 1 || BL_APB2_DIV == 2 || BL_APB2_DIV == 4 ||
                  BL_APB2_DIV == 8 || BL_APB2_DIV == 16,
              "BL_APB2_DIV 取值非法");

constexpr uint32_t pllPEnum() noexcept
{
    return (BL_PLL_P == 2) ? RCC_PLLP_DIV2 :
           (BL_PLL_P == 4) ? RCC_PLLP_DIV4 :
           (BL_PLL_P == 6) ? RCC_PLLP_DIV6 : RCC_PLLP_DIV8;
}

constexpr uint32_t ahbDivEnum() noexcept
{
    return (BL_AHB_DIV == 1)   ? RCC_SYSCLK_DIV1   :
           (BL_AHB_DIV == 2)   ? RCC_SYSCLK_DIV2   :
           (BL_AHB_DIV == 4)   ? RCC_SYSCLK_DIV4   :
           (BL_AHB_DIV == 8)   ? RCC_SYSCLK_DIV8   :
           (BL_AHB_DIV == 16)  ? RCC_SYSCLK_DIV16  :
           (BL_AHB_DIV == 64)  ? RCC_SYSCLK_DIV64  :
           (BL_AHB_DIV == 128) ? RCC_SYSCLK_DIV128 :
           (BL_AHB_DIV == 256) ? RCC_SYSCLK_DIV256 : RCC_SYSCLK_DIV512;
}

constexpr uint32_t apb1DivEnum() noexcept
{
    return (BL_APB1_DIV == 1)  ? RCC_HCLK_DIV1  :
           (BL_APB1_DIV == 2)  ? RCC_HCLK_DIV2  :
           (BL_APB1_DIV == 4)  ? RCC_HCLK_DIV4  :
           (BL_APB1_DIV == 8)  ? RCC_HCLK_DIV8 : RCC_HCLK_DIV16;
}

constexpr uint32_t apb2DivEnum() noexcept
{
    return (BL_APB2_DIV == 1)  ? RCC_HCLK_DIV1  :
           (BL_APB2_DIV == 2)  ? RCC_HCLK_DIV2  :
           (BL_APB2_DIV == 4)  ? RCC_HCLK_DIV4  :
           (BL_APB2_DIV == 8)  ? RCC_HCLK_DIV8 : RCC_HCLK_DIV16;
}

/* ============================================================================
 * 时钟配置
 * ==========================================================================*/

/** 按 bl_config.h 建立 HSE + PLL 时钟；HSE 起振失败返回失败（由调用方兜底） */
Status clockFromHse() noexcept
{
    RCC_OscInitTypeDef osc{};
    RCC_ClkInitTypeDef clk{};

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = BL_PLL_M;
    osc.PLL.PLLN       = BL_PLL_N;
    osc.PLL.PLLP       = pllPEnum();
    osc.PLL.PLLQ       = BL_PLL_Q;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return Status::Error;
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = ahbDivEnum();
    clk.APB1CLKDivider = apb1DivEnum();
    clk.APB2CLKDivider = apb2DivEnum();

    if (HAL_RCC_ClockConfig(&clk, BL_FLASH_LATENCY) != HAL_OK) {
        return Status::Error;
    }
    return Status::Ok;
}

/**
 * 兜底：退回内部 HSI，不走 PLL
 *
 * 触发场景：板子没焊晶振、晶振虚焊、负载电容不匹配。
 * 宁可跑 16MHz 也要能通信 —— 一旦串口通不了，设备就成了砖，
 * 而 16MHz 下 115200 波特率照样工作（HAL 会按实际时钟算分频）。
 */
Status clockFallbackHsi() noexcept
{
    RCC_OscInitTypeDef osc{};
    RCC_ClkInitTypeDef clk{};

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_NONE;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return Status::Error;
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
        return Status::Error;
    }
    return Status::Ok;
}

} // namespace

/* ============================================================================
 * 对外：平台初始化
 * ==========================================================================*/
namespace {

/**
 * @brief 使能 FPU（CP10 / CP11 全访问）
 *
 * 复位后 CPACR = 0，含义是「禁止访问协处理器」。本工程按硬浮点编译
 * （-mfloat-abi=hard），只要执行到一条 VFP 指令，就会立刻触发
 * UsageFault(NOCP) 并被升级成 HardFault —— 现象是刚启动就卡死，
 * 而且从表面完全看不出跟浮点有关，非常难猜。
 *
 * ST 的 system_stm32f4xx.c 在 SystemInit 里做的第一件事就是它，
 * 这里保持相同的位置与语义：**任何可能用到浮点的代码之前**。
 */
void fpuEnable() noexcept
{
    SCB->CPACR |= ((3UL << 20) | (3UL << 22));   /* CP10, CP11 全访问 */
    __DSB();
    __ISB();
}

} // namespace

Status platformInit() noexcept
{
    /* 0. FPU 必须最先使能 —— 见 fpuEnable 的说明 */
    fpuEnable();

    /* 1. HAL 底座：中断优先级分组、1ms 时基、HAL_MspInit */
    if (HAL_Init() != HAL_OK) {
        return Status::Error;
    }

    /* 2. 系统时钟 */
    const bool hseOk = ok(clockFromHse());
    if (!hseOk) {
        if (!ok(clockFallbackHsi())) {
            return Status::Error;
        }
    }

    /* 3. 时基要按时钟重算
     *    HAL_Init 里是按复位后的 16MHz 算的，切换 PLL 后 1ms 就不准了。 */
    SystemCoreClockUpdate();
    (void)HAL_InitTick(TICK_INT_PRIORITY);

    if (!hseOk) {
        BL_LOG("[clk] HSE failed -> running on HSI 16MHz\r\n");
    }
    BL_LOG("[clk] sysclk=%lu Hz (core clock %lu Hz)\r\n",
           static_cast<unsigned long>(HAL_RCC_GetSysClockFreq()),
           static_cast<unsigned long>(SystemCoreClock));

    return Status::Ok;
}

/* ============================================================================
 * 中断与异常处理
 *
 * 这些原本由 CubeMX 生成的 Core/Src/stm32f4xx_it.c 与 stm32f4xx_hal_msp.c
 * 提供。因为库不依赖宿主工程的 Core/Src，所以自己实现一份最精简的：
 * 只保留 HAL 时基需要的 SysTick，以及 HAL 底座需要的 MspInit。
 * ==========================================================================*/

/** HAL 的 1ms 时基来源（tickMs / delayMs 都靠它） */
extern "C" void SysTick_Handler(void)
{
    HAL_IncTick();
}

/**
 * HAL 底层依赖：使能 SYSCFG 与 PWR 时钟
 *
 * 缺 PWR 时钟的典型症状：HAL_RCC_ClockConfig 里配置调压器失败，
 * 时钟切不过去，但函数未必报错 —— 很难查，所以这里必须开。
 */
extern "C" void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

/// 硬件异常兜底：直写串口汇报，然后死循环等调试器
extern "C" void HardFault_Handler(void)
{
    static const char kMsg[] = "\r\n!! HARDFAULT in LUMOS-bootloader !!\r\n";

    for (const char* p = kMsg; *p != '\0'; ++p) {
        while ((USART1->SR & USART_SR_TXE) == 0U) {
        }
        USART1->DR = static_cast<uint32_t>(static_cast<unsigned char>(*p));
    }

    for (;;) {
    }
}

/* --------------------------------------------------------------------------
 * ② Flash 驱动
 * -------------------------------------------------------------------------- */

namespace {

/* ============================================================================
 * F4 扇区布局规则
 *
 *   偏移 0     - 64KB  : S0..S3，每扇区 16KB
 *   偏移 64KB  - 128KB : S4，单扇区 64KB
 *   偏移 128KB - 末尾  : S5..，每扇区 128KB
 * ==========================================================================*/
constexpr uint32_t kSmallSize  = 16U * 1024U;
constexpr uint32_t kSmallCount = 4U;
constexpr uint32_t kMidSize    = 64U * 1024U;
constexpr uint32_t kLargeSize  = 128U * 1024U;

constexpr uint32_t kMidBase   = kSmallCount * kSmallSize;    /* 64KB  */
constexpr uint32_t kLargeBase = kMidBase + kMidSize;         /* 128KB */

/** 全片扇区个数 = 5 + (容量 - 128KB) / 128KB。512KB→8，1MB→12 */
constexpr uint32_t kSectorCount =
    5U + (BL_FLASH_SIZE - kLargeBase) / kLargeSize;

static_assert(BL_FLASH_SIZE >= (256U * 1024U),
              "BL_FLASH_SIZE 太小：F4 至少要 256KB 才放得下 16KB Bootloader + 配置区");

/** 地址相对 Flash 起始的偏移 */
constexpr uint32_t flashOff(uint32_t addr) noexcept
{
    return addr - BL_FLASH_BASE;
}

/* 下面几个都写成「单个 return 表达式」而不是先声明局部变量：
 * C++11 的 constexpr 函数体只允许一条 return 语句。 */

/** 地址是否落在本片 Flash 内 */
constexpr bool inFlash(uint32_t addr) noexcept
{
    return addr >= BL_FLASH_BASE && addr < (BL_FLASH_BASE + BL_FLASH_SIZE);
}

/** 地址所属扇区序号；越界时返回值 >= kSectorCount */
constexpr uint32_t sectorIndex(uint32_t addr) noexcept
{
    return (flashOff(addr) < kMidBase)   ? (flashOff(addr) / kSmallSize) :
           (flashOff(addr) < kLargeBase) ? kSmallCount :
           (kSmallCount + 1U + (flashOff(addr) - kLargeBase) / kLargeSize);
}

/** 扇区起始地址 */
constexpr uint32_t sectorBase(uint32_t idx) noexcept
{
    return (idx < kSmallCount)  ? (BL_FLASH_BASE + idx * kSmallSize) :
           (idx == kSmallCount) ? (BL_FLASH_BASE + kMidBase) :
           (BL_FLASH_BASE + kLargeBase + (idx - kSmallCount - 1U) * kLargeSize);
}

/** 扇区大小 */
constexpr uint32_t sectorSize(uint32_t idx) noexcept
{
    return (idx < kSmallCount)  ? kSmallSize :
           (idx == kSmallCount) ? kMidSize : kLargeSize;
}

} // namespace

/* ============================================================================
 * 初始化
 * ==========================================================================*/
Status flashInit() noexcept
{
    /* F4 的 Flash 接口时钟由 HAL_Init 打开，这里只需确保处于锁定态 */
    HAL_FLASH_Lock();
    return Status::Ok;
}

uint32_t flashSectorSize(uint32_t addr) noexcept
{
    if (!inFlash(addr)) {
        return 0U;
    }
    const uint32_t idx = sectorIndex(addr);
    return (idx < kSectorCount) ? sectorSize(idx) : 0U;
}

uint32_t flashBytesToSectorEnd(uint32_t addr) noexcept
{
    if (!inFlash(addr)) {
        return 0U;
    }
    const uint32_t idx = sectorIndex(addr);
    if (idx >= kSectorCount) {
        return 0U;
    }
    return (sectorBase(idx) + sectorSize(idx)) - addr;
}

/* ============================================================================
 * 擦除
 *
 * core 层已保证 addr 落在扇区起始、len 是若干扇区之和；这里仍做独立校验，
 * 并额外拒绝擦除 Bootloader 自身 —— 就算上层逻辑写出 bug，
 * 也不可能把「重刷入口」擦掉。
 * ==========================================================================*/
Status flashErase(uint32_t addr, uint32_t len) noexcept
{
    if (len == 0U) {
        return Status::Ok;
    }

    if (addr < (BL_BOOT_BASE + BL_BOOT_SIZE)) {
        BL_LOG("[flash] refuse to erase bootloader region\r\n");
        return Status::BadParam;
    }
    if (!inFlash(addr) || !inFlash(addr + len - 1U)) {
        BL_LOG("[flash] erase out of flash range: 0x%08lX +%lu\r\n",
               static_cast<unsigned long>(addr),
               static_cast<unsigned long>(len));
        return Status::BadParam;
    }

    const uint32_t first = sectorIndex(addr);
    if (first >= kSectorCount || addr != sectorBase(first)) {
        BL_LOG("[flash] erase addr not sector-aligned: 0x%08lX\r\n",
               static_cast<unsigned long>(addr));
        return Status::BadParam;
    }

    /* 沿扇区边界走完整个区间，确认 len 正好由整数个扇区构成 */
    uint32_t remaining = len;
    uint32_t count     = 0;
    uint32_t cur       = addr;
    while (remaining > 0U) {
        const uint32_t idx = sectorIndex(cur);
        if (idx >= kSectorCount || cur != sectorBase(idx)) {
            BL_LOG("[flash] erase len not sector-multiple\r\n");
            return Status::BadParam;
        }
        const uint32_t sz = sectorSize(idx);
        if (remaining < sz) {
            BL_LOG("[flash] erase len not sector-multiple\r\n");
            return Status::BadParam;
        }
        remaining -= sz;
        cur       += sz;
        ++count;
    }

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase{};
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;      /* 2.7V - 3.6V */
    erase.Sector       = first;
    erase.NbSectors    = count;

    uint32_t sectorError = 0;
    const HAL_StatusTypeDef rc = HAL_FLASHEx_Erase(&erase, &sectorError);

    HAL_FLASH_Lock();

    if (rc != HAL_OK || sectorError != 0xFFFFFFFFUL) {
        BL_LOG("[flash] erase error rc=%d sectorErr=0x%08lX\r\n",
               static_cast<int>(rc),
               static_cast<unsigned long>(sectorError));
        return Status::FlashFail;
    }
    return Status::Ok;
}

/* ============================================================================
 * 写入
 *
 * F4 编程单位是 32 位字。正常路径下 core 传入的地址与长度都是 4 的倍数
 * （YMODEM 数据区天然对齐、配置区槽为 64 字节），但这里仍处理尾巴不足
 * 一个字的情况：读出原字 → 合并 → 写回。
 * ==========================================================================*/
Status flashWrite(uint32_t addr, const void* data, uint32_t len) noexcept
{
    if (data == nullptr || len == 0U) {
        return Status::BadParam;
    }
    if ((addr & 0x3U) != 0U) {
        BL_LOG("[flash] write addr not word-aligned: 0x%08lX\r\n",
               static_cast<unsigned long>(addr));
        return Status::BadParam;
    }

    const auto*    src = static_cast<const uint8_t*>(data);
    const uint32_t end = addr + len;

    HAL_FLASH_Unlock();

    while (addr < end) {
        const uint32_t remain   = end - addr;
        uint32_t       word     = 0;
        uint32_t       consumed = 4U;

        if (remain >= 4U) {
            std::memcpy(&word, src, 4);
        } else {
            /* 尾巴不足一个字：读出当前内容后合并，未覆盖的字节保持原值 */
            uint32_t old = 0;
            std::memcpy(&old, reinterpret_cast<const void*>(addr), 4);

            uint8_t merged[4];
            std::memcpy(merged, &old, 4);
            std::memcpy(merged, src, remain);
            std::memcpy(&word, merged, 4);
            consumed = remain;
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK) {
            HAL_FLASH_Lock();
            BL_LOG("[flash] program error at 0x%08lX\r\n",
                   static_cast<unsigned long>(addr));
            return Status::FlashFail;
        }

        addr += 4U;
        src  += consumed;
    }

    HAL_FLASH_Lock();
    return Status::Ok;
}

/* ============================================================================
 * 读取（Flash 内存映射，直接拷贝）
 * ==========================================================================*/
Status flashRead(uint32_t addr, void* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }
    std::memcpy(buf, reinterpret_cast<const void*>(addr), len);
    return Status::Ok;
}

/* --------------------------------------------------------------------------
 * ③ UART 驱动
 * -------------------------------------------------------------------------- */

namespace {

/** 控制台串口句柄：由本文件独占持有 */
UART_HandleTypeDef gUart{};

/** 配置 TX/RX 引脚复用。放在 uartInit 里而不是 MspInit，
 *  是为了让整个适配层不依赖 HAL 的回调约定，调用路径更直白。 */
void gpioSetup() noexcept
{
    BL_UART_GPIO_CLK_ENABLE();
    BL_UART_CLK_ENABLE();

    GPIO_InitTypeDef gpio{};
    gpio.Pin       = BL_UART_TX_PIN | BL_UART_RX_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = BL_UART_GPIO_AF;
    HAL_GPIO_Init(BL_UART_GPIO_PORT, &gpio);
}

} // namespace

namespace stm32f4 {

void consoleTxFlush(uint32_t timeoutMs) noexcept
{
    const uint32_t start = HAL_GetTick();
    while (__HAL_UART_GET_FLAG(&gUart, UART_FLAG_TC) == RESET) {
        if ((HAL_GetTick() - start) >= timeoutMs) {
            break;
        }
    }
}

} // namespace stm32f4

/* ========================================================================
 * 初始化
 * ======================================================================*/
Status uartInit(uint32_t baudrate) noexcept
{
    if (baudrate == 0U) {
        return Status::BadParam;
    }

    gpioSetup();

    gUart.Instance          = BL_UART_INSTANCE;
    gUart.Init.BaudRate     = baudrate;
    gUart.Init.WordLength   = UART_WORDLENGTH_8B;
    gUart.Init.StopBits     = UART_STOPBITS_1;
    gUart.Init.Parity       = UART_PARITY_NONE;
    gUart.Init.Mode         = UART_MODE_TX_RX;
    gUart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    gUart.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&gUart) != HAL_OK) {
        return Status::Error;
    }

    uartFlushRx();
    return Status::Ok;
}

/* ---- 读取：逐字节收，带总超时 ---- */
Status uartRead(uint8_t* buf, uint32_t len,
                 uint32_t timeoutMs, uint32_t* outRead) noexcept
{
    uint32_t got = 0;

    if (outRead != nullptr) {
        *outRead = 0;
    }
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    const uint32_t start = HAL_GetTick();

    while (got < len) {
        uint32_t slice = timeoutMs;

        if (timeoutMs != 0U) {
            const uint32_t elapsed = HAL_GetTick() - start;
            if (elapsed >= timeoutMs) {
                break;                          /* 总超时 */
            }
            slice = timeoutMs - elapsed;
        } else {
            slice = 1U;                         /* 0 表示只试一次 */
        }

        uint8_t ch = 0;
        if (HAL_UART_Receive(&gUart, &ch, 1, slice) == HAL_OK) {
            buf[got++] = ch;
        } else if (timeoutMs == 0U) {
            break;
        } else if ((HAL_GetTick() - start) >= timeoutMs) {
            break;
        }
    }

    if (outRead != nullptr) {
        *outRead = got;
    }
    return (got == len) ? Status::Ok : Status::Timeout;
}

/// 非阻塞探测单字节：直接读 DR 才是真「不等待」
bool uartTryGetc(uint8_t* ch) noexcept
{
    if (ch == nullptr) {
        return false;
    }
    if (__HAL_UART_GET_FLAG(&gUart, UART_FLAG_RXNE) == RESET) {
        return false;
    }
    *ch = static_cast<uint8_t>(gUart.Instance->DR & 0xFFU);
    return true;
}

/* ========================================================================
 * 写入
 * ======================================================================*/
Status uartWrite(const uint8_t* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    const uint32_t slice = 1000U + (len / 10U);      /* 按长度给足余量 */

    if (HAL_UART_Transmit(&gUart, const_cast<uint8_t*>(buf), len, slice) != HAL_OK) {
        return Status::Timeout;
    }
    return Status::Ok;
}

/* ========================================================================
 * 清空接收缓冲
 * ======================================================================*/
void uartFlushRx() noexcept
{
    /* 先清错误标志，否则后续接收会一直被阻塞 */
    __HAL_UART_CLEAR_OREFLAG(&gUart);
    __HAL_UART_CLEAR_FEFLAG(&gUart);
    __HAL_UART_CLEAR_NEFLAG(&gUart);
    __HAL_UART_CLEAR_PEFLAG(&gUart);

    uint32_t guard = 0;
    while (__HAL_UART_GET_FLAG(&gUart, UART_FLAG_RXNE) != RESET && guard++ < 4096U) {
        (void)gUart.Instance->DR;
    }
}

/* --------------------------------------------------------------------------
 * ④ 跳转 APP / 读复位原因
 * -------------------------------------------------------------------------- */

/* ========================================================================
 * 跳转到 APP
 *
 * 八个步骤缺一不可，顺序也不能乱。漏掉哪一步的典型症状：
 *   - 不停 SysTick       → APP 里 HAL_Delay 走时不对
 *   - 不复位 RCC         → APP 以为时钟还是 Bootloader 配的，串口波特率全错
 *   - 不清 NVIC 挂起标志 → APP 一开中断就冲进某个已挂起的中断服务函数
 *   - 不设 VTOR          → APP 的中断跳到 Bootloader 的向量表里
 *   - 不设 MSP           → 栈指针还停在 Bootloader 的栈上，一压栈就踩坏数据
 *   - 不清 CONTROL       → 若此前用过 PSP，APP 会在错误的栈上运行
 * ======================================================================*/
void jumpToApp(uint32_t appBase) noexcept
{
    stm32f4::consoleTxFlush(100U);   /* 等最后几行日志发完再跳 */

    /* 取向量表前两字：初始栈顶与复位入口 */
    const uint32_t initialSp = *reinterpret_cast<volatile uint32_t*>(appBase);
    const uint32_t resetVec  = *reinterpret_cast<volatile uint32_t*>(appBase + 4U);

    BL_LOG("[jump] sp=0x%08lX entry=0x%08lX\r\n",
           static_cast<unsigned long>(initialSp),
           static_cast<unsigned long>(resetVec));

    /* 1. 关全局中断 */
    __disable_irq();

    /* 2. 复位 RCC 到默认态（HSI）。
     *    APP 的 SystemInit 会按自己的配置重建 PLL。
     *
     * ⚠️ 顺序陷阱（实测踩过，很隐蔽）：
     *    HAL_RCC_DeInit() 内部末尾会调用 HAL_InitTick()，
     *    也就是**重新把 SysTick 配成 1ms 并使能它的中断**。
     *    因此「关 SysTick」必须放在它**之后**，放到前面会被它悄悄重新打开。
     *    后果是 APP 一跑起来就不断被 SysTick 中断打断，而 APP 的向量表里
     *    SysTick_Handler 通常是空的（Default_Handler = 一条 B .），
     *    于是直接卡死在异常处理里 —— 现象是「APP 完全不输出任何字符」，
     *    光看串口根本无法定位。必须用调试器读 ICSR 才能看到
     *    VECTACTIVE = 15。 */
    HAL_RCC_DeInit();

    /* 3. 关 SysTick 并清掉可能已经挂起的请求 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* SysTick / PendSV 是系统异常，不在 NVIC 里，必须单独清挂起位，
     * 否则 APP 一开中断就会立刻冲进这两个 handler。 */
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* 4. 清所有 NVIC 中断使能与挂起标志 */
    for (uint32_t i = 0; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* 5. 重定位向量表到 APP，并保证它对后续取指立即生效 */
    SCB->VTOR = appBase;
    __DSB();

    /* 6. 设置主堆栈指针 */
    __set_MSP(initialSp);

    /* 7. 回到特权级 + 使用 MSP（若之前用过 PSP） */
    __set_CONTROL(0U);
    __ISB();

    /* 8. 开中断并跳转 */
    __enable_irq();

    using AppEntry = void (*)(void);
    auto entry = reinterpret_cast<AppEntry>(resetVec);
    entry();

    /* 正常情况下不会执行到这里 */
    for (;;) {
    }
}

/* ---- 时基 ---- */

uint32_t tickMs() noexcept
{
    return HAL_GetTick();
}

void delayMs(uint32_t ms) noexcept
{
    HAL_Delay(ms);
}

/* ---- 复位原因 ---- */

ResetCause resetCause() noexcept
{
    // 复位标志是累积的，读后即清；SFTRSTF 优先于 POR/PIN：
    // 探针连着时软件复位会连带拉 NRST，PINRSTF 同时置位
    const uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;

    if ((csr & RCC_CSR_SFTRSTF)  != 0U) { return ResetCause::Software; }
    if ((csr & RCC_CSR_PORRSTF)  != 0U) { return ResetCause::PowerOn;  }
    if ((csr & RCC_CSR_PINRSTF)  != 0U) { return ResetCause::Pin;      }
    if ((csr & RCC_CSR_BORRSTF)  != 0U) { return ResetCause::BrownOut; }
    if ((csr & RCC_CSR_LPWRRSTF) != 0U) { return ResetCause::LowPower; }

    return ResetCause::Unknown;
}

} // namespace bl
