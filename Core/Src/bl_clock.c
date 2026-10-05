/**
 * @file    bl_clock.c
 * @brief   示例工程的系统时钟配置
 *
 * 这个文件属于**示例工程**，不属于库：库一行时钟配置都没有，它只是调用
 * SystemClock_Config()。移植到你自己的工程时，用你 CubeMX 生成的那份
 * （CubeMX 把它放在 main.c 里），把那个文件纳入编译即可 —— 记得别把它的
 * main() 一起带进来（库自带 main）。
 *
 * 本板实测：HSE 晶振起不来（HSERDY 一直为 0），所以这里用内部 RC 走 PLL：
 *
 *   HSI 16MHz --PLLM=16--> 1MHz --PLLN=336--> 336MHz --PLLP=4--> 84MHz
 *   AHB=84MHz，APB1=42MHz，APB2=84MHz，Flash latency=2
 *
 * 好处是不依赖任何外部器件：没焊晶振、晶振虚焊、负载电容不匹配，都照样跑 84MHz。
 * HSI 精度约 ±1%，对 115200 波特率完全够用。
 *
 * 想换回外部晶振：PLLSource 改 RCC_PLLSOURCE_HSE、PLLM 改成晶振频率（MHz），
 * 并把 stm32f4xx_hal_conf.h 里的 HSE_VALUE 设成一致。
 */
#include "stm32f4xx_hal.h"

/** 兜底：纯 HSI 16MHz，不走 PLL。只在 PLL 配不起来时用 */
static void clockHsi16(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        for (;;) {
        }
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
        for (;;) {
        }
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* 16MHz / 16 = 1MHz VCO 输入，× 336 = 336MHz，÷ 4 = 84MHz */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM            = 16;
    osc.PLL.PLLN            = 336;
    osc.PLL.PLLP            = RCC_PLLP_DIV4;
    osc.PLL.PLLQ            = 7;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        clockHsi16();
        return;
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        clockHsi16();
    }
}
