#include "App_MotorTest.h"
#include "App_Stepper.h"
#include "App_NewPeriph_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * 电机行程测试状态机.
 *  主循环节拍, 极性相关: dir=0 正转=向上触上行程 KEY_UP,
 *  dir=1 反转=向下触下行程 KEY_DOWN (均已确认接线极性).
 *  正转段只认 KEY_UP、反转段只认 KEY_DOWN 为有效触发; 若在正转段错触
 *  KEY_DOWN 或在反转段错触 KEY_UP(连续防抖时长) -> FAULT, 暴露方向/接线错.
 *  电机故障或单程超时 -> FAULT. 完成 N 次后自动停转 -> DONE -> IDLE.
 * ===================================================================== */

#define MT_PASS_MIN   1u

/* 测试故障/停机原因(诊断, 经 QUERY diag1 上报): 0=无/未判 */
#define MT_FR_NONE      0
#define MT_FR_STP_FAULT 1   /* 步进层故障 (nFAULT/寄存器) */
#define MT_FR_ESCAPE_MS 2   /* 挣脱被压住离出触点超时 */
#define MT_FR_WRONG     3   /* 方向极性错触 */
#define MT_FR_EXPECT_MS 4   /* 期望触点单程超时 */

static AppMotorTestState_t s_state = MT_STATE_IDLE;
static uint8_t s_faultReason = MT_FR_NONE;
static uint8_t  s_passTotal = 0;      /* 请求往返次数 */
static uint8_t  s_passDone  = 0;      /* 已完成往返次数 */
static uint8_t  s_dir       = 0;      /* 当前方向 (0=正转向上, 1=反转向下) */
static uint8_t  s_trimmedVisited = 0; /* 本次半程是否已触发过触点 (消重触) */
static uint8_t  s_armed      = 1;     /* 触点可触发标志 (两触点均释放后置位) */
static uint32_t s_legStartMs = 0;     /* 本半程启动时刻 (单程超时基准) */
static uint32_t s_trigMs     = 0;     /* 触点开始持续的起始时刻 (防抖) */
static uint8_t  s_trigActive = 0;     /* 触点当前是否保持触发 (防抖锁存) */
static uint32_t s_wrongMs    = 0;     /* 错触触点持续起始时刻 (防抖) */
static uint8_t  s_wrongActive = 0;    /* 错触触点当前是否保持触发 */
static uint8_t  s_brokeAway  = 1;     /* 已脱离起点进入自由行程 (两触点均释放过) */

/* 行程测试速度: 目标 1500 微步/s = 7.5 转/s = 450 RPM. 直接以全速起步会致机构从
 * 静止点回弹误触下行程开关(方向错触); 起步由 App_Stepper 加速斜坡(全步1圈)平缓提速,
 * 可干净越过静止点. 转矩保持满刻度保证带载触碰. */
/* 行程测试速度=1000 微步/s = 5 转/s = 300 RPM: 预留振动余量, 最稳.
 * 加速斜坡(全步1圈)平缓越过静止点. 更高(1200/1500)在高速振动下会回弹误触下行程开关. */
#define MT_TEST_SPEED_HZ    1000u
#define MT_TEST_TORQUE_PCT  100u

/* 测试速度1000(300RPM, 实测干净上限) + 满转矩(保证带载触碰).
 * 已实测: 1500(450RPM)高速机械振动回弹误触下行程开关, 与转矩无关
 * (100%/50%均失败且无失速 diag2=0); 1200 带斜坡可通过, 1000 最稳且留余量. */
