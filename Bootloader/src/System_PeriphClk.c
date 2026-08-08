#include "System_PeriphClk.h"
#include "Boot_Config.h"
#include "stm32f10x_rcc.h"

/* ============================================================
 *  System_PeriphClkInit - 统一开启 BOOT 所有外设时钟
 *
 *  对应宏定义在 Boot_Config.h:
 *    BOOT_RCC_APB2_PERIPH : GPIOA | GPIOB | AFIO
 *    BOOT_RCC_APB1_PERIPH : USB
 *
 *  注意:
 *  - RCC_USBCLKConfig (PLL/1.5 选 48M) 由 Sys_CfgClock (main.c) 完成,
 *    此处只负责外设使能位。
 *  - 调用前后无须 cpsid/cpsie, 时钟开启是寄存器直接写入, 不产生中断。
 * ============================================================ */
void System_PeriphClkInit(void)
{
    RCC_APB2PeriphClockCmd(BOOT_RCC_APB2_PERIPH, ENABLE);
    RCC_APB1PeriphClockCmd(BOOT_RCC_APB1_PERIPH, ENABLE);
}