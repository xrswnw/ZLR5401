#ifndef __APP_SYS_CFG_CLOCK_H
#define __APP_SYS_CFG_CLOCK_H

/* 时钟树初始化 (显式从 main 调用)
 * HSE 12MHz × PLL6 = SYSCLK 72MHz, AHB/PCLK2=72MHz, PCLK1=36MHz
 * USB = 72 / 1.5 = 48MHz
 * 与 Bootloader/src/main.c 的 Sys_CfgClock 一致 (复用 StdPeriph API) */
void App_Sys_CfgClock(void);

#endif /* __APP_SYS_CFG_CLOCK_H */
