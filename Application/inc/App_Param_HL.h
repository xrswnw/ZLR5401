#ifndef __APP_PARAM_HL_H
#define __APP_PARAM_HL_H

#include <stdint.h>

/* Flash 控制器原语 (RAMCODE) */
void FlashHl_Unlock(void);
void FlashHl_Lock(void);
int  FlashHl_ErasePage(uint32_t addr);
int  FlashHl_WriteWord(uint32_t addr, uint32_t data);

/* 参数区安全写序列: 关中断/清fault/关预取/解锁 与 加锁/开预取/开中断 */
void FlashHl_SaveBegin(void);
void FlashHl_SaveFinish(void);

#endif /* __APP_PARAM_HL_H */
