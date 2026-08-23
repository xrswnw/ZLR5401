#ifndef __APP_LED_H
#define __APP_LED_H

void AppLedInit(void);
void AppLedProcess(void);     /* 绿灯状态心跳 */

/* 按键诊断闪烁 (长按保持): 仅在行程开关低电平(按下)时驱动 ERR 灯.
 *  KEY_UP    按住 -> ERR 100ms 闪烁; 放开 -> 常亮
 *  (下行程 KEY_DOWN=PC9 已改作 USB_EN, 分支暂禁用). 由主循环调用. */
void AppLed_KeyBlinkProcess(void);

#endif /* __APP_LED_H */
