/**
 * @file    app_main.c
 * @brief   LUMOS-bootloader 的测试 APP
 *
 * 用途：验证「Bootloader 收到固件 -> 写入 APP 区 -> 校验 -> 跳转」这条链路。
 *
 * 设计原则：完全不依赖 Bootloader 的代码，也不依赖 HAL/CubeMX。
 *   - 编译期链接到 0x08004000（BL_APP_BASE）
 *   - 运行期第一件事就是把向量表搬到 0x08004000（SCB->VTOR）
 *   - 串口用寄存器直接驱动，避免为测试引入一堆依赖
 *
 * 这样它就是一个「真实的 APP」：即使没有 Bootloader、直接烧到
 * 0x08004000，它也能独立跑起来。能跑通才说明跳转是真的成功。
 */

#include <stdint.h>

/* ========================================================================
 * 硬件定义
 * ======================================================================*/
#define REG32(a)            (*(volatile uint32_t *)(a))

/* APP 区基址 —— 必须与 bl_config.h 的 BL_APP_BASE 一致 */
#define APP_BASE            0x08004000UL

/* ========================================================================
 * 故障注入开关（仅用于验证 Bootloader 的回滚机制）
 *
 *   0 = 正常
 *   1 = 故意不喂狗：IWDG 超时后把 CPU 拉回 Bootloader。用来验证
 *       「看门狗自确认」判据，以及连续失败后的自动回滚
 *   2 = 一启动就触发 HardFault：验证崩溃路径
 *
 * 用 build_app.py 的 --fail N 编译出故障固件（app_fail.bin）。
 * ======================================================================*/
#ifndef APP_FAIL_MODE
#define APP_FAIL_MODE       0
#endif

/* 启动阶段标记：写到 RAM 高位（远离栈与 ZI 区），
 * 用调试器读回来就能知道 APP 执行到了哪一步。
 * 串口不通时这是最可靠的诊断手段。 */
#define APP_MARK(n, v)      (*(volatile uint32_t *)(0x20010000UL + 4U * (n)) = (v))
#define MK_SYSINIT          0xA1U
#define MK_FPU              0xA2U
#define MK_VTOR             0xA3U
#define MK_CLOCK            0xA4U
#define MK_MAIN             0xA5U
#define MK_UART             0xA6U
#define MK_IWDG             0xA7U
#define MK_BANNER           0xA8U

/* RCC */
#define RCC_BASE            0x40023800UL
#define RCC_CR              REG32(RCC_BASE + 0x00U)
#define RCC_PLLCFGR         REG32(RCC_BASE + 0x04U)
#define RCC_CFGR            REG32(RCC_BASE + 0x08U)
#define RCC_AHB1ENR         REG32(RCC_BASE + 0x30U)
#define RCC_APB2ENR         REG32(RCC_BASE + 0x44U)

/* FLASH */
#define FLASH_ACR           REG32(0x40023C00UL)

/* GPIOA（USART1 在 PA9/PA10，AF7） */
#define GPIOA_BASE          0x40020000UL
#define GPIOA_MODER         REG32(GPIOA_BASE + 0x00U)
#define GPIOA_OSPEEDR       REG32(GPIOA_BASE + 0x08U)
#define GPIOA_PUPDR         REG32(GPIOA_BASE + 0x0CU)
#define GPIOA_AFRH          REG32(GPIOA_BASE + 0x24U)

/* USART1 */
#define USART1_BASE         0x40011000UL
#define USART1_SR           REG32(USART1_BASE + 0x00U)
#define USART1_DR           REG32(USART1_BASE + 0x04U)
#define USART1_BRR          REG32(USART1_BASE + 0x08U)
#define USART1_CR1          REG32(USART1_BASE + 0x0CU)

/* SCB */
#define SCB_VTOR            REG32(0xE000ED08UL)
#define SCB_CPACR           REG32(0xE000ED88UL)
#define SCB_ICSR            REG32(0xE000ED04UL)

/* SysTick（本 APP 不用它做时基，只用于清掉 Bootloader 的遗留状态） */
#define SYST_CSR            REG32(0xE000E010UL)
#define SYST_RVR            REG32(0xE000E014UL)
#define SYST_CVR            REG32(0xE000E018UL)

