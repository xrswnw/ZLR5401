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
 * 定时器时钟: PCLK1=36MHz, APB1 预分频=2 (>1) -> TIM4 时钟 72MHz;
 *   PSC 两档换分频 (TIM_PSC_*): 1MHz/10kHz 计数,
 *   期望微步间隔 interval(us) -> ARR = interval/pscDiv - 1; CCR = ARR/2.
 *
 * 仅驱动 STEP 脉冲产生 + 步数计数 + 限步 + 斜坡调速(ISR 内按步数更新 ARR);
 * 方向(DIR)、使能(EN_OUT/ENABLE)、故障检测仍走 App_Stepper / DRV SDK.
 * ==================================================================== */

/* PSC 分频 (2026-09-19 修): 原实现 PSC=0 (72MHz 直数) 且 ARR 16 位封顶
 * 65535 -> 任何 <1099Hz 的请求被悄悄抬到 ~1099Hz (统一 1000 微步/s 裁决
 * 时黑匣子实测 1096 步/s 发现; 过载降速档 800 / MOVE 低速同源失真)。
 * 改两档 PSC: 1MHz 计数精确到 1us, 覆盖 >=15.26Hz (全部业务速度);
 * 更慢 (协议 MOVE 允许 1Hz) 用 10kHz 计数, 100us 粒度覆盖 0.16~15Hz。
 * PSC 在 Start 按本腿最慢间隔(=起步间隔, 斜坡只升频)一次性选定。 */
#define TIM_PSC_FAST   71u    /* 1MHz 计数:  ARR = iv_us - 1 */
#define TIM_PSC_SLOW   7199u  /* 10kHz 计数: ARR = iv_us/100 - 1 */

#define STEP_RAMP_STEPS   400u
#define STEP_RAMP_MIN_HZ  1000u

static volatile uint32_t  s_stepsDone = 0;
static volatile uint32_t  s_stepsReq  = 0;   /* 0=持续 */
static volatile uint8_t   s_run       = 0;
static volatile uint32_t  s_targetHz  = 0;
static volatile uint8_t   s_pscDiv    = 1u;  /* µs/计数tick: 1(快档) 或 100(慢档) */

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
            uint32_t arr = (iv / s_pscDiv) - 1UL;
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
    tb.TIM_Prescaler     = TIM_PSC_FAST;         /* 1MHz 计数 (Start 按速度换档) */
    tb.TIM_Period        = 999u;                 /* 默认 1000us/步 (1000 微步/s) */
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
    s_pscDiv = 1u;
}

/* 启动连续 STEP, 从斜坡起步转速开始, ISR 内升到 targetHz */
void StepperTim4_Start(uint32_t stepsReq, uint32_t targetHz)
{
    uint32_t iv = tim4_ramp_interval(0u, targetHz);
    /* 起步间隔即本腿最慢间隔 (斜坡只升频, 恒速腿全程不变) ->
     * 据此选 PSC 档: 1us 粒度覆盖 >=15.26Hz, 慢于此换 10kHz 档 */
    if (iv <= 65536u) {
        s_pscDiv = 1u;
        TIM_PrescalerConfig(TIM4, TIM_PSC_FAST, TIM_PSCReloadMode_Immediate);
    } else {
        s_pscDiv = 100u;
        TIM_PrescalerConfig(TIM4, TIM_PSC_SLOW, TIM_PSCReloadMode_Immediate);
    }
    uint32_t arr = (iv / s_pscDiv) - 1UL;
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
