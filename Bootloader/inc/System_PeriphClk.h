#ifndef __BOOT_SYSTEM_PERIPH_CLK_H
#define __BOOT_SYSTEM_PERIPH_CLK_H

/* ============================================================
 *  System_PeriphClk - Bootloader 外设时钟统一初始化
 *
 *  设计要点:
 *  - 所有外设 RCC APB/AHB 时钟使能集中到一处, 替代各 HL_Init 分散调用
 *  - 调用顺序: Sys_CfgClock() (72MHz 时钟树) -> System_PeriphClkInit()
 *  - DISABLE 仍由各 HL 的 DeInit 自行处理 (单外设回收)
 *  - 不包含 RCC_USBCLKConfig (PLL 选择器), 该设置归 Sys_CfgClock 负责
 * ============================================================ */

/* 一次性开启 BOOT 全部外设时钟 (APB2 / APB1 / AHB)
 * 应在 main() 开头、HL_Init() 之前调用 */
void System_PeriphClkInit(void);

#endif /* __BOOT_SYSTEM_PERIPH_CLK_H */