/**
 * @file    prog_blob.c
 * @brief   在目标芯片 RAM 里运行的 Flash 编程小程序（绕过调试器的总线限制）
 *
 * 背景：某些调试探针（CMSIS-DAP 实现不完整，例如自制的 DAPLink 固件）
 *       在目标 Flash 编程期间无法处理 AHB 总线拉停 —— 会返回非法 ACK
 *       （0b110），而标准的 CMSIS-Pack 算法又因本地原因返回错误码。
 *       现象很明确：擦除正常、读正常、RAM 读写正常，唯独"编程"失败。
 *
 * 对策：把"编程"这件事整个搬到目标侧执行。宿主只做三件事：
 *       1. 把本程序与镜像数据写进 RAM（RAM 访问是可靠的）
 *       2. 设置寄存器并放开核心运行
 *       3. 轮询 RAM 里的状态字（不碰 Flash，因此不会触发总线拉停）
 *
 * 注意：本文件不是 Bootloader 的一部分，只是烧写工具链的一环，
 *       不参与 Bootloader 的固件编译。
 */
#include <stdint.h>

/* ---- STM32F4 FLASH 寄存器（RM0090 §3.9） ---- */
typedef struct {
    volatile uint32_t ACR;      /* 0x00 访问控制 */
    volatile uint32_t KEYR;     /* 0x04 解锁键 */
    volatile uint32_t OPTKEYR;  /* 0x08 */
    volatile uint32_t SR;       /* 0x0C 状态 */
    volatile uint32_t CR;       /* 0x10 控制 */
    volatile uint32_t OPTCR;    /* 0x14 选项 */
} flash_regs_t;

#define FLASH               ((flash_regs_t *)0x40023C00UL)

#define SR_BSY              (1u << 16)
/* 编程相关错误位：OPERR(1) WRPERR(4) PGAERR(5) PGPERR(6) PGSERR(7) */
#define SR_ERR_MASK         0x000000F2u

#define CR_PG               0x00000001u
#define CR_PSIZE_X32        (2u << 8)       /* 2.7-3.6V 用 32 位并行度 */
#define CR_LOCK             (1u << 31)

#define ACR_LATENCY_MASK    0x0000000Fu

/* 状态字的编码 */
#define ST_RUNNING          0xE1u
#define ST_OK               0x00u
#define ST_ERR_BUSY_STUCK   0xE2u
#define ST_ERR_PROGRAM      0xE3u
#define ST_ERR_TIMEOUT      0xE4u
#define ST_ERR_UNLOCK       0xE5u

/**
 * @brief 把 RAM 中的镜像写入 Flash
 *
 * @param src    源数据（目标 RAM 中，4 字节对齐）
 * @param dst    目标 Flash 地址（必须是擦除过的区域）
 * @param len    字节数（必须是 4 的倍数）
 * @param status 状态字指针，四个字：
 *               [0]=状态码 [1]=已写入字数 [2]=失败时的 FLASH_SR [3]=失败时的 FLASH_CR
 * @return 恒为 0，调用方看 status
 */
__attribute__((noinline, used, section(".text.bl_prog")))
uint32_t bl_prog(uint32_t *src, uint32_t dst, uint32_t len,
                 volatile uint32_t *status)
{
    uint32_t i;
    const uint32_t words = len >> 2;
    uint32_t guard;

    status[0] = ST_RUNNING;
    status[1] = 0u;
    status[2] = 0u;
    status[3] = 0u;

    /* 关掉预取与 I/D 缓存：部分 F4 器件在缓存开启时编程会失败，
     * ST 的 HAL 在写 Flash 前也会这么做。只留等待周期。 */
    FLASH->ACR &= ACR_LATENCY_MASK;

    /* 解锁 */
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
    if ((FLASH->CR & CR_LOCK) != 0u) {          /* 还锁着说明解锁没成功 */
        status[0] = ST_ERR_UNLOCK;
        status[2] = FLASH->SR;
        status[3] = FLASH->CR;
        return 0u;
    }

    /* 等上一次操作收尾，清所有标志（写 1 清除） */
    guard = 0;
    while ((FLASH->SR & SR_BSY) != 0u) {
        if (++guard > 0x00400000u) {
            status[0] = ST_ERR_BUSY_STUCK;
            status[2] = FLASH->SR;
            goto done;
        }
    }
    FLASH->SR = 0xFFFFFFFFu;

    /* PSIZE = x32，进入编程模式 */
    FLASH->CR = CR_PSIZE_X32 | CR_PG;

    /* 上一字写完后 Flash 控制器会拉停总线直到编程结束，
     * 这里仍显式轮询 BSY，把"什么时候可以写下一字"说清楚。 */
    for (i = 0; i < words; ++i) {
        const uint32_t value = src[i];

        guard = 0;
        while ((FLASH->SR & SR_BSY) != 0u) {
            if (++guard > 0x00400000u) {
                status[0] = ST_ERR_BUSY_STUCK;
                status[2] = FLASH->SR;
                goto done;
            }
        }

        *((volatile uint32_t *)(dst + (i << 2))) = value;

        guard = 0;
        while ((FLASH->SR & SR_BSY) != 0u) {
            if (++guard > 0x00400000u) {
                status[0] = ST_ERR_TIMEOUT;
                status[2] = FLASH->SR;
                goto done;
            }
        }

        if ((FLASH->SR & SR_ERR_MASK) != 0u) {
            status[0] = ST_ERR_PROGRAM;
            status[1] = i;
            status[2] = FLASH->SR;
            status[3] = FLASH->CR;
            goto done;
        }
        status[1] = i + 1u;
    }

    status[0] = ST_OK;

done:
    FLASH->CR = CR_PSIZE_X32;        /* 退出编程模式（清 PG） */
    FLASH->CR |= CR_LOCK;            /* 重新上锁 */
    return 0u;
}
