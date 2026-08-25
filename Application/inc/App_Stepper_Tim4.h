#ifndef __APP_STEPPER_TIM4_H
#define __APP_STEPPER_TIM4_H

#include <stdint.h>

/* 步进 STEP 硬件定时器驱动 (TIM4_CH1 = PB6). 见 App_Stepper_Tim4.c */

void     StepperTim4_Init(void);
void     StepperTim4_Start(uint32_t stepsReq, uint32_t targetHz); /* 斜坡起速后升到 targetHz */
void     StepperTim4_Stop(void);
uint32_t StepperTim4_GetStepsDone(void);
uint8_t  StepperTim4_IsRunning(void);

#endif /* __APP_STEPPER_TIM4_H */
