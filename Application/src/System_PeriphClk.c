#include "System_PeriphClk.h"
#include "App_Config.h"
#include "stm32f10x_rcc.h"

/* ============================================================
 *  System_PeriphClkInit - 统一开启 APP 所有外设时钟
 *
 *  对应宏定义在 App_Config.h:
 *    APP_RCC_APB2_PERIPH : GPIOA | GPIOB | GPIOC | AFIO | USART1 (AM消磁器/RS232)
 *    APP_RCC_APB1_PERIPH : USB | TIM2(RUN灯硬件PWM) | TIM4(STEP脉冲) | SPI3(步进电机)
 *                         | USART3(UHF) | UART4(调试串口, 已禁用)
 *
 *  注意:
 *  - RCC_USBCLKConfig (PLL/1.5 选 48M) 由 App_Sys_CfgClock 完成,
 *    此处只负责外设使能位。
 *  - 调用前后无须 cpsid/cpsie, 时钟开启是寄存器直接写入, 不产生中断。
 * ============================================================ */
void System_PeriphClkInit(void)
{
    RCC_APB2PeriphClockCmd(APP_RCC_APB2_PERIPH, ENABLE);
    RCC_APB1PeriphClockCmd(APP_RCC_APB1_PERIPH, ENABLE);
}
