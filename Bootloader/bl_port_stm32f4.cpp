// STM32F4 的移植实现 —— 换芯片时照这份再写一个。
// 库只调 bl_port.h 声明的函数，一行都不会碰这里；芯片初始化由宿主工程负责。

#include "bl_port.h"
#include "stm32f4xx_hal.h"
#include <cstring>

// 板级配置：改这几行就能换板子。
// 串口本身由宿主工程初始化（8N1、波特率与上位机一致），这里只声明库用哪一路。
#define BL_UART_INSTANCE        USART1
#define BL_UART_GPIO_PORT       GPIOA
#define BL_UART_TX_PIN          GPIO_PIN_9
#define BL_UART_RX_PIN          GPIO_PIN_10
#define BL_UART_GPIO_AF         GPIO_AF7_USART1

#define BL_UART_CLK_ENABLE()    __HAL_RCC_USART1_CLK_ENABLE()
#define BL_UART_GPIO_CLK_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()

namespace bl {

// ② Flash 驱动

namespace {

// F4 扇区布局规则（扇区大小不等）
//
// 偏移 0     - 64KB  : S0..S3，每扇区 16KB
// 偏移 64KB  - 128KB : S4，单扇区 64KB
// 偏移 128KB - 末尾  : S5..，每扇区 128KB
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

// 擦除
//
// core 层已保证 addr 落在扇区起始、len 是若干扇区之和；这里仍做独立校验，
// 并额外拒绝擦除 Bootloader 自身 —— 就算上层逻辑写出 bug，
// 也不可能把「重刷入口」擦掉。
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

// 写入
//
// F4 编程单位是 32 位字。正常路径下 core 传入的地址与长度都是 4 的倍数
// （YMODEM 数据区天然对齐、提交时写 8 字节），但这里仍处理尾巴不足
// 一个字的情况：读出原字 → 合并 → 写回。
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

// 读取（Flash 内存映射，直接拷贝）
Status flashRead(uint32_t addr, void* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }
    std::memcpy(buf, reinterpret_cast<const void*>(addr), len);
    return Status::Ok;
}

// 串口收发：直接用寄存器轮询。
// 不依赖 HAL_UART，也不持有任何句柄 —— 宿主初始化好之后，库只负责搬字节。
namespace {
constexpr uint32_t kLoopGuard = 200000U;    // 等标志位的兜底上限，防止硬件异常时死等
}

Status uartRead(uint8_t* buf, uint32_t len, uint32_t timeoutMs, uint32_t* outRead) noexcept
{
    uint32_t got = 0;

    if (outRead != nullptr) {
        *outRead = 0;
    }
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    const uint32_t start = tickMs();

    while (got < len) {
        if ((BL_UART_INSTANCE->SR & USART_SR_RXNE) != 0U) {
            buf[got++] = static_cast<uint8_t>(BL_UART_INSTANCE->DR & 0xFFU);
            continue;
        }
        if (timeoutMs == 0U) {                      // 0 = 只试一次
            break;
        }
        if ((tickMs() - start) >= timeoutMs) {      // timeoutMs 是「总超时」
            break;
        }
    }

    if (outRead != nullptr) {
        *outRead = got;
    }
    return (got == len) ? Status::Ok : Status::Timeout;
}

Status uartWrite(const uint8_t* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    for (uint32_t i = 0; i < len; ++i) {
        uint32_t guard = 0;
        while ((BL_UART_INSTANCE->SR & USART_SR_TXE) == 0U) {
            if (++guard > kLoopGuard) {
                return Status::Timeout;
            }
        }
        BL_UART_INSTANCE->DR = buf[i];
    }

    // 等最后一个字节移完再返回：跳转 APP 前不丢日志
    uint32_t guard = 0;
    while ((BL_UART_INSTANCE->SR & USART_SR_TC) == 0U) {
        if (++guard > kLoopGuard) {
            break;
        }
    }
    return Status::Ok;
}

void uartFlushRx() noexcept
{
    // 先读 SR 再读 DR，清掉 RXNE/ORE/NE/FE/PE（F4 的清除序列）
    volatile uint32_t scratch = BL_UART_INSTANCE->SR;
    scratch = BL_UART_INSTANCE->DR;
    (void)scratch;

    uint32_t guard = 0;
    while ((BL_UART_INSTANCE->SR & USART_SR_RXNE) != 0U && guard++ < 4096U) {
        (void)BL_UART_INSTANCE->DR;
    }

    // 关掉「接收类」中断源：宿主若开了 USART1 的 NVIC，IAP 期间的收字节
    // 会触发中断风暴打断传输。库是轮询收的，这些中断源不需要。
    BL_UART_INSTANCE->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_PEIE);
    BL_UART_INSTANCE->CR3 &= ~USART_CR3_EIE;
}

// 跳转到 APP
//
// 八个步骤缺一不可，顺序也不能乱。漏掉哪一步的典型症状：
// - 不停 SysTick       → APP 里 HAL_Delay 走时不对
// - 不复位 RCC         → APP 以为时钟还是 Bootloader 配的，串口波特率全错
// - 不清 NVIC 挂起标志 → APP 一开中断就冲进某个已挂起的中断服务函数
// - 不设 VTOR          → APP 的中断跳到 Bootloader 的向量表里
// - 不设 MSP           → 栈指针还停在 Bootloader 的栈上，一压栈就踩坏数据
// - 不清 CONTROL       → 若此前用过 PSP，APP 会在错误的栈上运行
void jumpToApp(uint32_t appBase) noexcept
{
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

    return ResetCause::Unknown;
}

} // namespace bl