/* IWDG（APP 自己管理看门狗，不依赖 Bootloader 是否启动过） */
#define IWDG_BASE           0x40003000UL
#define IWDG_KR             REG32(IWDG_BASE + 0x00U)
#define IWDG_PR             REG32(IWDG_BASE + 0x04U)
#define IWDG_RLR            REG32(IWDG_BASE + 0x08U)
#define IWDG_SR             REG32(IWDG_BASE + 0x0CU)

/* 时钟参数：HSE 25MHz -> PLL -> 84MHz（与 Bootloader 保持一致） */
#define HSE_VALUE           25000000UL
#define PLL_M               25UL
#define PLL_N               336UL
#define PLL_P               4UL      /* 2 的幂编号：PLLP=4 表示 /4 */
#define PLL_Q               7UL

#define UART_BAUDRATE       115200UL

/* ========================================================================
 * 系统级杂项
 * ======================================================================*/

/**
 * @brief 使能 FPU（CP10 / CP11 全访问）
 *
 * 复位后 CPACR 是 0，也就是「禁止访问协处理器」。
 * 用硬浮点（-mfloat-abi=hard）编译出来的代码只要碰到一条 VFP 指令，
 * 就会立刻触发 UsageFault(NOCP) 并被升级成 HardFault ——
 * 现象是 APP 刚跑起来就卡死在 HardFault_Handler 里，非常难猜。
 *
 * 所以这必须放在最初的时机（在 SystemInit 的第一行），
 * 任何可能用到浮点的代码之前。
 */
static void fpu_enable(void)
{
    SCB_CPACR |= (0xFUL << 20);         /* CP10 = CP11 = 全访问 */
    __asm volatile ("dsb");
    __asm volatile ("isb");
}

/**
 * @brief 配置并启动独立看门狗
 *
 * IWDG 一旦启动就无法停止（只能靠复位），所以 APP 必须持续喂狗。
 * 这里不去猜测 Bootloader 是否已经开过它 —— 无论开没开，
 * 按同样的参数重新配一遍都是安全的（写 KR=0xCCCC 对已启动的 IWDG 无副作用）。
 *
 * 超时按 6 秒设置，与 Bootloader 保持一致：
 *   LSI ≈ 32kHz，64 分频 → 500Hz，重载 3000 → 约 6 秒
 */
static void iwdg_init(void)
{
    /* 顺序很关键，必须与 ST 的 HAL_IWDG_Init 一致：
     *   1) 先写 0xCCCC **启动** IWDG —— 硬件会顺带把 LSI 振荡器打开
     *   2) 等 PVU/RVU 落（LSI 已经在跑，这两位才有可能被硬件清掉）
     *   3) 写 0x5555 允许改写 PR/RLR，再写参数
     *   4) 喂一次狗
     *
     * 反例（踩过的坑）：先写 0x5555 + PR/RLR 而不先启动 IWDG，
     *   LSI 没有时钟去驱动 IWDG 逻辑，SR 的 PVU/RVU 会**一直保持 1**，
     *   任何“等它清零”的循环都会死锁 —— 现象是 APP 刚进去就再也不出声。
     */
    IWDG_KR = 0xCCCCU;                              /* 启动（LSI 随之使能） */

    for (uint32_t i = 0; i < 0x100000U; ++i) {      /* 等 LSI 起振、SR 清零 */
        if (IWDG_SR == 0U) { break; }
    }

    IWDG_KR  = 0x5555U;                             /* 允许改写 PR / RLR */
    IWDG_PR  = 4U;                                  /* 64 分频 */
    IWDG_RLR = 3000U;                               /* 3000 → 约 6 秒 */

    for (uint32_t i = 0; i < 0x100000U; ++i) {      /* 等参数写入生效 */
        if ((IWDG_SR & 0x3U) == 0U) { break; }
    }

    IWDG_KR = 0xAAAAU;                              /* 喂一次 */
}

/* 喂狗 */
#define iwdg_feed()         do { IWDG_KR = 0xAAAAU; } while (0)

