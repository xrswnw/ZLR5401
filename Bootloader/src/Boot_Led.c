#include "Boot_Led.h"
#include "Boot_Led_HL.h"
#include "Boot_SysTick_HL.h"

/* 需求: Boot 绿灯 50ms 闪烁 */
#define BOOT_LED_TOGGLE_MS  50U

static uint32_t s_u32LastToggle;

void BootLedInit(void) {
    LedHl_Init();
    s_u32LastToggle = 0;
}

void BootLedProcess(void) {
    uint32_t now = SysTickHl_GetMs();
    if ((now - s_u32LastToggle) >= BOOT_LED_TOGGLE_MS) {
        s_u32LastToggle = now;
        LedHl_Toggle();
    }
}

void BootLedOff(void) {
    LedHl_Off();
}
