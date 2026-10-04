#ifndef __INTERFACE_USART_H
#define __INTERFACE_USART_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "stm32f4xx_hal.h"
#include "usart.h"

#include <stdint.h>
#include <stdio.h> 
#include <string.h> 
#include <stdarg.h> 

#define DEBUG_PRINT_BUF_SIZE 512
#define DEBUG_ENABLE 1

void Usart_sendString(UART_HandleTypeDef *handle, uint8_t *str);
void Usart_debugMsg(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // __INTERFACE_USART_H
