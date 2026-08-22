#ifndef __APP_BOOTSELFTEST_H
#define __APP_BOOTSELFTEST_H

/* =====================================================================
 * 上电自检 (POST) — 硬件验证闭环
 *   1. 蜂鸣器响 500ms 提示上电
 *   2. 逐个探测外设 (步进电机 / UHF / AM) 并读其状态
 *   3. 结果(含通信错误)经调试串口 (UART4) 打印
 *   需在开全局中断后调用 (UHF/AM 的帧收依赖 USART 收中断)。
 * ===================================================================== */

void App_BootSelfTest_Run(void);

#endif /* __APP_BOOTSELFTEST_H */
