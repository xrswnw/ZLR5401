/* Boot 中断服务程序 */
#include "stm32f10x_it.h"
#include "Boot_SysTick_HL.h"
#include "Boot_FaultStorm.h"

/* Cortex-M3 处理器异常处理
 * Round_098 BUG#5: Boot 无 IWDG, fault 死循环 = 设备永久哑死 (USB 无响应、
 * SysTick 冻结、只能 JLink/断电救). App 侧 fault 早已是复位策略; Boot 对齐:
 * 复位后 Boot 重新可响应 (flash UPG 标志/2.5s 窗口均在), IAP 会话丢失但
 * 协议幂等 (START 重擦重传), 主机重试即可恢复.
 * Round_098 优化 #7: 直接复位在确定性故障下会形成快速复位风暴, 主机抓
 * 不到稳定窗口. 经 BootStorm_OnFault 计数, 连续 >=3 次后常驻升级循环
 * 等待 IAP 救援, 不再复位. */
void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    BootStorm_OnFault();
}

void MemManage_Handler(void)
{
    BootStorm_OnFault();
}

void BusFault_Handler(void)
{
    BootStorm_OnFault();
}

void UsageFault_Handler(void)
{
    BootStorm_OnFault();
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
