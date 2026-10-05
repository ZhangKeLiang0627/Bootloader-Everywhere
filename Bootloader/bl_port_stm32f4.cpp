// STM32F4 的移植实现 —— 换芯片时照这份再写一个。
// 库只调 bl_port.h 声明的函数，一行都不会碰这里；外设初始化由宿主工程负责。

#include "bl_port.h"
#include "stm32f4xx_hal.h"

#include <cstring>

// ---- 板级配置：改这几行就能换板子 ----

// 库要用的那一路串口（宿主已完成初始化：8N1、波特率与上位机一致）
#define BL_UART_INSTANCE        USART1

// 硬件按钮：按住它上电 → 留在 IAP，不启动 APP。板子上没有按钮就把下面四行注释掉。
// 引脚本身由宿主配成「输入 + 上拉」（见 Core/Src/gpio.c 的 MX_GPIO_Init）。
#define BL_BOOT_PIN_PORT        GPIOC
#define BL_BOOT_PIN             GPIO_PIN_0              // PC0
#define BL_BOOT_PIN_PRESSED_LEVEL 0U                    // 0 = 按下拉低
#define BL_BOOT_PIN_CLK_MASK    RCC_AHB1ENR_GPIOCEN     // 与上面端口对应，换端口要一起改

namespace bl {

// ------------------------------------------------------------------ Flash

// 扇区表：S0-S3 各 16KB，S4 是 64KB，S5 起每个 128KB。
// F401 512KB 到 S7；F405/F407 1MB 到 S11（按 BL_FLASH_SIZE 自动截断）。
extern const FlashSector kFlashSectors[] = {
    { 0x08000000U,  16U * 1024U },   // S0
    { 0x08004000U,  16U * 1024U },   // S1
    { 0x08008000U,  16U * 1024U },   // S2
    { 0x0800C000U,  16U * 1024U },   // S3
    { 0x08010000U,  64U * 1024U },   // S4  ← 只有它是 64KB
    { 0x08020000U, 128U * 1024U },   // S5
    { 0x08040000U, 128U * 1024U },   // S6
    { 0x08060000U, 128U * 1024U },   // S7  ← F401 512KB 到此为止
#if BL_FLASH_SIZE > 512U * 1024U
    { 0x08080000U, 128U * 1024U },   // S8
    { 0x080A0000U, 128U * 1024U },   // S9
    { 0x080C0000U, 128U * 1024U },   // S10
    { 0x080E0000U, 128U * 1024U },   // S11
#endif
};

extern const uint32_t kFlashSectorCount =
    sizeof(kFlashSectors) / sizeof(kFlashSectors[0]);

namespace {

constexpr bool inFlash(uint32_t addr) noexcept
{
    return addr >= BL_FLASH_BASE && addr < (BL_FLASH_BASE + BL_FLASH_SIZE);
}

// 扇区号（= HAL 要的下标）；不在表内返回 kFlashSectorCount
uint32_t sectorIndexAt(uint32_t addr) noexcept
{
    const FlashSector* s = flashSectorAt(addr);
    return (s != nullptr) ? static_cast<uint32_t>(s - kFlashSectors) : kFlashSectorCount;
}

} // namespace

Status flashErase(uint32_t addr, uint32_t len) noexcept
{
    if (len == 0U) {
        return Status::Ok;
    }

    // 独立的范围校验 + 拒绝擦除 Bootloader 自身：就算上层逻辑写出 bug，
    // 也不可能把「重刷入口」擦掉。
    if (addr < (BL_BOOT_BASE + BL_BOOT_SIZE)) {
        BL_LOG("[flash] refuse to erase bootloader region\r\n");
        return Status::BadParam;
    }
    if (!inFlash(addr) || !inFlash(addr + len - 1U)) {
        BL_LOG("[flash] erase out of range: 0x%08lX +%lu\r\n",
               static_cast<unsigned long>(addr), static_cast<unsigned long>(len));
        return Status::BadParam;
    }

    // 沿扇区边界走一遍，确认 addr 对齐且 len 正好由整数个扇区构成
    const uint32_t first = sectorIndexAt(addr);
    if (first >= kFlashSectorCount || kFlashSectors[first].base != addr) {
        BL_LOG("[flash] erase addr not sector-aligned: 0x%08lX\r\n",
               static_cast<unsigned long>(addr));
        return Status::BadParam;
    }

    uint32_t remaining = len;
    uint32_t count     = 0U;
    uint32_t cur       = addr;
    while (remaining > 0U) {
        const uint32_t idx = sectorIndexAt(cur);
        if (idx >= kFlashSectorCount || kFlashSectors[idx].base != cur) {
            BL_LOG("[flash] erase len not sector-multiple\r\n");
            return Status::BadParam;
        }
        const uint32_t sz = kFlashSectors[idx].size;
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
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;      // 2.7V - 3.6V
    erase.Sector       = first;
    erase.NbSectors    = count;

    uint32_t sectorError = 0U;
    const HAL_StatusTypeDef rc = HAL_FLASHEx_Erase(&erase, &sectorError);

    HAL_FLASH_Lock();

    if (rc != HAL_OK || sectorError != 0xFFFFFFFFUL) {
        BL_LOG("[flash] erase error rc=%d sectorErr=0x%08lX\r\n",
               static_cast<int>(rc), static_cast<unsigned long>(sectorError));
        return Status::FlashFail;
    }
    return Status::Ok;
}

// F4 按 32 位字编程。正常路径下地址与长度都是 4 的倍数（数据流天然对齐、提交写 8 字节），
// 这里仍处理尾巴不足一个字的情况：读出原字 → 合并 → 写回。
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
        uint32_t       word     = 0U;
        uint32_t       consumed = 4U;

        if (remain >= 4U) {
            std::memcpy(&word, src, 4U);
        } else {
            uint32_t old = 0U;
            std::memcpy(&old, reinterpret_cast<const void*>(addr), 4U);

            uint8_t merged[4];
            std::memcpy(merged, &old, 4U);
            std::memcpy(merged, src, remain);
            std::memcpy(&word, merged, 4U);
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

// Flash 是内存映射的，直接拷贝
Status flashRead(uint32_t addr, void* buf, uint32_t len) noexcept
{
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }
    std::memcpy(buf, reinterpret_cast<const void*>(addr), len);
    return Status::Ok;
}

// ------------------------------------------------------------------ 串口

// 走中断接收。写 Flash 期间 CPU 会被总线停摆（RM0090：Flash 读写时取指不能进行），
// 轮询收必然丢字节；中断 + 环形缓冲把「搬运」放在总线之外完成。
// 这里只碰寄存器，不用宿主的 UART 句柄 —— 库是客人，初始化是宿主的事。
namespace {

constexpr uint32_t kRxRingSize  = 512U;     // 停等模型，缓冲不必大
constexpr uint32_t kTxTimeoutMs = 1000U;

uint8_t           gRxRing[kRxRingSize];
volatile uint32_t gRxHead = 0U;             // ISR 写入
volatile uint32_t gRxTail = 0U;             // 主循环读出

inline void rxPush(uint8_t b) noexcept
{
    const uint32_t next = (gRxHead + 1U) % kRxRingSize;
    if (next == gRxTail) {
        return;                             // 满则丢新字节：该帧 CRC 不过，靠重传补
    }
    gRxRing[gRxHead] = b;
    gRxHead = next;
}

inline bool rxPop(uint8_t& b) noexcept
{
    if (gRxHead == gRxTail) {
        return false;
    }
    b = gRxRing[gRxTail];
    gRxTail = (gRxTail + 1U) % kRxRingSize;
    return true;
}

} // namespace

// 只认 RXNE，不开错误中断：写 Flash 期间 ORE 必然出现，开错误中断会中断风暴。
// 写 Flash 期间到达的字节允许丢，靠重传补回来。
void uartRxIrqHandler() noexcept
{
    if ((BL_UART_INSTANCE->SR & USART_SR_RXNE) == 0U) {
        return;
    }
    const uint8_t b = static_cast<uint8_t>(BL_UART_INSTANCE->DR & 0xFFU);
    rxPush(b);
}

Status uartRead(uint8_t* buf, uint32_t len, uint32_t timeoutMs, uint32_t* outRead) noexcept
{
    uint32_t got = 0U;

    if (outRead != nullptr) {
        *outRead = 0U;
    }
    if (buf == nullptr || len == 0U) {
        return Status::BadParam;
    }

    const uint32_t start = tickMs();

    while (got < len) {
        while (got < len && rxPop(buf[got])) {
            ++got;
        }
        if (got >= len) {
            break;
        }
        if ((tickMs() - start) >= timeoutMs) {
            break;
        }
        __WFI();                    // 睡到下一个中断（收到字节 或 SysTick）
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

    const uint32_t start = tickMs();

    for (uint32_t i = 0U; i < len; ++i) {
        while ((BL_UART_INSTANCE->SR & USART_SR_TXE) == 0U) {
            if ((tickMs() - start) >= kTxTimeoutMs) {
                return Status::Timeout;
            }
        }
        BL_UART_INSTANCE->DR = buf[i];
    }

    // 等最后一个字节移完（TC）再返回：跳转 APP 前不丢日志与应答
    while ((BL_UART_INSTANCE->SR & USART_SR_TC) == 0U) {
        if ((tickMs() - start) >= kTxTimeoutMs) {
            return Status::Timeout;
        }
    }
    return Status::Ok;
}

void uartFlushRx() noexcept
{
    BL_UART_INSTANCE->CR1 &= ~USART_CR1_RXNEIE;     // 先关中断，避免与清空竞争

    // F4 清 RXNE/ORE/NE/FE/PE 的序列就是「读 SR 再读 DR」
    (void)BL_UART_INSTANCE->SR;
    (void)BL_UART_INSTANCE->DR;

    gRxHead = 0U;
    gRxTail = 0U;

    // 库不初始化外设，但「怎么用」是自己的事：接收中断源由库打开
    BL_UART_INSTANCE->CR1 |= USART_CR1_RXNEIE;
}

// ------------------------------------------------------------------ 时基

uint32_t tickMs() noexcept
{
    return HAL_GetTick();
}

void delayMs(uint32_t ms) noexcept
{
    HAL_Delay(ms);
}

// ------------------------------------------------------------------ 复位

ResetCause resetCause() noexcept
{
    // 读后即清（标志是累积的）；SFTRSTF 优先于 POR/PIN：
    // 探针连着时软件复位会连带拉 NRST，PINRSTF 同时置位。
    const uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;

    if ((csr & RCC_CSR_SFTRSTF) != 0U) { return ResetCause::Software; }
    if ((csr & RCC_CSR_PORRSTF) != 0U) { return ResetCause::PowerOn;  }
    if ((csr & RCC_CSR_PINRSTF) != 0U) { return ResetCause::Pin;      }

    return ResetCause::Unknown;
}

// ------------------------------------------------------------------ 按钮

namespace {

#ifdef BL_BOOT_PIN_PORT
uint32_t bootPinLevel() noexcept
{
    return (BL_BOOT_PIN_PORT->IDR & BL_BOOT_PIN) ? 1U : 0U;
}
#endif

} // namespace

bool bootPinHeld() noexcept
{
#ifdef BL_BOOT_PIN_PORT
    // 时钟没开时读 IDR 恒为 0（本板实测），会被误判成「按住」→ 每次上电都进 IAP。
    // 所以此时按「没按」处理：按键失效只是少一条通道，误判会让 APP 永远起不来。
    if ((RCC->AHB1ENR & BL_BOOT_PIN_CLK_MASK) == 0U) {
        return false;
    }
    if (bootPinLevel() != BL_BOOT_PIN_PRESSED_LEVEL) {
        return false;
    }

    delayMs(5U);                    // 连读两次滤掉上电毛刺；未按下时零开销
    return bootPinLevel() == BL_BOOT_PIN_PRESSED_LEVEL;
#else
    return false;                   // 板子上没有按钮
#endif
}

// ------------------------------------------------------------------ 跳转

// 八步缺一不可，顺序也不能乱。两个踩过的坑：
//   · HAL_RCC_DeInit() 末尾会重新打开 SysTick，所以「关 SysTick」必须放在它之后
//   · SysTick / PendSV 不在 NVIC 里，挂起位要单独清
void jumpToApp(uint32_t appBase) noexcept
{
    const uint32_t initialSp = *reinterpret_cast<volatile uint32_t*>(appBase);
    const uint32_t resetVec  = *reinterpret_cast<volatile uint32_t*>(appBase + 4U);

    BL_LOG("[jump] sp=0x%08lX entry=0x%08lX\r\n",
           static_cast<unsigned long>(initialSp), static_cast<unsigned long>(resetVec));

    __disable_irq();                                    // 1. 关全局中断

    HAL_RCC_DeInit();                                   // 2. 复位 RCC（APP 的 SystemInit 会重建）

    SysTick->CTRL = 0U;                                 // 3. 关 SysTick（必须在 RCC_DeInit 之后）
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    for (uint32_t i = 0U; i < 8U; ++i) {                 // 4. 清所有 NVIC 使能与挂起
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    SCB->VTOR = appBase;                                // 5. 向量表指向 APP
    __DSB();

    __set_MSP(initialSp);                               // 6. 设主堆栈指针
    __set_CONTROL(0U);                                  // 7. 回特权级 + 用 MSP
    __ISB();

    __enable_irq();                                     // 8. 跳转

    using AppEntry = void (*)(void);
    auto entry = reinterpret_cast<AppEntry>(resetVec);
    entry();

    for (;;) {                                          // 正常不会走到这里
    }
}

} // namespace bl
