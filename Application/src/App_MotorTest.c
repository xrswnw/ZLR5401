#include "App_MotorTest.h"
#include "App_Stepper.h"
#include "App_NewPeriph_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * 电机行程测试状态机.
 *  主循环节拍, 极性无关: 正转(或当前方向)运行中, 任一行程开关触发
 *  (连续 MT_DEBOUNCE_MS 保持) 即视为到达触点; 之后反转去触发另一
 *  触点构成 1 次往返. 电机故障或单程超时 -> FAULT.
 *  完成 N 次后自动停转 -> DONE -> (短暂保持不变可由 QUERY 读到) -> IDLE.
 * ===================================================================== */

#define MT_PASS_MIN   1u

static AppMotorTestState_t s_state = MT_STATE_IDLE;
static uint8_t  s_passTotal = 0;      /* 请求往返次数 */
static uint8_t  s_passDone  = 0;      /* 已完成往返次数 */
static uint8_t  s_dir       = 0;      /* 当前方向 (0=CW 正转, 1=CCW 反转) */
static uint8_t  s_trimmedVisited = 0; /* 本次半程是否已触发过触点 (消重触) */
static uint8_t  s_armed      = 1;     /* 触点可触发标志 (两触点均释放后置位) */
static uint32_t s_legStartMs = 0;     /* 本半程启动时刻 (单程超时基准) */
static uint32_t s_trigMs     = 0;     /* 触点开始持续的起始时刻 (防抖) */
static uint8_t  s_trigActive = 0;     /* 触点当前是否保持触发 (防抖锁存) */

/* 最高速 + 满转矩 */
static void set_fast(void)
{
    (void)App_Stepper_SetSpeedHz(2000u);
    (void)App_Stepper_SetTorquePercent(100u);
}

static void start_leg(uint8_t dir)
{
    AppStepperMove_t mv;
    s_dir = dir;
    s_trimmedVisited = 0;
    s_legStartMs = SysTickHl_GetMs();
    mv.dir = dir;
    mv.steps = 0u;                     /* 0 = 持续运行 */
    (void)App_Stepper_Move(&mv);
}

void App_MotorTest_Init(void)
{
    s_state = MT_STATE_IDLE;
    s_passTotal = 0; s_passDone = 0;
    s_dir = 0; s_trimmedVisited = 0; s_armed = 1;
    s_trigActive = 0; s_trigMs = 0;
}

int App_MotorTest_Start(uint8_t passes)
{
    if (passes < MT_PASS_MIN) return -2;               /* 参数非法 */
    if (s_state == MT_STATE_RUN) return -1;            /* 忙 */
    if (App_Stepper_GetState() == APP_STEPPER_FAULT) return -3;  /* 电机故障 */

    set_fast();
    s_passTotal = passes;
    s_passDone = 0;
    s_armed = 1;
    s_trigActive = 0;
    s_state = MT_STATE_RUN;
    start_leg(0u);                                     /* 先正转 */
    return 0;
}

int App_MotorTest_Stop(void)
{
    App_Stepper_Stop();
    s_passTotal = 0; s_passDone = 0;
    s_state = MT_STATE_IDLE;
    return 0;
}

int App_MotorTest_IsBusy(void) { return (s_state == MT_STATE_RUN) ? 1 : 0; }
uint8_t App_MotorTest_GetState(void) { return (uint8_t)s_state; }

void App_MotorTest_GetProgress(uint8_t *total, uint8_t *done)
{
    if (total) *total = s_passTotal;
    if (done)  *done  = s_passDone;
}

void App_MotorTest_Process(void)
{
    uint32_t now = SysTickHl_GetMs();

    if (s_state == MT_STATE_RUN)
    {
        /* 1) 电机故障 -> 停 + FAULT */
        if (App_Stepper_GetState() == APP_STEPPER_FAULT) {
            App_Stepper_Stop();
            s_state = MT_STATE_FAULT;
            return;
        }

        /* 2) 防抖采样: 任一触点触发 (高电平, 连续保持) */
        uint8_t anyHit = (App_NewPeriph_ReadKeyUp() != 0u) ||
                         (App_NewPeriph_ReadKeyDown() != 0u);
        if (anyHit) {
            if (!s_trigActive) { s_trigActive = 1; s_trigMs = now; }
            /* 3) 消重触: 本次半程触发过且未 re-arm, 忽略 */
            if (s_armed && !s_trimmedVisited &&
                (now - s_trigMs) >= MT_DEBOUNCE_MS) {
                s_trimmedVisited = 1;
                /* 完成本次行程: 一轮=正转(上)到达 + 反转(下)到达 */
                if (s_dir == 1u) {                  /* 本次为反转(第2半程) -> 完成1次往返 */
                    s_passDone++;
                    s_armed = 0;                    /* 等待两触点释放再 arm */
                    if (s_passDone >= s_passTotal) {
                        App_Stepper_Stop();
                        s_state = MT_STATE_DONE;
                        return;
                    }
                }
                start_leg((uint8_t)(s_dir ^ 1u));   /* 换向进入下一半程 */
                return;
            }
        } else {
            s_trigActive = 0;
            /* 两触点均释放 -> 允许下次触发 */
            if (!s_armed && (App_NewPeriph_ReadKeyUp() == 0u) &&
                             (App_NewPeriph_ReadKeyDown() == 0u))
                s_armed = 1;
        }

        /* 4) 单程超时 -> FAULT */
        if ((now - s_legStartMs) >= MT_TEST_TIMEOUT_MS) {
            App_Stepper_Stop();
            s_state = MT_STATE_FAULT;
            return;
        }
    }
}
