#include "App_SysTick_HL.h"
#include "stm32f10x.h"

static volatile uint32_t g_u32SysTickMs = 0;

void SysTickHl_Init(void) {
    SysTick_Config(SystemCoreClock / 1000);
}

uint32_t SysTickHl_GetMs(void) {
    return g_u32SysTickMs;
}

void SysTickHl_Inc(void) {
    g_u32SysTickMs++;
}

void SysTickHl_DelayMs(uint32_t ms) {
    uint32_t start = g_u32SysTickMs;
    while ((g_u32SysTickMs - start) < ms)
        ;
}