static void set_test_params(void)
{
    (void)App_Stepper_SetSpeedHz(MT_TEST_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(MT_TEST_TORQUE_PCT);
}

static void start_leg(uint8_t dir)
{
    AppStepperMove_t mv;
    s_dir = dir;
    s_trimmedVisited = 0;
    s_wrongActive = 0;
    s_legStartMs = SysTickHl_GetMs();
    /* 警戒线防护: 上行程 KEY_UP 是正向终点/警戒线, 绝不允许"正向越过"它.
     * 若启动正程(dir=0)时上行程已被压住(已在警戒线), 不得再正向驱动,
     * 直接判 FAULT, 防止电机顶着/跳过警戒线继续正向. */
    if (dir == 0u && App_NewPeriph_ReadKeyUp() == 0u) {
        App_Stepper_Stop();
        s_faultReason = MT_FR_WRONG;
        s_state = MT_STATE_FAULT;
        return;
    }
    /* 若段起点两触点均已释放(未按下), 视为已进入自由行程; 否则起始被压住的
     * 触点(如机构停在下触点)是离出位置, 需等电机挣脱后才判极性错触. */
    s_brokeAway = ((App_NewPeriph_ReadKeyUp() == 0u) &&
                   (App_NewPeriph_ReadKeyDown() == 0u)) ? 1u : 0u;
    mv.dir = dir;
    mv.steps = 0u;                     /* 0 = 持续运行 */
    (void)App_Stepper_Move(&mv);
}

void App_MotorTest_Init(void)
{
    s_state = MT_STATE_IDLE;
    s_faultReason = MT_FR_NONE;
    s_passTotal = 0; s_passDone = 0;
    s_dir = 0; s_trimmedVisited = 0; s_armed = 1;
    s_trigActive = 0; s_trigMs = 0; s_wrongActive = 0; s_wrongMs = 0;
    s_brokeAway = 1;
}

int App_MotorTest_Start(uint8_t passes)
{
    if (passes < MT_PASS_MIN) return -2;               /* 参数非法 */
    if (s_state == MT_STATE_RUN) return -1;            /* 忙 */
    if (App_Stepper_GetState() == APP_STEPPER_FAULT) return -3;  /* 电机故障 */

    set_test_params();
    s_passTotal = passes;
    s_passDone = 0;
    s_armed = 1;
    s_trigActive = 0; s_wrongActive = 0;
    s_state = MT_STATE_RUN;
    start_leg(0u);                                     /* 先正转向上 */
    return 0;
}

int App_MotorTest_Stop(void)
{
    App_Stepper_Stop();
    s_passTotal = 0; s_passDone = 0;
    s_faultReason = MT_FR_NONE;
    s_state = MT_STATE_IDLE;
    return 0;
}

int App_MotorTest_IsBusy(void) { return (s_state == MT_STATE_RUN) ? 1 : 0; }
uint8_t App_MotorTest_GetState(void) { return (uint8_t)s_state; }
uint8_t App_MotorTest_GetFaultReason(void) { return s_faultReason; }

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
            s_faultReason = MT_FR_STP_FAULT;
            s_state = MT_STATE_FAULT;
            return;
        }

        uint8_t upHit   = (App_NewPeriph_ReadKeyUp() == 0u);   /* 按下(低)=触上行程 */
        uint8_t downHit = (App_NewPeriph_ReadKeyDown() == 0u); /* 按下(低)=触下行程 */

        /* 仍未脱离起点(两触点均未释放过): 等待电机挣脱被压住的离出触点.
         * 只有已进入自由行程后才开始判方向极性错触与期望触点. */
        if (!s_brokeAway) {
            if (!upHit && !downHit) s_brokeAway = 1;
            else {
                s_trigActive = 0; s_wrongActive = 0;
            }
            if ((now - s_legStartMs) >= MT_TEST_TIMEOUT_MS) {   /* 挣脱超时 */
                App_Stepper_Stop();
                s_faultReason = MT_FR_ESCAPE_MS;
                s_state = MT_STATE_FAULT;
                return;
            }
            return;
        }

        /* 4) 校验方向极性: 正转段应触上行程, 反转段应触下行程;
         *    自由行程中在正转段错误触到 KEY_DOWN 或在反转段错误触到
         *    KEY_UP (连续保持) -> 判定方向/接线异常 -> FAULT. */
        uint8_t wrongHit = (s_dir == 0u) ? downHit : upHit;
        if (wrongHit) {
            if (!s_wrongActive) { s_wrongActive = 1; s_wrongMs = now; }
            if ((now - s_wrongMs) >= MT_DEBOUNCE_MS) {
                App_Stepper_Stop();
                s_faultReason = MT_FR_WRONG;
                s_state = MT_STATE_FAULT;
                return;
            }
        } else {
            s_wrongActive = 0;
        }

        /* 5) 期望触点的防抖采样 (本次行程应到达的触点) */
        uint8_t expectHit = (s_dir == 0u) ? upHit : downHit;
        if (expectHit) {
            if (!s_trigActive) { s_trigActive = 1; s_trigMs = now; }
            /* 消重触: 本次半程触发过且未 re-arm, 忽略 */
            if (s_armed && !s_trimmedVisited &&
                (now - s_trigMs) >= MT_DEBOUNCE_MS) {
                s_trimmedVisited = 1;
                /* 警戒线硬停: 正程(dir=0)触到上行程 KEY_UP 即警戒线, 立即停,
                 * 绝不在正向再走一步, 再换向反转. DC磁制动抗惯性滑行减超程. */
                if (s_dir == 0u) {
                    App_Stepper_DcBrakeStop();
                }
                /* 反转段(第2半程)到达 -> 完成1次往返 */
                if (s_dir == 1u) {
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
            if (!s_armed && !upHit && !downHit)
                s_armed = 1;
        }

        /* 6) 单程超时 -> FAULT */
        if ((now - s_legStartMs) >= MT_TEST_TIMEOUT_MS) {
            App_Stepper_Stop();
            s_faultReason = MT_FR_EXPECT_MS;
            s_state = MT_STATE_FAULT;
            return;
        }
    }
}
