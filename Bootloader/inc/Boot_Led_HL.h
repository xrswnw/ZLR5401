#ifndef __BOOT_LED_HL_H
#define __BOOT_LED_HL_H

/* 单灯 Boot 心跳: 绿灯 PB4 (高电平点亮) */
void LedHl_Init(void);

void LedHl_On(void);
void LedHl_Off(void);
void LedHl_Toggle(void);

#endif /* __BOOT_LED_HL_H */
