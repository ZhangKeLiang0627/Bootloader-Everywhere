#include "interface_uart.h"

void Usart_sendString(UART_HandleTypeDef *handle, uint8_t *str)
{
    if (handle == NULL || str == NULL)
        return;

    // 计算长度
    uint16_t len = strlen((char *)str);
    if (len == 0)
        return;

    // 整块发送
    HAL_UART_Transmit(handle, str, len, 1000);
}

void Usart_debugMsg(const char *fmt, ...)
{
#if ((DEBUG_ENABLE))

    char debug_buf[DEBUG_PRINT_BUF_SIZE] = {0};
    va_list args;

    va_start(args, fmt);
    vsnprintf(debug_buf, DEBUG_PRINT_BUF_SIZE - 3, fmt, args);
    va_end(args);

    strcat(debug_buf, "\r\n");

    Usart_sendString(&huart1, (uint8_t *)debug_buf);
#endif
}
