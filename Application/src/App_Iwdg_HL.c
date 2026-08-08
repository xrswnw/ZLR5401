#include "App_Iwdg_HL.h"
#include "App_Config.h"
#include "stm32f10x.h"

void IwdgHl_Init(void) {
#if USE_IWDG
    IWDG->KR = 0x5555;
    IWDG->PR = 0x04;
    IWDG->RLR = (IWDG_COUNTER_RATE / IWDG_RELOAD_DIV) * IWDG_TIMEOUT_MS / 1000;
    IWDG->KR = 0xAAAA;
    IWDG->KR = 0xCCCC;
#endif
}

void IwdgHl_Feed(void) {
#if USE_IWDG
    IWDG->KR = 0xAAAA;
#endif
}

