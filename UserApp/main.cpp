#include "common_inc.h"
#include "interface_uart.h"
#include "ArduinoJson.h"

// 5 User-Timers, can choose from htim1/htim2/htim4/htim10/htim11

/* Thread Definitions -----------------------------------------------------*/


/* Timer Callbacks -------------------------------------------------------*/


/* Default Entry -------------------------------------------------------*/
void Main(void)
{
    HAL_Delay(1000);
    Usart_debugMsg("Bootloader-Everywhere begin!");

    char json[] = "{\"sensor\":\"gps\",\"time\":1351824120,\"data\":[48.756080,2.302038]}";

    DynamicJsonDocument doc(1024);
    deserializeJson(doc, json);

    const char* sensor = doc["sensor"];

    Usart_debugMsg("[ArduinoJson6.20.0] sensor: %s", sensor);
    
    for (;;)
    {   
        Usart_debugMsg("Bootloader-Everywhere is running!");
        HAL_Delay(5000);
    }
}