/* ========================================================================
 * 时钟
 *
 * 放在 SystemInit 里调用 —— 此刻 C 运行时还没初始化（RW/ZI 尚未建立），
 * 所以这里只用寄存器和局部变量，不碰任何全局对象。
 *
 * ⚠️ 这里必须考虑「由 Bootloader 热跳转过来」的场景：
 *    Bootloader 跳转前会调 HAL_RCC_DeInit()，把系统切回 HSI 并关闭 PLL。
 *    但只要 PLL 还没彻底停下来（PLLRDY 仍为 1），写 RCC_PLLCFGR 就会被
 *    硬件**静默忽略** —— 于是 PLL 用复位默认值（16/16*304/2 = 152MHz）
 *    跑起来，远超 F401 的 84MHz 上限，一开就飞。
 *    所以顺序必须是：先切走系统时钟 → 再确认 PLL 关闭 → 最后才写 PLLCFGR。
 * ======================================================================*/

/* 等寄存器某几位变成期望值；带超时，避免硬件异常时死循环 */
static int wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    for (uint32_t i = 0; i < 0x200000U; ++i) {
        if ((*reg & mask) == want) {
            return 1;
        }
    }
    return 0;
}

static void clock_init(void)
{
    /* ---- 0. 回到干净起点：系统时钟切 HSI、PLL 关停 ----
     * 对冷启动而言这几步本来就已满足，等于空操作；
     * 对热跳转而言它们是正确性的前提。 */
    RCC_CFGR &= ~3UL;                                   /* SW = HSI */
    (void)wait_bits(&RCC_CFGR, 0xCUL, 0x0UL);           /* 等 SWS = HSI */
    RCC_CR   &= ~(1UL << 24);                           /* PLLON = 0 */
    (void)wait_bits(&RCC_CR, 1UL << 25, 0UL);           /* 等 PLLRDY = 0 */

    /* ---- 1. 配 Flash 等待周期 ----
     * 先按 84MHz 的上限给足 2WS，并打开预取与 I/D cache。
     * 在只有 16MHz 的阶段这几个 wait state 是多余的，但绝对安全。 */
    FLASH_ACR = (FLASH_ACR & ~0xFUL) | 2UL;
    FLASH_ACR |= (1UL << 8) | (1UL << 9) | (1UL << 10);

    /* ---- 2. 总线分频：AHB=1, APB1=2, APB2=1 ---- */
    RCC_CFGR &= ~((0xFUL << 4) | (7UL << 10) | (7UL << 13));
    RCC_CFGR |= (0x4UL << 10);                          /* PPRE1 = /2 → 42MHz */

    /* ---- 3. 打开 HSE 并等它稳定 ----
     * 热跳转时 Bootloader 刚把 HSE 关掉，重新起振需要 1-2ms，
     * 因此等待循环要留足余量（这里最多约 2M 次迭代）。 */
    RCC_CR |= (1UL << 16);                              /* HSEON */
    const int hse_ok = wait_bits(&RCC_CR, 1UL << 17, 1UL << 17);

    if (!hse_ok) {
        /* HSE 起不来就老老实实留在 HSI 16MHz。
         * ⚠️ 此时绝不能去碰 PLL：复位默认的 PLLCFGR 会让 PLL 输出 152MHz。 */
        return;
    }

    /* ---- 4. 配置 PLL：25MHz / 25 * 336 / 4 = 84MHz ----
     * 能走到这里说明 PLL 已确认关闭（第 0 步等过 PLLRDY），写入一定生效。 */
    RCC_PLLCFGR = PLL_M
                | (PLL_N << 6)
                | (((PLL_P >> 1) - 1UL) << 16)
                | (PLL_Q << 24)
                | (1UL << 22);                          /* PLLSRC = HSE */

    /* ---- 5. 使能 PLL 并等锁定 ---- */
    RCC_CR |= (1UL << 24);                              /* PLLON */
    if (!wait_bits(&RCC_CR, 1UL << 25, 1UL << 25)) {    /* 等 PLLRDY */
        RCC_CR &= ~(1UL << 24);                         /* 锁不上就放弃 PLL */
        return;
    }

    /* ---- 6. 切到 PLL 作为系统时钟 ---- */
    RCC_CFGR = (RCC_CFGR & ~3UL) | 2UL;
    (void)wait_bits(&RCC_CFGR, 0xCUL, 0x8UL);           /* 等 SWS = PLL */
}

