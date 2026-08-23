#ifndef __APP_RGB_LED_HL_H
#define __APP_RGB_LED_HL_H

#include <stdint.h>

/* 三色灯硬件层:
 * G=PA4, R=PA5, B=PA6 (高电平点亮), 直接 GPIO 开关, 无调光.
 * 颜色位掩码: G=bit0, R=bit1, B=bit2 (见 App_Config.h RGB_BIT_*).*/
void RgbLedHl_Init(void);

/* 按位掩码一次性设置三色: 置位=亮, 清位=灭.
 * 仅取 bit0~2 (G/R/B), 其余位忽略.*/
void RgbLedHl_Set(uint8_t mask);

#endif /* __APP_RGB_LED_HL_H*/
