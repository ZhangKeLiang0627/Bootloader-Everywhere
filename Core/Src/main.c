/* Bootloader-Everywhere 的宿主工程：把芯片初始化好，然后把控制权交给库。
 *
 * 库不做任何初始化，也不带 main() —— 时钟、串口、Flash 接口时钟全在这里配好。
 * usart.c / gpio.c / stm32f4xx_it.c / stm32f4xx_hal_msp.c 都是 CubeMX 生成的，
 * 保持原样即可。
 */
#include "main.h"
#include "usart.h"
#include "gpio.h"
#include "bl.h"

static void SystemClock_Config(void);
static void bootPinInit(void);
static void bootLedInit(void);

int main(void)
{
  HAL_Init();

  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART1_UART_Init();      /* PA9 / PA10，115200 8N1 —— 与上位机一致 */
  bootPinInit();              /* PC0：按住它上电 = 留在 IAP（见下） */
  bootLedInit();              /* PC13：BL 运行期间闪烁，一眼看出有没有进 Bootloader */

  blRun();                    /* 库的入口：决策 + 收固件，永不返回 */

  for (;;) {
  }
}

/* 把 PC0 配成「输入 + 上拉」，供 Bootloader 的「按住按钮上电 = 留在 IAP」使用。
 *
 * 库不初始化外设，所以这个脚由宿主负责配好。库那边只做两件事：
 * 先确认 GPIOC 时钟已开（没开就当「没按」，避免时钟关掉时 IDR 恒 0 被误判成按住），
 * 再读电平，连读两次都按下才算数。
 *
 * 换板子/换引脚：改这里 + Bootloader/bl_port_stm32f4.cpp 顶部那四行宏。
 */
static void bootPinInit(void)
{
  GPIO_InitTypeDef btn = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();

  btn.Pin  = GPIO_PIN_0;
  btn.Mode = GPIO_MODE_INPUT;
  btn.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOC, &btn);
}

/* BL 指示灯：PC13 推挽输出。
 *
 * 用途：Bootloader 固件运行期间，SysTick 中断每隔 500ms 翻转一次这个脚
 * （见 stm32f4xx_it.c 的 SysTick_Handler），于是肉眼就能判断：
 *
 *   - 灯在闪      → 芯片正在跑 Bootloader（按住按钮上电 / APP 区是空的）
 *   - 灯不闪      → 没进 BL（已跳到 APP，或运行的是 APP 固件）
 *
 * 不关心 LED 接法：无论高电平点亮还是低电平点亮，翻转都会让它闪。
 *
 * ⚠️ PC13 属于 VBAT 供电域，驱动能力受限（数据手册：灌电流 ≤3mA、速度 ≤2MHz），
 *    所以用低速。建议接成「低电平点亮」——LED 阳极经限流电阻接 3V3，
 *    阴极接 PC13，由引脚灌电流。
 */
static void bootLedInit(void)
{
  GPIO_InitTypeDef led = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();

  led.Pin   = GPIO_PIN_13;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &led);

  GPIOC->BSRR = GPIO_PIN_13;    /* 起始置高（低电平点亮时即熄灭） */
}

/* 系统时钟：HSI + PLL → 84MHz
 *
 * 本板的 HSE 晶振起不来（实测 HSERDY 恒为 0），所以不依赖外部晶振：
 *   HSI 16MHz --PLLM=16--> 1MHz --PLLN=336--> 336MHz --PLLP=4--> 84MHz
 *   AHB=84MHz，APB1=42MHz，APB2=84MHz，Flash latency=2
 *
 * 没焊晶振、晶振虚焊、负载电容不匹配都照样跑。HSI 精度约 ±1%，
 * 对 115200 波特率完全够用。
 *
 * 想换回外部晶振：PLLSource 改 RCC_PLLSOURCE_HSE、PLLM 改成晶振频率（MHz），
 * 并把 stm32f4xx_hal_conf.h 里的 HSE_VALUE 设成一致。
 */
static void clockHsi16(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState            = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState        = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    Error_Handler();
  }

  clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
    Error_Handler();
  }
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

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

void Error_Handler(void)
{
  __disable_irq();
  for (;;) {
  }
}
