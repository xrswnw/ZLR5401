#include "App_MotorHoming.h"
#include "App_NewPeriph_HL.h"
#include "App_Stepper.h"
#include "App_SysTick_HL.h"
#include "App_RgbLed_Pattern.h"
#include "App_BootSelfTest.h"

/* =====================================================================
 * 上电行程自检 / 回零 — 后台状态机 (Round_098 #11: 原阻塞实现占住启动
 * 10~20s, 期间 USB 已枚举但协议无人服务; 改为非阻塞后复位重连从 ~14s
 * 降至数秒, 回零期间 MOVE/TEST 由分发层回 BUSY, 业务仍受 IsReady 门控)。
 *
 * 判据与原阻塞版一致:
 *   阶段1 向上找 KEY_UP  (上开关未触发才驱动; 已触发则跳过)
 *   阶段2 向下回 KEY_DOWN (建立下行程绝对基准)
 *   触到 -> DC 磁制动停位 + 清障; 堵转交 App_Stepper_Process 的 TRQ 监测
 *   (步进状态机由主循环推进, 本模块不再手动泵); 丢步停滞 400ms / 超步 /
 *   单腿 30s 超时 / 步进层故障 -> 该腿失败; 整体重试至多 3 次 (200ms
 *   间隔), 全败 -> switchErr(bit0=上 bit1=下) + 自检锁存位, 禁止 MOVE/TEST。
 * ===================================================================== */

#define HOMING_SPEED_HZ     1000u      /* 回零转速 — 2026-09-19 用户统一裁决:
                                         * 上电自测/行程测试/解锁腿全 1000 微步/s
                                         * (≈150RPM, 腿全程 ~4.2s; 1000=斜坡下限,
                                         * 起步即恒速无斜坡) */
#define HOMING_TORQUE_PCT   90u        /* 回零转矩 — 2026-09-19 现场堵转排查定稿值 90%
                                         * (阶梯标定: 20% 原地顶死/40% 中途顶住/60%、80%、90% 通过;
                                         * TRQ_COUNT 各档恒 4095, 过载监测失明, 兜底=步数上限) */
#define HOMING_DIR_DOWN     1u         /* dir=1 = 反转向下 (奔 KEY_DOWN) */
#define HOMING_DIR_UP       0u         /* dir=0 = 正转向上 (奔 KEY_UP) */
#define HOMING_DOWN_MAX     (4324u * 2u + 800u)   /* 下腿实测×2 + 裕量, 防无限冲底 */
#define HOMING_UP_MAX       (4085u * 2u + 800u)   /* 上腿实测×2 + 裕量 */
#define HOMING_TIMEOUT_MS   30000u     /* 单腿超时 */
#define HOMING_STALL_MS     400u       /* 步数停滞 (丢步兜底) */
#define HOMING_ATTEMPTS     3u         /* 整体重试次数 (上电首驱瞬态 nFAULT ~1/3) */
#define HOMING_RETRY_GAP_MS 200u       /* 重试间隔 */
#define HOMING_TRANSIENT_STEPS 200u     /* 瞬态误报判定: 走步少且故障寄存器非零。
                                         * 2026-09-19 黑匣子实测首驱瞬态死于 ~110 步
                                         * (runMs=100ms, fault=0xC0 FAULT|SPI_ERROR);
                                         * 死点随时基走步, 200 覆盖斜坡速率抖动。
                                         * 判线内走"立即同腿重试"(免 200ms 间隔),
                                         * 上电观感=一次连续上升, 消除"下坠-停住"抖动。 */

typedef enum {
    LG_NONE = 0,   /* 未启动 */
    LG_UP,         /* 阶段1: 向上找 KEY_UP */
    LG_DOWN,       /* 阶段2: 向下回 KEY_DOWN */
    LG_PAUSE,     /* 尝试间隔 */
    LG_DONE,       /* 成功 (回零完成) */
    LG_FAIL        /* 失败 (开关缺失/不可达) */
} HomingLeg_t;

static uint8_t  s_stat;        /* HOMING_STAT_* (对外状态) */
static uint8_t  s_leg;         /* HomingLeg_t (内部阶段) */
static uint8_t  s_attempt;     /* 已消耗尝试次数 */
static uint8_t  s_transTried;  /* 本腿瞬态重试已用 */
static uint32_t s_legT0;       /* 本腿起点 (超时基准) */
static uint32_t s_lastSeq;     /* 步数停滞基准 */
static uint32_t s_stallMs;     /* 步数停滞起点 */
static uint32_t s_pauseT0;     /* 尝试间隔起点 */

uint8_t App_MotorHoming_GetStatus(void) { return s_stat; }
int     App_MotorHoming_IsReady(void)   { return (s_stat == HOMING_STAT_READY) ? 1 : 0; }

static void leg_fail_(void);