/* ========================================================================
 * 串口（寄存器直接驱动）
 * ======================================================================*/

/**
 * @brief 从 RCC 寄存器推算当前 PCLK2
 *
 * 直接读寄存器而不是用全局变量：SystemInit 阶段 C 运行时的 ZI 还没建立，
 * 往全局变量里存东西是不可靠的。而且这样也能自然适应
 * 「HSE 没起来、退回 HSI 16MHz」的降级情况。
 */
static uint32_t get_pclk2(void)
{
    const uint32_t sws = (RCC_CFGR >> 2) & 3UL;
    uint32_t sysclk;

    if (sws == 2UL) {                                   /* 系统时钟 = PLL */
        const uint32_t pllm = RCC_PLLCFGR & 0x3FUL;
        const uint32_t plln = (RCC_PLLCFGR >> 6) & 0x1FFUL;
        const uint32_t pllp = (((RCC_PLLCFGR >> 16) & 3UL) + 1UL) * 2UL;
        const uint32_t src  = ((RCC_PLLCFGR >> 22) & 1UL) ? HSE_VALUE : 16000000UL;
        sysclk = (pllm != 0UL) ? (src / pllm * plln / pllp) : 16000000UL;
    } else if (sws == 1UL) {                            /* 系统时钟 = HSE */
        sysclk = HSE_VALUE;
    } else {                                            /* HSI */
        sysclk = 16000000UL;
    }

    /* APB2 分频档（PPRE2 = bits[15:13]）：<4 表示不分频 */
    const uint32_t ppre2 = (RCC_CFGR >> 13) & 7UL;
    return (ppre2 < 4UL) ? sysclk : (sysclk >> (ppre2 - 3UL));
}

static void uart_init(void)
{
    /* 时钟 */
    RCC_AHB1ENR |= (1UL << 0);                      /* GPIOAEN */
    RCC_APB2ENR |= (1UL << 4);                      /* USART1EN */
    (void)RCC_AHB1ENR;
    (void)RCC_APB2ENR;

    /* PA9 / PA10 -> AF7（USART1） */
    GPIOA_MODER   = (GPIOA_MODER & ~((3UL << 18) | (3UL << 20)))
                  |  (2UL << 18) | (2UL << 20);
    GPIOA_OSPEEDR |= ((3UL << 18) | (3UL << 20));
    GPIOA_PUPDR    = (GPIOA_PUPDR & ~((3UL << 18) | (3UL << 20)))
                   |  (1UL << 18);                  /* PA9 上拉 */
    GPIOA_AFRH    = (GPIOA_AFRH & ~((0xFUL << 4) | (0xFUL << 8)))
                  |  (7UL << 4) | (7UL << 8);

    /* 波特率：16 倍过采样，BRR = fPCLK2 / baud。
     * 按实际时钟推导，HSE 降级到 HSI 时也能给出正确波特率。 */
    const uint32_t pclk2 = get_pclk2();
    USART1_BRR = (pclk2 + (UART_BAUDRATE / 2UL)) / UART_BAUDRATE;

    USART1_CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2);   /* UE | TE | RE */
}

static void uart_putc(char c)
{
    while (((USART1_SR >> 7) & 1UL) == 0UL) {       /* 等 TXE */
    }
    USART1_DR = (uint32_t)(unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static void uart_put_u32(uint32_t v)
{
    char tmp[11];
    uint32_t n = 0;
    if (v == 0U) {
        uart_putc('0');
        return;
    }
    while (v != 0U) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0U) {
        uart_putc(tmp[--n]);
    }
}

static void uart_put_hex(uint32_t v)
{
    static const char *digits = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 7; i >= 0; --i) {
        uart_putc(digits[(v >> (i * 4)) & 0xFU]);
    }
}

static void delay_ms(uint32_t ms)
{
    /* 84MHz 下粗略估算：每 ms 约 84000 次空转，取经验值 */
    for (volatile uint32_t i = 0; i < ms * 8400U; ++i) {
    }
}

/* ========================================================================
 * 启动
 *
 * SystemInit 由 startup 在进入 __main 之前调用。
 * 这里必须完成「向量表重定位」—— 否则任何中断都会打到 Bootloader 的
 * 向量表上去（Bootloader 的向量表在 0x08000000）。
 * ======================================================================*/
