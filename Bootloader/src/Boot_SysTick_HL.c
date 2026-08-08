#include "Boot_SysTick_HL.h"
#include "stm32f10x.h"

static volatile uint32_t g_u32BootTickMs = 0;

void SysTickHl_Init(void) {
    SysTick_Config(SystemCoreClock / 1000U);
    SystemCoreClockUpdate();
    if (SystemCoreClock != 72000000U) {
        SysTick->CTRL = 0;
        SysTick_Config(SystemCoreClock / 1000U);
    }
}

void SysTickHl_Stop(void) {
    SysTick->CTRL = 0;
}

uint32_t SysTickHl_GetMs(void) {
    return g_u32BootTickMs;
}

void SysTickHl_Inc(void) {
    g_u32BootTickMs++;
}

void SysTickHl_DelayMs(uint32_t ms) {
    uint32_t start = g_u32BootTickMs;
    while ((g_u32BootTickMs - start) < ms)
        ;
}
