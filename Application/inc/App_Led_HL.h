#ifndef __APP_LED_HL_H
#define __APP_LED_HL_H

#include <stdint.h>

/* 单灯硬件层: LED_G (PB4) = TIM3_CH1 硬件 PWM (呼吸灯).
 * Init 后 PB4 由 TIM3_CH1 PWM 驱动, 亮度由 LedHl_GSetBrightness 设置.*/
void LedHl_Init(void);

void LedHl_GSetBrightness(uint16_t ccr);   /* 0..999, 0=灭 999=全亮*/
void LedHl_GOn(void);                       /* 全亮 (故障指示用)*/
void LedHl_GOff(void);                      /* 灭*/

#endif /* __APP_LED_HL_H*/
