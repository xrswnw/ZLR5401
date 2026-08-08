#include "App_Sys_CfgClock.h"
#include "stm32f10x.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_flash.h"

void App_Sys_CfgClock(void)
{
    ErrorStatus HSEStartUpStatus = ERROR;

    RCC_DeInit();
    /* Enable HSE */
    RCC_HSEConfig(RCC_HSE_ON);

    /* Wait till HSE is ready */
    HSEStartUpStatus = RCC_WaitForHSEStartUp();

    if (HSEStartUpStatus == SUCCESS) {
        /* HCLK = SYSCLK = 72M */
        RCC_HCLKConfig(RCC_SYSCLK_Div1);

        /* PCLK2 = HCLK = 72M */
        RCC_PCLK2Config(RCC_HCLK_Div1);

        /* PCLK1 = HCLK/2 = 36M */
        RCC_PCLK1Config(RCC_HCLK_Div2);

        /* ADCCLK = PCLK2/2 */
        RCC_ADCCLKConfig(RCC_PCLK2_Div2);

        /* Select USBCLK source 72 / 1.5 = 48M */
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);

        /* Flash 2 wait state */
        FLASH_SetLatency(FLASH_Latency_2);

        /* Enable Prefetch Buffer */
        FLASH_PrefetchBufferCmd(FLASH_PrefetchBuffer_Enable);

        /* PLLCLK = 12MHz * 6 = 72 MHz */
        RCC_PLLConfig(RCC_PLLSource_HSE_Div1, RCC_PLLMul_6);

        /* Enable PLL */
        RCC_PLLCmd(ENABLE);

        /* Wait till PLL is ready */
        while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) {
        }

        /* Select PLL as system clock source */
        RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);

        /* Wait till PLL is used as system clock source */
        while (RCC_GetSYSCLKSource() != 0x08) {
        }
    }
    /* else: HSE 起不来则保持 HSI 8MHz, SysTick 用错的 SystemCoreClock 配
     * 这是原有行为; 后加超时 fallback 可在此处处理 */

    /* 同步软件变量 SystemCoreClock 到当前 SYSCLK 来源/分频,
     * 调用方 (main.c) 无须再手动调 SystemCoreClockUpdate(). */
    SystemCoreClockUpdate();
}
