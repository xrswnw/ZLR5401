#include "App_Led.h"
#include "App_Led_HL.h"
#include "App_SysTick_HL.h"

/* 硬件呼吸灯, 5s 一周期 (弱->强->弱), 持续.
 * 500 步 × 10ms = 5000ms: 前 250 步占空比 0->max (2.5s 弱->强),
 * 后 250 步 max->0 (2.5s 强->弱). 硬件 PWM (TIM3_CH1) 调光, 软件仅每 10ms 更新 CCR1.
 * 改用伽马²曲线 (占空比 ∝ 相位²). 人眼对亮度是对数感知, 线性 PWM 下
 * 中高段几乎看不出变化 (CCR 500->999 视觉差异极小), 低段又太快 -> "不明显".
 * 改 brightness = phase² × MAX 后, 低段慢启动、中段加速、峰值拉满, 人眼感知
 * 均匀渐亮渐暗, 呼吸效果明显. */
#define BREATH_PERIOD_MS  3000U
#define BREATH_STEP_MS    10U
#define BREATH_STEPS      (BREATH_PERIOD_MS / BREATH_STEP_MS)   /* 300*/
#define BREATH_HALF       (BREATH_STEPS / 2U)                   /* 150*/
#define BREATH_MAX        999U
#define BREATH_HALF_SQ    (BREATH_HALF * BREATH_HALF)           /* 62500, 伽马²分母*/

static uint32_t s_u32LastStep;
static uint16_t s_u16Step;

void AppLedInit(void) {
    s_u32LastStep = 0;
    s_u16Step = 0;
    LedHl_Init();
    LedHl_GSetBrightness(0);
}

/* 呼吸: 主循环调用, 软件伽马²斜坡驱动硬件 PWM 占空比*/
void AppLedProcess(void) {
    uint32_t now = SysTickHl_GetMs();
    if ((now - s_u32LastStep) < BREATH_STEP_MS) {
        return;
    }
    s_u32LastStep = now;

    s_u16Step = (uint16_t)((s_u16Step + 1U) % BREATH_STEPS);   /* 0..499 循环*/
    /* 伽马²: 上升 phase=step/HALF (0..1), 下降 phase=(STEPS-step)/HALF (1..0)
     * brightness = phase² × MAX. uint32 计算: step² × MAX / HALF² (step≤250,
     * 250²×999=62,437,500 < 2^32, 安全).*/
    uint32_t phase;
    if (s_u16Step < BREATH_HALF) {
        phase = (uint32_t)s_u16Step;                 /* 上升: 0..249*/
    } else {
        phase = (uint32_t)(BREATH_STEPS - s_u16Step); /* 下降: 250..1*/
    }
    uint32_t brightness = (phase * phase * BREATH_MAX) / BREATH_HALF_SQ;
    LedHl_GSetBrightness((uint16_t)brightness);
}

