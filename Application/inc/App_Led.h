#ifndef __APP_LED_H
#define __APP_LED_H

#include <stdint.h>

void AppLedInit(void);
void AppLedProcess(void);     /* 绿灯状态心跳 */

/* 触发诊断指示 (触发期间驱动, 不锁存): ERR 灯.
 *  行程 UP 按住   -> 100ms 闪;  DOWN 按住 -> 1000ms 闪 (同按 DOWN 优先)
 *  IR(光电 PC4) 触发 -> 长亮
 *  行程+IR 同触发 -> 行程闪烁优先 (行程关电机安全, 优先级最高)
 *  全无触发 -> 灭. 由主循环调用. */
void AppLed_KeyBlinkProcess(void);

/* 行程开关错误的 RGB 黄/粉红指示已迁移至 App_RgbLed_Pattern (仲裁器). */

#endif /* __APP_LED_H */