static void leg_start(uint8_t dir)
{
    AppStepperMove_t mv;
    mv.dir = dir;
    mv.steps = 0u;                     /* 持续, 由触点/兜底停 */
    s_transTried = 0u;
    s_leg = (dir == HOMING_DIR_DOWN) ? LG_DOWN : LG_UP;
    s_legT0  = SysTickHl_GetMs();
    s_lastSeq = App_Stepper_GetStepsDone();
    s_stallMs = s_legT0;
    if (App_Stepper_Move(&mv) != 0) {
        leg_fail_();                   /* 启动即拒 (故障保持): 走失败路径 */
    }
}

/* 腿失败: 置对应开关错误位; 瞬态误报 (首驱 nFAULT, 走步<50 且寄存器非零)
 * 清障重试本腿一次; 否则计尝试, 未耗尽 200ms 后重开整轮, 耗尽判失败。 */
static void leg_fail_(void)
{
    App_Stepper_Stop();
    App_Stepper_SetSwitchErr((s_leg == LG_UP) ? 0x01u : 0x02u);
    if (s_transTried == 0u &&
        App_Stepper_GetStepsDone() < HOMING_TRANSIENT_STEPS &&
        App_Stepper_GetFault() != 0u) {
        s_transTried = 1u;
        (void)App_Stepper_ClearFault();       /* 同时清被瞬态污染的 switchErr */
        leg_start((s_leg == LG_UP) ? HOMING_DIR_UP : HOMING_DIR_DOWN);
        return;
    }
    if (s_attempt + 1u < HOMING_ATTEMPTS) {
        s_attempt++;
        s_leg = LG_PAUSE;
        s_pauseT0 = SysTickHl_GetMs();
        return;
    }
    s_stat = HOMING_STAT_FAILED;
    App_SelfTest_SetErrBits(SELF_ERR_TRAVEL_SW);
    App_RgbLedPat_Clear(RGBSRC_HOMING);
}

/* 尝试开始: 上开关已触发则跳过阶段1 (与原阻塞版一致) */
static void attempt_begin(void)
{
    if (App_NewPeriph_ReadKeyUp() != 0u) leg_start(HOMING_DIR_UP);
    else                                  leg_start(HOMING_DIR_DOWN);
}

void App_MotorHoming_Start(void)
{
    if (s_stat == HOMING_STAT_RUNNING || s_stat == HOMING_STAT_READY) return;
    s_stat = HOMING_STAT_RUNNING;
    s_attempt = 0u;
    (void)App_Stepper_SetSpeedHz(HOMING_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(HOMING_TORQUE_PCT);
    attempt_begin();
}

void App_MotorHoming_Process(void)
{
    if (s_stat != HOMING_STAT_RUNNING) return;

    App_RgbLedPat_Set(RGBSRC_HOMING, RGBPAT_HOMING_SLOW);   /* 回零: 黄慢闪 */

    if (s_leg == LG_PAUSE) {
        if ((SysTickHl_GetMs() - s_pauseT0) >= HOMING_RETRY_GAP_MS) {
            (void)App_Stepper_ClearFault();
            attempt_begin();
        }
        return;
    }
    if (s_leg != LG_UP && s_leg != LG_DOWN) return;

    /* 步进层故障 (TRQ 堵转停机已在 App_Stepper_Process 兜底) */
    if (App_Stepper_GetState() == APP_STEPPER_FAULT) { leg_fail_(); return; }

    uint32_t now = SysTickHl_GetMs();

    /* 到达目标行程开关: DC 磁制动停位 + 清障 */
    int hit = (s_leg == LG_DOWN) ? (App_NewPeriph_ReadKeyDown() == 0u)
                                : (App_NewPeriph_ReadKeyUp() == 0u);
    if (hit) {
        App_Stepper_DcBrakeStop();
        (void)App_Stepper_ClearFault();
        if (s_leg == LG_UP) {
            leg_start(HOMING_DIR_DOWN);   /* 阶段2: 向下建基准 */
        } else {
            s_leg = LG_DONE;
            s_stat = HOMING_STAT_READY;  /* 上下均验证: 已回零到下行程 */
            App_RgbLedPat_Clear(RGBSRC_HOMING);
        }
        return;
    }

    uint32_t nowSteps = App_Stepper_GetStepsDone();
    if (nowSteps != s_lastSeq) { s_lastSeq = nowSteps; s_stallMs = now; }

    /* 丢步停滞: 步数不进且未触开关 -> 该端不可达 */
    if ((now - s_stallMs) >= HOMING_STALL_MS)  { leg_fail_(); return; }
    /* 超限: 走了整程×2 仍未触发 -> 该端开关缺失/失效 */
    if (nowSteps >= ((s_leg == LG_UP) ? HOMING_UP_MAX : HOMING_DOWN_MAX)) {
        leg_fail_(); return;
    }
    /* 电机陷入 IDLE (TRQ 停机/外部停转): 该端不可达 */
    if (App_Stepper_GetState() == APP_STEPPER_IDLE) { leg_fail_(); return; }
    /* 单腿超时 */
    if ((now - s_legT0) >= HOMING_TIMEOUT_MS) { leg_fail_(); return; }
}
