#ifndef __BOOT_SYSTICK_HL_H
#define __BOOT_SYSTICK_HL_H

#include <stdint.h>

void     SysTickHl_Init(void);
void     SysTickHl_Stop(void);
uint32_t SysTickHl_GetMs(void);
void     SysTickHl_Inc(void);
void     SysTickHl_DelayMs(uint32_t ms);

#endif /* __BOOT_SYSTICK_HL_H */
