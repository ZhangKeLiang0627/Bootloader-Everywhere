// 本工程的用户代码入口。
//
// 为什么单独一个目录：Core/ 下的文件（main.c / gpio.c / usart.c / stm32f4xx_it.c …）
// 都是 CubeMX 生成的，改动集中在 main.c 的 USER CODE 区；把「我们自己的代码」放到
// 这里，重新用 CubeMX 生成代码不会覆盖它。
//
// main.c 里只做三件事：HAL_Init -> SystemClock_Config -> MX_xxx_Init，然后调 Main()。

#include "common_inc.h"       /* 声明了 Main()，并带进 main.h / HAL 头 */

#include "bl.h"               /* blRun() */

/* ========================================================================
 * PC0：按住它上电 = 留在 IAP
 *
 * 库不初始化外设，"这个脚是输入还是输出、有没有上拉"由宿主说了算。
 * 库那边只做两件事：先确认 GPIOC 时钟已开（没开就当「没按」处理，避免时钟
 * 关掉时 IDR 恒 0 被误判成「按住」），再连读两次电平、都为按下才算数。
 *
 * 换板子/换引脚：改这里 + Bootloader/bl_port_stm32f4.cpp 顶部那四行宏。
 * ======================================================================*/
static void bootPinInit(void)
{
  GPIO_InitTypeDef btn = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();

  btn.Pin  = GPIO_PIN_0;
  btn.Mode = GPIO_MODE_INPUT;
  btn.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOC, &btn);
}

/* ========================================================================
 * PC13：Bootloader 运行期间闪烁（0.5s 一次）
 *
 * 翻转逻辑在 stm32f4xx_it.c 的 SysTick_Handler 里（CubeMX 的 USER CODE 区）。
 * 之所以能用 SysTick 判断：库在跳转 APP 之前会关掉包括 SysTick 在内的所有中断，
 * 所以「SysTick 在跑」就等价于「当前固件是 Bootloader」：
 *
 *   灯在闪  -> 正在跑 Bootloader（正在等升级 / 刚上电还没决策）
 *   灯不闪  -> 已跳到 APP
 *
 * PC13 属 VBAT 供电域，数据手册限流 ≤3mA、速度 ≤2MHz，故用低速；
 * 建议接成「低电平点亮」（阳极经限流电阻接 3V3，阴极接 PC13）。
 * ======================================================================*/
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

/* ========================================================================
 * 入口：main.c 调它。做 Bootloader 专属的准备，然后把控制权交给库。
 *
 * ⚠️ blRun() 不返回 —— 它要么在 IAP 里等升级，要么跳进 APP。
 *    所以下面这个 for(;;) 体实际只执行一次；写成循环是为了语义清楚：
 *    「这里就是主循环」，将来若 blRun() 增加了返回路径也不会漏掉。
 * ======================================================================*/
extern "C" void Main(void)
{
  bootPinInit();
  bootLedInit();

  for (;;)
  {
    blRun();
  }
}
