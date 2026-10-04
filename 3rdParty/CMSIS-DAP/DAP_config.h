#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__


#define CPU_CLOCK               84000000	 	///< Specifies the CPU Clock in Hz


#define IO_PORT_WRITE_CYCLES    2               ///< I/O Cycles: 2=default, 1=Cortex-M0+ fast I/0


#define DAP_SWD                 1               ///< SWD Mode:  1 = available, 0 = not available

#define DAP_JTAG                0               ///< JTAG Mode: 0 = not available

#define DAP_JTAG_DEV_CNT        0               ///< Maximum number of JTAG devices on scan chain

#define DAP_DEFAULT_PORT        1               ///< Default JTAG/SWJ Port Mode: 1 = SWD, 2 = JTAG.

#define DAP_DEFAULT_SWJ_CLOCK   4000000         ///< Default SWD/JTAG clock frequency in Hz.


/// Maximum Package Size for Command and Response data.
#define DAP_PACKET_SIZE         64              ///< USB: 64 = Full-Speed, 1024 = High-Speed.

/// Maximum Package Buffers for Command and Response data.
#define DAP_PACKET_COUNT        1              ///< Buffers: 64 = Full-Speed, 4 = High-Speed.


/// Indicate that UART Serial Wire Output (SWO) trace is available.
#define SWO_UART                0               ///< SWO UART:  1 = available, 0 = not available

#define SWO_UART_MAX_BAUDRATE   10000000U       ///< SWO UART Maximum Baudrate in Hz

/// Indicate that Manchester Serial Wire Output (SWO) trace is available.
#define SWO_MANCHESTER          0               ///< SWO Manchester:  1 = available, 0 = not available

#define SWO_BUFFER_SIZE         4096U           ///< SWO Trace Buffer Size in bytes (must be 2^n)
// #define SWO_BUFFER_SIZE         8192U           ///< SWO Trace Buffer Size in bytes (must be 2^n)

/// Debug Unit is connected to fixed Target Device.
#define TARGET_DEVICE_FIXED     0               ///< Target Device: 1 = known, 0 = unknown;



//**************************************************************************************************
/**
JTAG I/O Pin                 | SWD I/O Pin          | CMSIS-DAP Hardware pin mode
---------------------------- | -------------------- | ---------------------------------------------
TCK: Test Clock              | SWCLK: Clock         | Output Push/Pull
TMS: Test Mode Select        | SWDIO: Data I/O      | Output Push/Pull; Input (for receiving data)
TDI: Test Data Input         |                      | Output Push/Pull
TDO: Test Data Output        |                      | Input
nTRST: Test Reset (optional) |                      | Output Open Drain with pull-up resistor
nRESET: Device Reset         | nRESET: Device Reset | Output Open Drain with pull-up resistor

DAP Hardware I/O Pin Access Functions
*/

#include "stm32f4xx_hal.h"
// #include "interface_uart.h"

// Configure DAP I/O pins ------------------------------

#define SWCLK_PORT			GPIOA
#define SWCLK_PIN  			GPIO_PIN_5
#define SWDIO_PORT			GPIOA
#define SWDIO_PIN  			GPIO_PIN_6
#define SWDIO_PIN_INDEX		6

#define SWDIO_MODE_MASK 	(~(3u << (SWDIO_PIN_INDEX * 2)))
#define SWDIO_MODE_OUT 		(1u << (SWDIO_PIN_INDEX * 2))
#define SWDIO_PUPD_MASK 	(~(3u << (SWDIO_PIN_INDEX * 2)))
#define SWDIO_PUPD_PU 		(1u << (SWDIO_PIN_INDEX * 2))

#define nRST_PORT			GPIOA
#define nRST_PIN			GPIO_PIN_7

#define LED_CONNECTED_PORT  GPIOC
#define LED_CONNECTED_PIN   GPIO_PIN_13
#define LED_RUNNING_PORT    GPIOC
#define LED_RUNNING_PIN     GPIO_PIN_5


/** Setup JTAG I/O pins: TCK, TMS, TDI, TDO, nTRST, and nRESET.
 - TCK, TMS, TDI, nTRST, nRESET to output mode and set to high level.
 - TDO to input mode.
*/
static void PORT_JTAG_SETUP(void)
{
#if (DAP_JTAG != 0)
#endif
}