void SystemInit(void)
{
    APP_MARK(0, MK_SYSINIT);

    /* 防御性清理：把 Bootloader 可能遗留下来的内核级状态抹掉。
     * 正常情况下 Bootloader 在跳转前已经清过，但多这一道成本极低，
     * 而且能避免「APP 一进来就被 SysTick 中断打断、卡死在空 handler 里」
     * 这类极难定位的问题（SysTick 属于系统异常，不在 NVIC 里）。 */
    SYST_CSR = 0U;                          /* 关 SysTick */
    SYST_RVR = 0U;
    SYST_CVR = 0U;
    SCB_ICSR = (1UL << 25);                 /* PENDSTCLR：清 SysTick 挂起 */

    fpu_enable();           /* 必须最先：硬浮点代码的前提条件 */
    APP_MARK(1, MK_FPU);
    SCB_VTOR = APP_BASE;
    APP_MARK(2, MK_VTOR);
    clock_init();
    APP_MARK(3, MK_CLOCK);
    APP_MARK(4, get_pclk2() / 1000U);   /* 记录实际 PCLK2(kHz) */
}

static uint32_t g_tick = 0;

int main(void)
{
    APP_MARK(5, MK_MAIN);
    uart_init();
    APP_MARK(6, MK_UART);
    iwdg_init();
    APP_MARK(7, MK_IWDG);

    APP_MARK(8, MK_BANNER);
    uart_puts("\r\n");
    uart_puts("===== LUMOS APP (test) =====\r\n");
    uart_puts("[app] running at  : ");
    uart_put_hex((uint32_t)(uintptr_t)&main);
    uart_puts("\r\n");
    uart_puts("[app] SCB->VTOR   : ");
    uart_put_hex(SCB_VTOR);
    uart_puts("\r\n");
    uart_puts("[app] PCLK2      : ");
    uart_put_u32(get_pclk2() / 1000U);
    uart_puts(" kHz\r\n");
    uart_puts("[app] CPACR      : ");
    uart_put_hex(SCB_CPACR);
    uart_puts("\r\n");
    uart_puts("[app] Vectors[0..3]: ");
    uart_put_hex(REG32(APP_BASE + 0x00U));
    uart_putc(' ');
    uart_put_hex(REG32(APP_BASE + 0x04U));
    uart_putc(' ');
    uart_put_hex(REG32(APP_BASE + 0x08U));
    uart_putc(' ');
    uart_put_hex(REG32(APP_BASE + 0x0CU));
    uart_puts("\r\n");
    uart_puts("[app] UART 115200 8N1 ready\r\n");
    uart_puts("----------------------------\r\n");

#if APP_FAIL_MODE == 1
    /* 【故障注入 1】故意不喂狗。
     * 主循环里没有 iwdg_feed()，所以 IWDG（6 秒）超时后会把 CPU 复位，
     * Bootloader 由此看到 IWDGRSTF，判定这个固件跑不起来并逐步累加计数。
     * 用于验证「看门狗自确认」判据与自动回滚。 */
    uart_puts("[app] FAIL-MODE=1: watchdog will NOT be fed\r\n");
    for (;;) {
        /* 什么都不做，等看门狗出手 */
    }
#elif APP_FAIL_MODE == 2
    /* 【故障注入 2】一启动就踩非法地址，触发 HardFault。
     * 此时 CPU 会停在自己的 HardFault_Handler 里，同样不再喂狗，
     * 最终仍由看门狗把控制权交回 Bootloader。 */
    uart_puts("[app] FAIL-MODE=2: deliberate HardFault\r\n");
    delay_ms(20U);
    *(volatile uint32_t *)0xFFFFFFF0UL = 0xDEADBEEFUL;
    for (;;) {
    }
#endif

    /* 主循环：每秒报一次存活，期间切成 100ms 小片逐片喂狗。
     * IWDG 超时 6 秒，但喂狗间隔做小些更稳（也便于将来加长任务）。 */
    for (;;) {
        uart_puts("[app] alive, tick=");
        uart_put_u32(g_tick++);
        uart_puts("\r\n");

        for (uint32_t i = 0; i < 10U; ++i) {
            iwdg_feed();
            delay_ms(100U);
        }
    }
}
