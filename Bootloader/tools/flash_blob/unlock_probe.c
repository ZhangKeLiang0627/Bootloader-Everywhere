/**
 * @file    unlock_probe.c
 * @brief   在目标 RAM 里跑的解锁排查小程序
 *
 * 只做一件事：用几种不同的顺序尝试解锁 FLASH，把每一步之后的 CR/SR/OPTCR
 * 记录下来，供宿主读回分析。
 *
 * 背景：标准 CMSIS-Pack 算法能擦除（说明它解锁成功了），但把同样的 key
 *       序列放进自己的小程序却解不开。要么是我漏了某个前置步骤，要么
 *       这颗料处于某种受保护状态。用数据说话。
 *
 * 结果写入 status[0..9]：
 *   [0] 结果码（0 = 成功解锁）
 *   [1] CR  (解锁前)
 *   [2] SR  (解锁前)
 *   [3] CR  (A: 直接连写两个 key 之后)
 *   [4] CR  (B: 先清 SR 再写 key 之后)
 *   [5] CR  (C: 再写一遍 key 之后)
 *   [6] CR  (D: 写 OPTKEYR 两个 key 之后)
 *   [7] OPTCR
 *   [8] ACR
 *   [9] 第二次读 CR（确认读数稳定）
 */
#include <stdint.h>

typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t OPTCR;
} flash_regs_t;

#define FLASH ((flash_regs_t *)0x40023C00UL)

#define CR_LOCK (1u << 31)

static inline void unlock_keys(void)
{
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
}

__attribute__((noinline, used, section(".text.bl_prog")))
uint32_t bl_probe(volatile uint32_t *status)
{
    uint32_t k;

    for (k = 0; k < 10u; ++k) {
        status[k] = 0u;
    }

    status[1] = FLASH->CR;              /* 解锁前 */
    status[2] = FLASH->SR;
    status[8] = FLASH->ACR;
    status[7] = FLASH->OPTCR;

    /* A：直接连写两个 key */
    unlock_keys();
    status[3] = FLASH->CR;
    if ((status[3] & CR_LOCK) == 0u) { status[0] = 0u; goto out; }

    /* B：先清干净 SR 标志再写 key */
    FLASH->SR = 0xFFFFFFFFu;
    unlock_keys();
    status[4] = FLASH->CR;
    if ((status[4] & CR_LOCK) == 0u) { status[0] = 4u; goto out; }

    /* C：再写一遍（有些实现要求 key 紧邻、中间不得有任何 FLASH 访问） */
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
    status[5] = FLASH->CR;
    if ((status[5] & CR_LOCK) == 0u) { status[0] = 5u; goto out; }

    /* D：走选项字节解锁通道，看通道本身是否可用 */
    FLASH->OPTKEYR = 0x08192A3Bu;
    FLASH->OPTKEYR = 0x4C5D6E7Fu;
    status[6] = FLASH->CR;
    if ((status[6] & CR_LOCK) == 0u) { status[0] = 6u; goto out; }

    status[0] = 0xFFu;                  /* 全部失败 */
    status[9] = FLASH->CR;              /* 再读一次，确认不是偶然 */

out:
    return 0u;
}
