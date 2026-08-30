#include "App_Led_HL.h"
#include "App_Config.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_tim.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c):
 * APP_RCC_APB1_PERIPH 含 RCC_APB1Periph_TIM2 (RUN 灯硬件 PWM).
 *
 * RUN=PA2 即 TIM2_CH3 默认映射 (无需重映射), 呼吸调光直接硬件 PWM:
 * TIM2 1MHz 计数 / ARR=999 -> 1kHz PWM, 亮度 (App_Led 0..999) 直写 CCR3,
 * 占空比 = CCR/1000. 无需任何中断, 消除旧方案 10kHz 更新中断翻转开销.
 * PWM 占空比(亮度)由 App_Led.c 软件伽马²斜坡更新 CCR.
 * ERR=PA3 故障指示, 默认灭, 直接 GPIO 开关*/

#define LED_PWM_PRESC     71U        /* 72MHz / (71+1) = 1MHz 计数 */
#define LED_PWM_ARR       999U       /* 1MHz / (999+1) = 1kHz PWM 周期 */
#define LED_BRIGHTNESS_MAX 999U      /* 上层 (App_Led) 传入的亮度范围上限 */

void LedHl_Init(void)
{
    GPIO_InitTypeDef gpio;
    TIM_TimeBaseInitTypeDef tim;
    TIM_OCInitTypeDef oc;

    /* 1) ERR=PA3 推挽输出, 默认灭 (高电平点亮) */
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Pin   = LED_ERR_GPIO_PIN;
    GPIO_Init(LED_ERR_GPIO_PORT, &gpio);
    GPIO_ResetBits(LED_ERR_GPIO_PORT, LED_ERR_GPIO_PIN);

    /* 2) RUN=PA2 复用推挽, 交 TIM2_CH3 硬件 PWM 驱动 (高电平点亮) */
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin   = LED_RUN_GPIO_PIN;
    GPIO_Init(LED_RUN_GPIO_PORT, &gpio);

    /* 3) TIM2 时基: 1MHz 计数, ARR=999 -> 1kHz PWM */
    TIM_DeInit(TIM2);
    tim.TIM_Prescaler     = LED_PWM_PRESC;
    tim.TIM_Period        = LED_PWM_ARR;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &tim);

    /* 4) CH3 = PA2, PWM1 模式, CCR 即亮度 (0=灭, 999=最亮) */
    oc.TIM_OCMode        = TIM_OCMode_PWM1;
    oc.TIM_OutputState   = TIM_OutputState_Enable;
    oc.TIM_Pulse         = 0u;
    oc.TIM_OCPolarity    = TIM_OCPolarity_High;
    oc.TIM_OCIdleState   = TIM_OCIdleState_Reset;
    oc.TIM_OCNIdleState  = TIM_OCNIdleState_Reset;
    oc.TIM_OCNPolarity   = TIM_OCPolarity_High;
    oc.TIM_OutputNState  = TIM_OutputState_Disable;
    TIM_OC3Init(TIM2, &oc);
    TIM_OC3PreloadConfig(TIM2, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM2, ENABLE);

    TIM_Cmd(TIM2, ENABLE);
}

/* 设置 RUN 亮度 (上层 0..999), 直写 CCR3 即占空比 (1kHz 周期 ARR=999) */
void LedHl_RunSetBrightness(uint16_t ccr)
{
    if (ccr > LED_BRIGHTNESS_MAX) ccr = LED_BRIGHTNESS_MAX;
    TIM_SetCompare3(TIM2, ccr);
}

void LedHl_RunOn(void)  { TIM_SetCompare3(TIM2, LED_BRIGHTNESS_MAX + 1U); } /* CCR>ARR = 100% 常亮 */
void LedHl_RunOff(void) { TIM_SetCompare3(TIM2, 0U); }                     /* RUN 灭 */

/* ERR 故障灯: 常亮 / 灭 (默认灭)*/
void LedHl_EOn(void)
{
    GPIO_SetBits(LED_ERR_GPIO_PORT, LED_ERR_GPIO_PIN);
}
void LedHl_EOff(void)
{
    GPIO_ResetBits(LED_ERR_GPIO_PORT, LED_ERR_GPIO_PIN);
}