/** Setup SWD I/O pins: SWCLK, SWDIO, and nRESET.
 - SWCLK, SWDIO, nRESET to output mode and set to default high level.
*/
static void PORT_SWD_SETUP(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	GPIOA->BSRR = SWCLK_PIN | SWDIO_PIN;

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();

	GPIO_InitStruct.Pin = SWCLK_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(SWCLK_PORT, &GPIO_InitStruct);

	GPIO_InitStruct.Pin = SWDIO_PIN;
	HAL_GPIO_Init(SWDIO_PORT, &GPIO_InitStruct);

	// GPIO_InitStruct.Pin = nRST_PIN;
	// GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	// GPIO_InitStruct.Pull = GPIO_PULLUP;
	// GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
	// HAL_GPIO_Init(nRST_PORT, &GPIO_InitStruct);

	GPIO_InitStruct.Pin = LED_CONNECTED_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
	HAL_GPIO_Init(LED_CONNECTED_PORT, &GPIO_InitStruct);

	SWCLK_PORT->BSRR = SWCLK_PIN;
	SWDIO_PORT->BSRR = SWDIO_PIN;
	LED_CONNECTED_PORT->BSRR = LED_CONNECTED_PIN;
}

/** Disable JTAG/SWD I/O Pins.
 - TCK/SWCLK, TMS/SWDIO, TDI, TDO, nTRST, nRESET to High-Z mode.
*/
static void PORT_OFF(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	GPIO_InitStruct.Pin = SWCLK_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(SWCLK_PORT, &GPIO_InitStruct);

	GPIO_InitStruct.Pin = SWDIO_PIN;
	HAL_GPIO_Init(SWDIO_PORT, &GPIO_InitStruct);
}


// SWCLK/TCK I/O pin -------------------------------------
#pragma clang optimize off
// Current status of the SWCLK/TCK DAP hardware I/O pin
static inline uint32_t PIN_SWCLK_TCK_IN(void)
{
	return (SWCLK_PORT->ODR & SWCLK_PIN) ? 1 : 0;
}

static inline void PIN_SWCLK_TCK_SET(void)
{
	SWCLK_PORT->BSRR = SWCLK_PIN;
}

static inline void PIN_SWCLK_TCK_CLR(void)
{
	SWCLK_PORT->BSRR = SWCLK_PIN << 16U;
}


// SWDIO/TMS Pin I/O --------------------------------------

// Current status of the SWDIO/TMS DAP hardware I/O pin
static inline uint32_t PIN_SWDIO_TMS_IN(void)
{
	return (SWDIO_PORT->IDR & SWDIO_PIN) ? 1 : 0;
}

static inline void PIN_SWDIO_TMS_SET(void)
{
	SWDIO_PORT->BSRR = SWDIO_PIN;
}

static inline void PIN_SWDIO_TMS_CLR(void)
{
	SWDIO_PORT->BSRR = SWDIO_PIN << 16U;
}


static inline uint32_t PIN_SWDIO_IN(void)
{
	return (SWDIO_PORT->IDR & SWDIO_PIN) ? 1 : 0;
}

static inline void PIN_SWDIO_OUT(uint32_t bit)
{
	if (bit & 1)
		SWDIO_PORT->BSRR = SWDIO_PIN;
	else
		SWDIO_PORT->BSRR = SWDIO_PIN << 16U;
}

static inline void PIN_SWDIO_OUT_ENABLE(void)
{
	nRST_PORT->BSRR = nRST_PIN;

	uint32_t temp;
	temp = SWDIO_PORT->MODER;
	temp &= SWDIO_MODE_MASK;
	temp |= SWDIO_MODE_OUT;
	SWDIO_PORT->MODER = temp;

	temp = SWDIO_PORT->PUPDR; 	// 读取上拉/下拉寄存器
	temp &= SWDIO_PUPD_MASK;	// 清除对应引脚的上下拉配置位
	temp |= SWDIO_PUPD_PU;		// 设置为上拉
	SWDIO_PORT->PUPDR = temp;

	SWDIO_PORT->BSRR = SWDIO_PIN << 16U;

	// Usart_debugMsg("[DAP] set swdio out");
}

