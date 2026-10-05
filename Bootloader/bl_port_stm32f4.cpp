// STM32F4 的移植实现 —— 换芯片时照这份再写一个。
// 库只调 bl_port.h 声明的函数，一行都不会碰这里；芯片初始化由宿主工程负责。

#include "bl_port.h"
#include "stm32f4xx_hal.h"
#include <cstring>

// 板级配置：改这几行就能换板子。串口和按钮都由宿主工程初始化，这里只声明库用哪个。
//
// 宿主的串口句柄：CubeMX 生成的 usart.h 里 extern 声明、usart.c 里定义。
// 换了串口或改了句柄名，只改这一行。
extern "C" UART_HandleTypeDef huart1;
#define BL_UART_HANDLE          huart1

// 硬件按钮：按住它上电 → 留在 IAP，不启动 APP。板子上没有按钮就把下面四行注释掉。
// 宿主必须把这个脚配成「输入 + 上拉」（见 Core/Src/gpio.c 的 MX_GPIO_Init）。
#define BL_BOOT_PIN_PORT        GPIOC
#define BL_BOOT_PIN             GPIO_PIN_0              // PC0
#define BL_BOOT_PIN_PRESSED_LEVEL 0U                    // 按下时的电平：0 = 按下拉低
#define BL_BOOT_PIN_CLK_MASK    RCC_AHB1ENR_GPIOCEN     // 与上面端口对应，换端口要一起改

namespace bl {

// ② Flash 驱动

// 扇区表（契约要求 port 提供，见 bl_port.h）。地址一眼可查，不用算：
// S0-S3 各 16KB，S4 是 64KB，S5 起每个 128KB。
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

// 地址是否落在这片 Flash 内（擦写前的范围校验）
constexpr bool inFlash(uint32_t addr) noexcept
{
    return addr >= BL_FLASH_BASE && addr < (BL_FLASH_BASE + BL_FLASH_SIZE);
}

// 地址所在扇区在表里的下标（= HAL 要的扇区号）；不在表内返回 kFlashSectorCount
uint32_t sectorIndexAt(uint32_t addr) noexcept
{
    const FlashSector* s = flashSectorAt(addr);
    return (s != nullptr) ? static_cast<uint32_t>(s - kFlashSectors) : kFlashSectorCount;
}

} // namespace

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

    const uint32_t first = sectorIndexAt(addr);
    if (first >= kFlashSectorCount || kFlashSectors[first].base != addr) {
        BL_LOG("[flash] erase addr not sector-aligned: 0x%08lX\r\n",
               static_cast<unsigned long>(addr));
        return Status::BadParam;
    }

    /* 沿扇区边界走完整个区间，确认 len 正好由整数个扇区构成 */
    uint32_t remaining = len;
    uint32_t count     = 0;
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

// 串口收发：宿主已经把 USART 配好（8N1、波特率与上位机一致），库只负责搬字节。
// 这里用 HAL 而不是直接怼寄存器 —— port 层本来就是芯片相关的那一层，
// 而且固件里 HAL_UART 已经链接了（宿主 usart.c 要用），没有额外负担。
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
        // timeoutMs 是「总超时」。每次只等 1ms 再回来查总时间，
        // 免得单次阻塞把总超时拖过去。
        if ((tickMs() - start) >= timeoutMs) {
            break;
        }
        uint8_t ch = 0U;
        if (HAL_UART_Receive(&BL_UART_HANDLE, &ch, 1U, 1U) == HAL_OK) {
            buf[got++] = ch;
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
    if (len > 0xFFFFU) {                    // HAL 的 Size 是 uint16_t
        return Status::BadParam;
    }

    // 1 秒发送超时（日志行很短，正常几微秒发完）。
    // HAL_UART_Transmit 会等到最后一个字节移完（TC）才返回 —— 跳转 APP 前不丢日志。
    return (HAL_UART_Transmit(&BL_UART_HANDLE, buf, static_cast<uint16_t>(len),
                              1000U) == HAL_OK) ? Status::Ok : Status::Timeout;
}

void uartFlushRx() noexcept
{
    // 清 RXNE / ORE / NE / FE / PE —— F4 的清除序列就是「读 SR 再读 DR」，HAL 有这个宏
    __HAL_UART_CLEAR_PEFLAG(&BL_UART_HANDLE);

    // 把已经躺在数据寄存器里的残留字节丢掉
    for (uint32_t guard = 0U;
         (__HAL_UART_GET_FLAG(&BL_UART_HANDLE, UART_FLAG_RXNE) != RESET) && (guard < 4096U);
         ++guard) {
        (void)BL_UART_HANDLE.Instance->DR;
    }

    // 关掉「接收类」中断源：宿主若开了 USART1 的 NVIC，IAP 期间收字节会触发
    // 中断风暴打断传输。库是轮询收的，这些中断源不需要。
    __HAL_UART_DISABLE_IT(&BL_UART_HANDLE, UART_IT_RXNE);
    __HAL_UART_DISABLE_IT(&BL_UART_HANDLE, UART_IT_PE);
    __HAL_UART_DISABLE_IT(&BL_UART_HANDLE, UART_IT_ERR);
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

/* ---- 上电读硬件按钮 ---- */

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

    delayMs(5U);                        // 连读两次滤掉上电毛刺；未按下时零开销
    return bootPinLevel() == BL_BOOT_PIN_PRESSED_LEVEL;
#else
    return false;                       // 板子上没有按钮
#endif
}

} // namespace bl
