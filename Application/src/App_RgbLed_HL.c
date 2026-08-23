#include "App_RgbLed_HL.h"
#include "App_Config.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c):
 * APP_RCC_APB2_PERIPH 含 RCC_APB2Periph_GPIOA.
 * G=PA4, R=PA5, B=PA6 (高电平点亮), 直接 GPIO 开关 (无调光).*/

/* 三色引脚 -> 位掩码映射 (掩码见 App_Config.h RGB_BIT_*) */
static void rgb_write(uint8_t mask)
{
    (mask & RGB_BIT_G) ? GPIO_SetBits(RGB_G_GPIO_PORT, RGB_G_GPIO_PIN)
                       : GPIO_ResetBits(RGB_G_GPIO_PORT, RGB_G_GPIO_PIN);
    (mask & RGB_BIT_R) ? GPIO_SetBits(RGB_R_GPIO_PORT, RGB_R_GPIO_PIN)
                       : GPIO_ResetBits(RGB_R_GPIO_PORT, RGB_R_GPIO_PIN);
    (mask & RGB_BIT_B) ? GPIO_SetBits(RGB_B_GPIO_PORT, RGB_B_GPIO_PIN)
                       : GPIO_ResetBits(RGB_B_GPIO_PORT, RGB_B_GPIO_PIN);
}

void RgbLedHl_Init(void)
{
    GPIO_InitTypeDef gpio;

    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;

    gpio.GPIO_Pin   = RGB_G_GPIO_PIN;
    GPIO_Init(RGB_G_GPIO_PORT, &gpio);
    gpio.GPIO_Pin   = RGB_R_GPIO_PIN;
    GPIO_Init(RGB_R_GPIO_PORT, &gpio);
    gpio.GPIO_Pin   = RGB_B_GPIO_PIN;
    GPIO_Init(RGB_B_GPIO_PORT, &gpio);

    rgb_write(0);   /* 默认全灭 */
}

void RgbLedHl_Set(uint8_t mask)
{
    rgb_write((uint8_t)(mask & (RGB_BIT_G | RGB_BIT_R | RGB_BIT_B)));
}
