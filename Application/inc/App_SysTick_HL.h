#ifndef __APP_SYSTICK_HL_H
#define __APP_SYSTICK_HL_H

#include <stdint.h>

void     SysTickHl_Init(void);
uint32_t SysTickHl_GetMs(void);
void     SysTickHl_Inc(void);
void     SysTickHl_DelayMs(uint32_t ms);

#endif /* __APP_SYSTICK_HL_H */
