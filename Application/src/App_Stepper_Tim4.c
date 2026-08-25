#include "App_Stepper_Tim4.h"
#include "App_Config.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_tim.h"

/* ====================================================================
 * 步进 STEP 硬件定时器驱动 (TIM4_CH1 = PB6, PWM 输出).
 *
 * 取代 App_Stepper_Process 里"软件累加 + pin_step_pulse 忙等 10us"的软脉冲,
 * 用 TIM4 硬件 PWM 以微秒级精确间隔自动在 PB6 上产生 STEP 脉冲(50% 占空).
 * 每个完整 PWM 周期 = 一个 STEP 脉冲 (上升+下降沿推进一步). 硬件自动输出,
 * 零 CPU 忙等, 脉冲均匀, 根治软件时基导致的"一顿一顿".
 *
 * 定时器时钟: PCLK1=36MHz, APB1 预分频=2 (>1) -> TIM4 计数时钟 72MHz.
 * 期望微步间隔 interval(us) -> ARR = 72*interval - 1; CCR = ARR/2 (50% duty).
 *
 * 仅驱动 STEP 脉冲产生 + 步数计数 + 限步 + 斜坡调速(ISR 内按步数更新 ARR);
 * 方向(DIR)、使能(EN_OUT/ENABLE)、故障检测仍走 App_Stepper / DRV SDK.
 * ==================================================================== */

#define STEP_TIM_CLK_HZ   72000000UL   /* TIM4 计数时钟 72MHz */

/* 斜坡参数 (与 App_Stepper 一致; 档位相关: 1/2档=400步/圈) */
#define STEP_RAMP_STEPS   400u
#define STEP_RAMP_MIN_HZ  1000u

static volatile uint32_t  s_stepsDone = 0;
static volatile uint32_t  s_stepsReq  = 0;   /* 0=持续 */
static volatile uint8_t   s_run       = 0;
static volatile uint32_t  s_targetHz  = 0;

/* S 形曲线 LUT: 输入 P(0..100) -> 升频权重 S(0..100), 两端缓/中段快.
 * 采样点 = (1-cos(pi*X/10))/2*100, X=0..10 (0,2,10,21,35,50,65,79,90,98,100).
 * 定点线性插值, 避免浮点/链接软库. */
static uint32_t tim4_sin8(uint8_t p)
{
    static const uint8_t lut[11] = {0, 2, 10, 21, 35, 50, 65, 79, 90, 98, 100};
    if (p >= 100u) return 100u;
    uint32_t i = p / 10u;
    uint32_t f = p % 10u;
    return (uint32_t)lut[i] + ((uint32_t)(lut[i + 1] - lut[i]) * f) / 10u;
}

/* 按已完成步数计算当前微步间隔(us): S 曲线升频 */
static uint32_t tim4_ramp_interval(uint32_t stepDone, uint32_t targetHz)
{
    if (stepDone >= STEP_RAMP_STEPS || targetHz <= STEP_RAMP_MIN_HZ)
        return 1000000u / targetHz;
    uint32_t p = stepDone * 100u / STEP_RAMP_STEPS;
    uint32_t s = tim4_sin8((uint8_t)p);
    uint32_t hz = STEP_RAMP_MIN_HZ + (targetHz - STEP_RAMP_MIN_HZ) * s / 100u;
    return 1000000u / hz;
}

/* TIM4 更新中断: 每个 PWM 周期(=1 STEP)触发一次, 计数 + 斜坡 + 限步停机 */
void TIM4_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM4, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM4, TIM_IT_Update);
        if (!s_run) { TIM_Cmd(TIM4, DISABLE); return; }

        s_stepsDone++;

        /* 限步: 达到目标即停 (输出由上层 Stop 关断) */
        if (s_stepsReq != 0u && s_stepsDone >= s_stepsReq) {
            TIM_Cmd(TIM4, DISABLE);
            s_run = 0;
            return;
        }

        /* 斜坡: 起步后逐步升频到目标速 (改 ARR; 周期性更新, 单片阶差可接受) */
        if (s_targetHz != 0u) {
            uint32_t iv = tim4_ramp_interval(s_stepsDone, s_targetHz);
            uint32_t arr = STEP_TIM_CLK_HZ / 1000000UL * iv - 1UL;
            if (arr > 65535UL) arr = 65535UL;
            TIM_SetAutoreload(TIM4, arr);
            TIM_SetCompare1(TIM4, arr / 2UL);
        }
    }
}

/* 配置 PB6 = TIM4_CH1(PWM 复用), TIM4 时基 + 输出比较, 更新中断 */
void StepperTim4_Init(void)
{
    GPIO_InitTypeDef      gpio;
    TIM_TimeBaseInitTypeDef tb;
    TIM_OCInitTypeDef     oc;

    /* PB6 = TIM4_CH1, AF_PP, 50MHz */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin   = MOTOR_STEP_PIN;
    GPIO_Init(MOTOR_CTRL_GPIO_PORT, &gpio);

    TIM_DeInit(TIM4);
    tb.TIM_Prescaler     = 0;                 /* 72MHz 直数 */
    tb.TIM_Period        = 8999u;             /* 默认 125us/步 (8000 微步/s) */
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &tb);

    oc.TIM_OCMode     = TIM_OCMode_PWM1;
    oc.TIM_OutputState  = TIM_OutputState_Enable;
    oc.TIM_Pulse        = 4500u;              /* 50% duty */
    oc.TIM_OCPolarity   = TIM_OCPolarity_High;
    TIM_OC1Init(TIM4, &oc);
    TIM_OC1PreloadConfig(TIM4, TIM_OCPreload_Enable);

    TIM_Cmd(TIM4, DISABLE);
    TIM_ITConfig(TIM4, TIM_IT_Update, ENABLE);
    NVIC_SetPriority(TIM4_IRQn, 8);
    NVIC_EnableIRQ(TIM4_IRQn);

    s_stepsDone = 0; s_stepsReq = 0; s_run = 0; s_targetHz = 0;
}

/* 启动连续 STEP, 从斜坡起步转速开始, ISR 内升到 targetHz */
void StepperTim4_Start(uint32_t stepsReq, uint32_t targetHz)
{
    uint32_t iv = tim4_ramp_interval(0u, targetHz);
    uint32_t arr = STEP_TIM_CLK_HZ / 1000000UL * iv - 1UL;
    if (arr > 65535UL) arr = 65535UL;

    s_stepsDone = 0;
    s_stepsReq  = stepsReq;
    s_targetHz  = targetHz;
    s_run       = 1;

    TIM_Cmd(TIM4, DISABLE);
    TIM_SetAutoreload(TIM4, arr);
    TIM_SetCompare1(TIM4, arr / 2UL);
    TIM_SetCounter(TIM4, 0);
    TIM_Cmd(TIM4, ENABLE);
}

/* 停止 STEP 输出 */
void StepperTim4_Stop(void)
{
    TIM_Cmd(TIM4, DISABLE);
    s_run = 0;
    s_stepsReq = 0;
    s_targetHz = 0;
}

uint32_t StepperTim4_GetStepsDone(void){ return s_stepsDone; }
uint8_t  StepperTim4_IsRunning(void)  { return s_run; }
