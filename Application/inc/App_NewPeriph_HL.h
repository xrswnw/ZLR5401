#ifndef __APP_NEWPERIPH_HL_H
#define __APP_NEWPERIPH_HL_H

#include <stdint.h>

/* =====================================================================
 * 新增外设硬件抽象层 (骨架, 见原理图)
 *   光电接口 (TLP181): MCU_IR1_DET = PC5 (输入, 检测红外/光电)
 *   行程开关:  MCU_KEY_UP   = PC8 (输入, 上行程)
 *              MCU_KEY_DOWN = PC9 (输入, 下行程, 已恢复)
 *   蜂鸣器:    MCU_BEEP5V0_CTL = PC12 (输出, 高电平响)
 *   调试串口:  UART4 TX=PC10 (AF_PP), RX=PC11 (输入浮空), 115200-8-N-1
 *             (UART4_IRQ 不在 MD 向量表, 调试串口仅轮询收发)
 * ===================================================================== */

void App_NewPeriph_Init(void);     /* 初始化全部 GPIO + 调试串口 */

/* ---- 光电 / 行程开关 (输入, 高电平有效) ---- */
uint8_t App_NewPeriph_ReadIr(void);       /* 1=检测到红外/光电 (PC4) */
uint8_t App_NewPeriph_ReadKeyUp(void);    /* 1=上行程触发 (PC8) */
uint8_t App_NewPeriph_ReadKeyDown(void);  /* 1=下行程触发 (PC9, 已恢复) */

/* ---- 蜂鸣器 (PC12, 高电平响) ---- */
void App_NewPeriph_Beep(uint8_t on);

/* ---- IR 检测(PC4, 高=触发) -> 蜂鸣器 100ms 循环响/停 (主循环周期调用) ---- */
void App_IrBuzzer_Process(void);

/* ---- 调试串口 (UART4, 轮询) ---- */
void App_NewPeriph_DebugInit(void);
int  App_NewPeriph_DebugSend(const uint8_t *buf, uint16_t len);  /* 阻塞发送 */
int  App_NewPeriph_DebugRecv(uint8_t *buf, uint16_t maxlen);     /* 读取可用数据 */
void App_NewPeriph_DebugPutStr(const char *s);

#endif /* __APP_NEWPERIPH_HL_H */
