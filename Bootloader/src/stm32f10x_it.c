/* Boot 中断服务程序 */
#include "stm32f10x_it.h"
#include "Boot_SysTick_HL.h"

/* Cortex-M3 处理器异常处理 */
void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
  while (1) { }
}

void MemManage_Handler(void)
{
  while (1) { }
}

void BusFault_Handler(void)
{
  while (1) { }
}

void UsageFault_Handler(void)
{
  while (1) { }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    SysTickHl_Inc();
}

/* USB ISR - STM32 官方 USB 库 */
#include "usb_istr.h"
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    USB_Istr();
}
