#include "App_Led.h"
#include "App_Led_HL.h"
#include "App_NewPeriph_HL.h"
#include "App_SysTick_HL.h"

/* 硬件呼吸灯, 5s 一周期 (弱->强->弱), 持续.
 * 500 步 × 10ms = 5000ms: 前 250 步占空比 0->max (2.5s 弱->强),
 * 后 250 步 max->0 (2.5s 强->弱). 硬件 PWM (TIM2_CH3) 调光, 软件仅每 10ms 更新 CCR3.
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
    LedHl_RunSetBrightness(0);
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
    LedHl_RunSetBrightness((uint16_t)brightness);
}

/* =====================================================================
 * 按键/IR 诊断指示 (仅触发期间动作, 不锁存)
 * 行程开关低电平(按下)时, ERR 灯周期性亮灭; 松开即熄灭恢复正常:
 *   KEY_UP   按住 -> ERR 100ms 闪烁 (亮100/灭100)
 *   KEY_DOWN 按住 -> ERR 1000ms 闪烁 (亮1000/灭1000)
 * 以不同闪烁周期区分按下的是哪个; 仅在持续按下期间闪烁, 松开灭, 不保持.
 * IR (光电, PC4 高=检测到) 触发时 ERR 长亮.
 * 合并优先级: 行程闪烁 > IR 长亮 > 灭 (行程异常关电机安全, 闪烁更显眼,
 * 两类同时触发时保持闪烁周期可辨是哪个行程; 纯 IR 触发用长亮区分).
 * 用活动模式 s_keyMode 区分, 仅在模式切换时重置相位, 避免"按下不亮".
 * ===================================================================== */
#define KEY_BLINK_UP_MS    100u
#define KEY_BLINK_DOWN_MS  1000u
#define KEY_MODE_NONE      0u   /* 无触发 -> ERR 灭 (正常) */
#define KEY_MODE_UP        1u   /* 按住 UP -> 100ms 闪 */
#define KEY_MODE_DOWN      2u   /* 按住 DOWN -> 1000ms 闪 */
#define KEY_MODE_IR        3u   /* IR 触发 -> 长亮 */

static uint32_t s_keyBlinkLast;
static uint32_t s_keyBlinkPhaseMs;   /* 当前闪烁相位流逝时间 (周期取模) */
static uint8_t  s_keyMode;           /* 上一个活动模式 (用于检测切换) */

void AppLed_KeyBlinkProcess(void)
{
    uint32_t now     = SysTickHl_GetMs();
    uint8_t  keyUp   = App_NewPeriph_ReadKeyUp();    /* 1=非触发(高), 0=按下(低) */
    uint8_t  keyDown = App_NewPeriph_ReadKeyDown();
    uint8_t  upLow   = (keyUp == 0u);
    uint8_t  downLow = (keyDown == 0u);
    uint8_t  irOn    = App_NewPeriph_ReadIr();      /* 1=检测到红外/光电 */

    /* 判定当前模式: DOWN 优先于 UP (两键同按显示较慢的 1000ms);
     * 行程均松开时 IR 长亮; 全无 -> NONE. */
    uint8_t mode = downLow ? KEY_MODE_DOWN :
                   (upLow  ? KEY_MODE_UP   :
                   (irOn   ? KEY_MODE_IR   : KEY_MODE_NONE));

    /* 模式切换: 重置相位与计时基准 (从亮开始); 回 NONE 时熄灭 */
    if (mode != s_keyMode) {
        s_keyMode = mode;
        s_keyBlinkPhaseMs = 0u;
        s_keyBlinkLast = now;            /* 复位基准, 避免残留时间戳导致首拍跳变 */
        if (mode == KEY_MODE_NONE) {
            LedHl_EOff();                /* 松开: 恢复正常(灭) */
        } else {
            LedHl_EOn();                 /* 进入闪烁/长亮: 先亮 */
        }
    }

    /* KEY_MODE_NONE(全无触发) -> 恢复正常, 灯灭 */
    if (mode == KEY_MODE_NONE) {
        LedHl_EOff();
        return;
    }

    /* IR 长亮: 不参与相位翻转 */
    if (mode == KEY_MODE_IR) {
        LedHl_EOn();
        return;
    }

    /* 行程持续按下: 定时推进相位并按周期翻转. 每 ~10ms 一个 tick. */
    uint32_t dt = now - s_keyBlinkLast;
    if (dt >= 10u) {
        s_keyBlinkLast = now;
        s_keyBlinkPhaseMs += dt;
    }
    uint32_t period = (mode == KEY_MODE_DOWN) ? KEY_BLINK_DOWN_MS : KEY_BLINK_UP_MS;
    /* 亮一半周期, 灭一半周期: 每 period 翻转一次 */
    if ((s_keyBlinkPhaseMs / period) & 1u) {
        LedHl_EOff();
    } else {
        LedHl_EOn();
    }
}

/* 行程开关错误的黄/粉红 RGB 指示已迁移至 App_RgbLed_Pattern 仲裁器
 * (App_RgbLedPat_Tick 内 L1 优先级), 本文件不再直接驱动 RGB。 */


