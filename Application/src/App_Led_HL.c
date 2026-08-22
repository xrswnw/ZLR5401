#include "App_Led_HL.h"
#include "App_Config.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_tim.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c):
 * APP_RCC_APB1_PERIPH 含 RCC_APB1Periph_TIM3 (LED 软 PWM 时基).
 *
 * RUN=PA2 无定时器通道映射, 呼吸调光改用软件 PWM:
 * TIM3 仅作 10kHz 时基, 更新中断里把软件计数器与亮度 CCR 比较, 翻转 PA2。
 * PWM 占空比(亮度)由 App_Led.c 软件伽马²斜坡更新 CCR。
 * ERR=PA3 故障指示, 默认灭, 直接 GPIO 开关*/

#define LED_PWM_TICK_HZ   1000000U   /* 1MHz 计数 tick (定时器钟 72MHz/(71+1)) */
#define LED_PWM_PRESC     71U        /* 72MHz / (71+1) = 1MHz */
#define LED_PWM_ARR       99U        /* 更新事件 1MHz/(99+1) = 10kHz */
#define LED_PWM_PERIOD    100U       /* 软件 PWM 周期 (0..99), LED PWM = 10kHz */
#define LED_BRIGHTNESS_MAX 999U      /* 上层 (App_Led) 传入的亮度范围上限 */

static volatile uint8_t  s_ledPhase;
static volatile uint16_t s_ledCcr;   /* 当前占空比 (0..LED_PWM_PERIOD-1) */

/* TIM3 更新中断: 10kHz 软件 PWM 相位累加, 按 CCR 比较翻转 RUN=PA2 (高电平点亮) */
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

        s_ledPhase = (uint8_t)((s_ledPhase + 1U) % LED_PWM_PERIOD);
        if (s_ledPhase < s_ledCcr) GPIO_SetBits(LED_RUN_GPIO_PORT, LED_RUN_GPIO_PIN);
        else                       GPIO_ResetBits(LED_RUN_GPIO_PORT, LED_RUN_GPIO_PIN);
    }
}

void LedHl_Init(void)
{
    GPIO_InitTypeDef gpio;
    TIM_TimeBaseInitTypeDef tim;

    /* 1) RUN=PA2 + ERR=PA3 推挽输出, 默认灭 (高电平点亮) */
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;

    gpio.GPIO_Pin   = LED_RUN_GPIO_PIN;
    GPIO_Init(LED_RUN_GPIO_PORT, &gpio);
    GPIO_ResetBits(LED_RUN_GPIO_PORT, LED_RUN_GPIO_PIN);

    gpio.GPIO_Pin   = LED_ERR_GPIO_PIN;
    GPIO_Init(LED_ERR_GPIO_PORT, &gpio);
    GPIO_ResetBits(LED_ERR_GPIO_PORT, LED_ERR_GPIO_PIN);

    /* 2) TIM3 时基: 1MHz 计数, ARR=99 -> 10kHz 更新中断 (软 PWM 相位源) */
    TIM_DeInit(TIM3);
    tim.TIM_Prescaler     = LED_PWM_PRESC;
    tim.TIM_Period        = LED_PWM_ARR;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim);

    /* 3) 使能更新中断 (NVIC 优先级 7) */
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
    NVIC_SetPriority(TIM3_IRQn, 7);
    NVIC_EnableIRQ(TIM3_IRQn);

    s_ledPhase = 0;
    s_ledCcr   = 0;

    TIM_Cmd(TIM3, ENABLE);
}

/* 设置 RUN 亮度 (上层 0..999), 映射到软 PWM 占空比 0..99, 0=灭 */
void LedHl_RunSetBrightness(uint16_t ccr)
{
    if (ccr > LED_BRIGHTNESS_MAX) ccr = LED_BRIGHTNESS_MAX;
    s_ledCcr = (uint16_t)(ccr * LED_PWM_PERIOD / (LED_BRIGHTNESS_MAX + 1U));
}

void LedHl_RunOn(void)  { s_ledCcr = LED_PWM_PERIOD - 1U; }  /* RUN 常亮 */
void LedHl_RunOff(void) { s_ledCcr = 0U; }                  /* RUN 灭 */

/* ERR 故障灯: 常亮 / 灭 (默认灭)*/
void LedHl_EOn(void)
{
    GPIO_SetBits(LED_ERR_GPIO_PORT, LED_ERR_GPIO_PIN);
}
void LedHl_EOff(void)
{
    GPIO_ResetBits(LED_ERR_GPIO_PORT, LED_ERR_GPIO_PIN);
}
