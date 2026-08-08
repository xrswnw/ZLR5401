#include "Boot_Led_HL.h"
#include "Boot_Config.h"
#include "stm32f10x_gpio.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c)*/

void LedHl_Init(void) {
    GPIO_InitTypeDef gpio;

    /* 释放 PB4 (JTAG NJTRST). LedHl_Init 在 USB HL (JTAG remap) 之前
     * 调用, 提前释放 PB4 使 LED 立即可用, 50ms 翻转从初始化起持续可见.*/
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;

    /* LED: PB4 (高电平点亮, 默认输出低 = 灭)*/
    gpio.GPIO_Pin = LED_GPIO_PIN;
    GPIO_Init(LED_GPIO_PORT, &gpio);
    GPIO_ResetBits(LED_GPIO_PORT, LED_GPIO_PIN);
}

void LedHl_On(void)     { GPIO_SetBits(LED_GPIO_PORT, LED_GPIO_PIN); }
void LedHl_Off(void)    { GPIO_ResetBits(LED_GPIO_PORT, LED_GPIO_PIN); }
void LedHl_Toggle(void) {
    if (GPIO_ReadOutputDataBit(LED_GPIO_PORT, LED_GPIO_PIN) == Bit_RESET)
        GPIO_SetBits(LED_GPIO_PORT, LED_GPIO_PIN);
    else
        GPIO_ResetBits(LED_GPIO_PORT, LED_GPIO_PIN);
}
