#include "App_Led_HL.h"
#include "App_Config.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_tim.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c):
 * APP_RCC_APB1_PERIPH 含 RCC_APB1Periph_TIM3 (加)
 * 本模块把 PB4 从普通 GPIO 输出改为 TIM3_CH1 硬件 PWM 输出, 实现呼吸灯调光.*/

/* PB4 = TIM3_CH1 (需 TIM3 部分重映射 + JTAG 释放 NJTRST).
 * 硬件 PWM 调光: TIM3 CH1 输出 PWM, 占空比(亮度)由 App_Led.c 经 SysTick
 * 软件斜坡更新 CCR1 (0->max->0, 2s 一周期), PWM 调光本身由硬件完成 (无 CPU 位拆).
 * TIM3 在 APB1, APB1 预分频=2 -> 定时器时钟 = 72MHz.*/
#define LED_PWM_PRESC    71U    /* 72MHz / (71+1) = 1MHz 计数 tick*/
#define LED_PWM_ARR      999U   /* 1MHz / (999+1) = 1kHz PWM, 占空比分辨率 0..999*/

void LedHl_Init(void) {
    GPIO_InitTypeDef gpio;
    TIM_TimeBaseInitTypeDef tim;
    TIM_OCInitTypeDef oc;

    /* 1) 释放 PB4 (JTAG NJTRST) + TIM3 部分重映射 (TIM3_CH1 -> PB4).
     * 提前在此做, 使 LED 在 USB HL 初始化前即可用 (不依赖 USB init 的 JTAG remap).*/
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    GPIO_PinRemapConfig(GPIO_PartialRemap_TIM3, ENABLE);

    /* 2) PB4 复用推挽 (TIM3_CH1 PWM 输出)*/
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin   = LED_G_GPIO_PIN;
    GPIO_Init(LED_G_GPIO_PORT, &gpio);

    /* 3) TIM3 时基: 1kHz PWM*/
    TIM_DeInit(TIM3);
    tim.TIM_Prescaler         = LED_PWM_PRESC;
    tim.TIM_Period            = LED_PWM_ARR;
    tim.TIM_ClockDivision     = TIM_CKD_DIV1;
    tim.TIM_CounterMode       = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim);

    /* 4) CH1 PWM 模式 1, 高电平有效 (LED 高电平点亮), 默认占空比 0 (灭)*/
    oc.TIM_OCMode      = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse       = 0;
    oc.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC1Init(TIM3, &oc);
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM3, ENABLE);

    TIM_SetCompare1(TIM3, 0);
    TIM_Cmd(TIM3, ENABLE);
}

/* 设置亮度 (CCR1 0..999), 由 App_Led.c 呼吸斜坡调用*/
void LedHl_GSetBrightness(uint16_t ccr) {
    if (ccr > LED_PWM_ARR) ccr = LED_PWM_ARR;
    TIM_SetCompare1(TIM3, ccr);
}

void LedHl_GOn(void)  { TIM_SetCompare1(TIM3, LED_PWM_ARR); }   /* 全亮 (故障指示)*/
void LedHl_GOff(void) { TIM_SetCompare1(TIM3, 0); }            /* 灭*/
