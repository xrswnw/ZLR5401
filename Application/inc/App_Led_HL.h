#ifndef __APP_LED_HL_H
#define __APP_LED_HL_H

#include <stdint.h>

/* 双灯硬件层:
 * RUN (PA2) = 呼吸灯, 软件 PWM 调光 (TIM3 时基 10kHz + 更新中断翻转).
 * ERR (PA3) = 故障/状态指示, 默认灭, 直接 GPIO 开关 (无调光).*/
void LedHl_Init(void);

/* RUN 呼吸灯亮度 0..999, 0=灭 999=全亮 (AppLedProcess 呼吸驱动)*/
void LedHl_RunSetBrightness(uint16_t ccr);
void LedHl_RunOn(void);                 /* RUN 常亮 */
void LedHl_RunOff(void);                /* RUN 灭 */

/* ERR 故障/状态灯: 常亮 / 灭 (默认灭)*/
void LedHl_EOn(void);
void LedHl_EOff(void);

#endif /* __APP_LED_HL_H*/
