#include "Boot_Led.h"
#include "Boot_Led_HL.h"
#include "Boot_SysTick_HL.h"

/* 需求: Boot 绿灯快闪. 100ms 翻转 = 5Hz, 人眼可读的快闪;
 * 50ms(10Hz) 呈现为"半亮/微光"而非闪烁 (上机观感, 2026-08-30 调整) */
#define BOOT_LED_TOGGLE_MS  100U

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
