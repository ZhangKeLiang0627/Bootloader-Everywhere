/**
 * @file    bl_port.h
 * @brief   Bootloader 移植接口（唯一契约层）
 *
 * 移植到新芯片时，实现本文件声明的全部函数即可，
 * core/ 与 app/ 目录下的代码不需要任何修改。
 *
 * 参考实现：target/stm32f4/
 * 移植模板：port/bl_port_template.c
 */
#ifndef BL_PORT_H
#define BL_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Flash 驱动
 *
 * 说明：core 层只按「字节地址 + 字节长度」请求操作，
 *       对齐、粒度补齐、解锁等芯片特性全部由移植层内部处理。
 * ======================================================================*/

/** 初始化 Flash 访问（解锁、使能时钟等） */
bl_status_t bl_port_flash_init(void);

/**
 * @brief 查询包含指定地址的扇区（页）大小
 * @param addr 目标地址
 * @return 该扇区字节数；0 表示地址非法
 */
uint32_t bl_port_flash_sector_size(uint32_t addr);

/**
 * @brief 查询从 addr 起、到下一个扇区边界为止的连续字节数
 *
 * 用于按扇区边界安全地分批擦除，避免跨扇区误擦。
 */
uint32_t bl_port_flash_bytes_to_sector_end(uint32_t addr);

/**
 * @brief 擦除区域（core 层保证 addr 与 len 都按扇区对齐）
 * @param addr 起始地址
 * @param len  字节数
 */
bl_status_t bl_port_flash_erase(uint32_t addr, uint32_t len);

/**
 * @brief 编程写入
 *
 * 移植层需处理：地址对齐、长度补齐（补 0xFF）、编程粒度。
 * 若 addr/len 不满足硬件对齐要求，由移植层内部读写-合并-回写完成。
 */
bl_status_t bl_port_flash_write(uint32_t addr, const void *data, uint32_t len);

/**
 * @brief 从 Flash 读取
 */
bl_status_t bl_port_flash_read(uint32_t addr, void *buf, uint32_t len);

/* ========================================================================
 * UART 驱动
 * ======================================================================*/

/** 初始化串口 */
bl_status_t bl_port_uart_init(uint32_t baudrate);

/**
 * @brief 运行时修改串口波特率
 *
 * 当前 IAP 阶段固定使用 BL_UART_BAUDRATE（115200），本接口为预留能力：
 * 将来若实现「握手阶段 115200、数据传输阶段切到 921600」的提速方案，
 * 由会话层在双方约定时机调用本接口。
 *
 * 实现要求：切换过程中不得丢失已接收数据；切换后需清空收发 FIFO。
 * 注意 PC 端必须同步切换，否则链路立即失步。
 */
bl_status_t bl_port_uart_set_baudrate(uint32_t baudrate);

/**
 * @brief 读取数据（阻塞至读满或超时）
 * @param buf        接收缓冲
 * @param len        期望读取字节数
 * @param timeout_ms 超时毫秒；0 表示不等待
 * @param out_read   实际读到的字节数（可为 NULL）
 * @return BL_OK 读满；BL_ERR_TIMEOUT 超时；其他为错误
 */
bl_status_t bl_port_uart_read(uint8_t *buf, uint32_t len,
                              uint32_t timeout_ms, uint32_t *out_read);

/**
 * @brief 向接收方向探测单个字节（不等待）
 * @param ch 读到的字节
 * @return true 读到；false 无数据
 */
bool bl_port_uart_try_getc(uint8_t *ch);

/** 阻塞式写入全部数据 */
bl_status_t bl_port_uart_write(const uint8_t *buf, uint32_t len);

/** 清空接收缓冲（进入 IAP 前调用，丢弃 APP 遗留数据） */
void bl_port_uart_flush_rx(void);

/** 调试格式化输出（仅在 BL_DEBUG_LOG 打开时被调用） */
void bl_port_uart_printf(const char *fmt, ...);

/* ========================================================================
 * 系统控制
 * ======================================================================*/

/**
 * @brief 从 Bootloader 跳转到 APP
 *
 * 实现须包含 Cortex-M 的标准跳转前准备：
 *   1. 关全局中断
 *   2. 停 SysTick 并清计数
 *   3. 关闭各外设时钟 / 复位 RCC 到默认态（HAL_RCC_DeInit）
 *   4. 清所有 NVIC 中断使能与挂起标志
 *   5. 重定位向量表 SCB->VTOR = app_base
 *   6. __set_MSP(*(uint32_t*)app_base)
 *   7. __set_CONTROL(0)（若 APP 使用 MSP 特权级）
 *   8. 取 *(uint32_t*)(app_base + 4) 作为入口并调用
 *
 * 本函数正常情况下不返回；返回即表示跳转失败。
 */
void bl_port_jump_to_app(uint32_t app_base);

/** 喂看门狗 */
void bl_port_wdg_feed(void);

/** 初始化看门狗 */
void bl_port_wdg_init(void);

/** 获取系统毫秒时基（上电起累计） */
uint32_t bl_port_get_tick_ms(void);

/** 触发系统复位（用于升级完成后重启） */
void bl_port_system_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_PORT_H */