static inline void PIN_SWDIO_OUT_DISABLE(void) 
{
	nRST_PORT->BSRR = nRST_PIN << 16U;

	uint32_t temp;
	temp = SWDIO_PORT->MODER;
	temp &= SWDIO_MODE_MASK;
	SWDIO_PORT->MODER = temp;

	SWDIO_PORT->BSRR = SWDIO_PIN << 16U;

	// Usart_debugMsg("[DAP] set swdio in");
}

// TDI Pin I/O ---------------------------------------------

static inline uint32_t PIN_TDI_IN(void)
{
#if (DAP_JTAG != 0)
#endif
	return 0;
}

static inline void PIN_TDI_OUT(uint32_t bit)
{
#if (DAP_JTAG != 0)
#endif
}


// TDO Pin I/O ---------------------------------------------

static inline uint32_t PIN_TDO_IN(void)
{
#if (DAP_JTAG != 0)
#endif
	return 0;
}


// nTRST Pin I/O -------------------------------------------

static inline uint32_t PIN_nTRST_IN(void)
{
    return 0;
}

static inline void PIN_nTRST_OUT(uint32_t bit)
{
}

// nRESET Pin I/O------------------------------------------
static inline uint32_t PIN_nRESET_IN(void)
{
	// return (uint32_t)(nRST_PORT->IDR & nRST_PIN ? 1 : 0);
	return 0;
}

// extern uint8_t swd_write_word(uint32_t addr, uint32_t val);
extern uint8_t swd_init_debug(void);
extern uint8_t swd_write_memory(uint32_t address, uint8_t *data, uint32_t size);
static inline void PIN_nRESET_OUT(uint32_t bit)
{
	// if (bit & 1)
	// 	nRST_PORT->BSRR = (uint32_t)nRST_PIN;
	// else
	// 	nRST_PORT->BSRR = (uint32_t)nRST_PIN << 16U;

	// if (bit == 0)
	// {
	// 	swd_write_word((uint32_t)&SCB->AIRCR, ((0x5FA << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk));
	// }

	// GPIO_InitTypeDef GPIO_InitStruct = {0};

	// if (bit & 1)
	// {
	// 	nRST_PORT->BSRR = (uint32_t)nRST_PIN;

	// 	GPIO_InitStruct.Pin = nRST_PIN;
	// 	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	// 	GPIO_InitStruct.Pull = GPIO_NOPULL;
	// 	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	// 	HAL_GPIO_Init(nRST_PORT, &GPIO_InitStruct);
	// }
	// else
	// {
	// 	nRST_PORT->BSRR = (uint32_t)nRST_PIN << 16U;

	// 	GPIO_InitStruct.Pin = nRST_PIN;
	// 	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
	// 	GPIO_InitStruct.Pull = GPIO_PULLUP;
	// 	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	// 	HAL_GPIO_Init(nRST_PORT, &GPIO_InitStruct);

	// 	swd_init_debug();
	// 	uint32_t swd_mem_write_data = 0x05FA0000 | 0x4;
	// 	swd_write_memory(0xE000ED0C, (uint8_t *)&swd_mem_write_data, 4);
	// }
}


//**************************************************************************************************
/** Connect LED: is active when the DAP hardware is connected to a debugger
    Running LED: is active when program execution in target started
*/

static inline void LED_CONNECTED_OUT(uint32_t bit)
{
	if (bit & 1)
		LED_CONNECTED_PORT->BSRR = LED_CONNECTED_PIN << 16U; // 低电平 -> led on
	else
		LED_CONNECTED_PORT->BSRR = LED_CONNECTED_PIN; // 高电平 -> led off
}

static inline void LED_RUNNING_OUT(uint32_t bit)
{
	// if (bit & 1)
	// 	LED_RUNNING_PORT->BSRR = LED_RUNNING_PIN;
	// else
	// 	LED_RUNNING_PORT->BSRR = LED_RUNNING_PIN << 16U;
}
#pragma clang optimize on

static void DAP_SETUP(void)
{
	PORT_OFF();
	// PORT_SWD_SETUP();
}


static uint32_t RESET_TARGET(void)
{
    return (0);              // change to '1' when a device reset sequence is implemented
}


#endif // __DAP_CONFIG_H__
