#include "stm32f10x_it.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x_iwdg.h"

void NMI_Handler(void) {
}

void HardFault_Handler(void) {
#if USE_IWDG
    volatile uint32_t delay = 0x00FFFFFF;
    while (delay--)
        ;
    NVIC_SystemReset();
#else
    while (1) {
    }
#endif
}

void MemManage_Handler(void) {
#if USE_IWDG
    volatile uint32_t delay = 0x00FFFFFF;
    while (delay--)
        ;
    NVIC_SystemReset();
#else
    while (1) {
    }
#endif
}

void BusFault_Handler(void) {
#if USE_IWDG
    volatile uint32_t delay = 0x00FFFFFF;
    while (delay--)
        ;
    NVIC_SystemReset();
#else
    while (1) {
    }
#endif
}

void UsageFault_Handler(void) {
#if USE_IWDG
    volatile uint32_t delay = 0x00FFFFFF;
    while (delay--)
        ;
    NVIC_SystemReset();
#else
    while (1) {
    }
#endif
}

void SVC_Handler(void) {
}

void DebugMon_Handler(void) {
}

void PendSV_Handler(void) {
}

void SysTick_Handler(void) {
    SysTickHl_Inc();
}

/* USB 低优先级中断 (LP CAN1 RX0 共享向量, STM32F103 USB FS 用此)
 * HP / Wakeup / Resume 由 USB_Istr() 内部处理*/
#include "usb_istr.h"
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    USB_Istr();
}
