#include "App_LockerOneShot.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_MotorHoming.h"
#include "App_Locker.h"
#include "App_NewPeriph_HL.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"
#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_CustomProtocol.h"   /* Memset8 */
#include "App_RgbLed_Pattern.h"  /* RGB 灯语 */
#include <stddef.h>

/* =====================================================================
 * 单条同步开锁实现 — 设计依据: Agent/Round_074/Plan.html (v2)。
 * 寻触驱动的兜底判定 (停滞/超步/超时/瞬态重试) 复用 App_MotorHoming
 * 已验证的模式与判据, 工况参数一致 (2000Hz / 40%)。
 * ===================================================================== */

#define ONE_DIR_UP      0u      /* dir=0 正转向上 (KEY_UP) */
#define ONE_DIR_DOWN    1u      /* dir=1 反转向下 (KEY_DOWN) */

/* 寻触段限值: 与回零一致 (实测行程x2 + 裕量, 防无限冲撞) */
#define ONE_RISE_MAX    (4085u * 2u + 800u)
#define ONE_LOWER_MAX   (4324u * 2u + 800u)
#define ONE_SEEK_TIMEOUT_MS   10000u
#define ONE_SEEK_STALL_MS     400u    /* 步数停滞判丢步 */
#define ONE_SEEK_SPEED_HZ     2000u
#define ONE_SEEK_TORQUE_PCT   40u

/* ---- 流程中交互状态 (阻塞期间经 one_pump 内嵌 Proto_Poll 服务) ---- */
static volatile uint8_t  s_busy;         /* 流程进行中 */
static volatile uint8_t  s_abort;        /* CANCEL 打断请求 */
static volatile uint8_t  s_phase;        /* ONE_PH_* */
static volatile uint8_t  s_retreatImmune;/* 回退寻触不打断 (打断后的安全动作必须完成) */
static uint32_t s_holdStart;            /* 保持期起点 (ms) */
static uint32_t s_demagBase;             /* 消磁事件计数基线 */
static volatile uint8_t s_tagPresent;   /* 保持期期望标签在场 */
static uint8_t s_progEpc[12];           /* 进度回显用 EPC 快照 */
static uint8_t s_progEpcLen;

uint8_t App_LockerOneShot_IsBusy(void) { return s_busy; }

void App_LockerOneShot_Abort(void) { if (s_busy) s_abort = 1u; }

void App_LockerOneShot_Finish(void)
{
    s_busy = 0u;
    s_abort = 0u;
    s_phase = ONE_PH_NONE;
    s_retreatImmune = 0u;
    App_RgbLedPat_Clear(RGBSRC_ONESHOT);   /* 流程结束撤销灯语声明 */
}

void App_LockerOneShot_GetProgress(LockerOneShotProgress_t *p)
{
    if (!p) return;
    Memset8((void*)p, 0, sizeof(*p));
    p->phase = s_phase;
    if (s_busy) {
        p->epcLen = s_progEpcLen;
        for (uint8_t i = 0; i < s_progEpcLen; i++) p->epc[i] = s_progEpc[i];
        p->steps = (uint16_t)(App_Stepper_GetStepsDone() & 0xFFFFu);
        if (s_phase == ONE_PH_HOLD) {
            uint32_t h = SysTickHl_GetMs() - s_holdStart;
            p->holdMs = (h > 0xFFFFu) ? 0xFFFFu : (uint16_t)h;
            p->tagPresent = s_tagPresent;
            if (s_demagBase != 0xFFFFFFFFu) {
                uint32_t d = App_AM_GetDeactCount() - s_demagBase;
                p->demagDone = (d > 255u) ? 255u : (uint8_t)d;
            }
        }
    }
}

/* 泵循环: 阻塞在主循环上下文时手动推进各状态机 + 喂狗 + 节拍。
 * TIM4(步进)/SysTick(ms)/USART(UHF+AM收帧)/USB 均为中断驱动, 不受阻塞影响。
 * 内嵌 Proto_Poll: 阻塞期间协议层继续收发, 分发层按互斥白名单放行
 * 进度查询/CANCEL/只读查询, 其余回 BUSY (防嵌套重入)。 */
static void one_pump(void)
{
    App_RgbLedPat_Tick();   /* 阻塞期间推进 RGB 灯语 (否则图样冻结) */
    App_UHF_Process();
    App_Stepper_Process();
    App_AM_Process();
    Proto_Poll();
    IwdgHl_Feed();
    SysTickHl_DelayMs(20u);
}

/* 朝 dir 方向持续驱动寻触点 (KEY_UP/KEY_DOWN)。0=已触到并停在该端;
 * 1=未触到 (停滞/超步/超时/被停机); 2=被 CANCEL 打断 (非回退段)。
 * 触到即 DC 磁制动停位并清障。s_retreatImmune 置位期间忽略打断 (安全回退必须完成)。
 * stepsOut: 返回该段已走微步数。 */
static int one_seek(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut)
{
    AppStepperMove_t mv;
    mv.dir = dir;
    mv.steps = 0u;                       /* 持续运行, 由触点/兜底停 */
    if (App_Stepper_Move(&mv) != 0) return 1;

    uint32_t t0      = SysTickHl_GetMs();
    uint32_t lastSeq = App_Stepper_GetStepsDone();
    uint32_t stallMs = t0;
    uint8_t  aborted = 0u;
    while (1) {
        one_pump();
        /* CANCEL 打断: 立即停机 (回退段免疫) */
        if (s_abort && !s_retreatImmune) { (void)App_Stepper_Stop(); aborted = 1u; break; }
        int hit = (dir == ONE_DIR_DOWN) ? (App_NewPeriph_ReadKeyDown() == 0u)
                                        : (App_NewPeriph_ReadKeyUp() == 0u);
        if (hit) {
            (void)App_Stepper_DcBrakeStop();
            (void)App_Stepper_ClearFault();
            if (stepsOut) *stepsOut = App_Stepper_GetStepsDone();
            return 0;
        }
        uint32_t now = SysTickHl_GetMs();
        uint32_t st  = App_Stepper_GetStepsDone();
        if (st != lastSeq) { lastSeq = st; stallMs = now; }
        /* 步数停滞: 电机不转 (完全丢步/被卡) */
        if ((now - stallMs) >= ONE_SEEK_STALL_MS) { (void)App_Stepper_Stop(); break; }
        /* 超步: 行程x2 仍未触 -> 触点缺失/失效 */
        if (st >= maxSteps)                  { (void)App_Stepper_Stop(); break; }
        /* 已被 Process 停机 (TRQ 堵转保护/DRV 故障/nFAULT) */
        if (App_Stepper_GetState() == APP_STEPPER_IDLE) { (void)App_Stepper_Stop(); break; }
        /* 超时 */
        if ((now - t0) >= ONE_SEEK_TIMEOUT_MS) { (void)App_Stepper_Stop(); break; }
    }
    if (stepsOut) *stepsOut = App_Stepper_GetStepsDone();
    return aborted ? 2 : 1;
}

/* 寻触 + 失败重试一次 (覆盖: 上电首驱 nFAULT 瞬态误停 [判据同回零] 与
 * 启动全失步 [实测脉冲走满整腿而机构未动, 紧随其后的同参数驱动即正常])。
 * 返回: 0=触到 / 1=未触到 / 2=被 CANCEL 打断。 */
static int one_seek_phase(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut)
{
    int r = one_seek(dir, maxSteps, stepsOut);
    if (r == 0) return 0;
    if (r == 2) return 2;
    (void)App_Stepper_ClearFault();
    r = one_seek(dir, maxSteps, stepsOut);
    if (r == 0) return 0;
    if (r == 2) return 2;
    return 1;
}

/* 电机段失败统一出口: 记录诊断字段; 磁块不在下端则安全回退 KEY_DOWN,
 * 回退结果记入 retreat。err 由调用方预填 (MOTOR_FAULT / MOTOR_TIMEOUT)。 */
static void one_motor_fail(uint8_t err, uint8_t phase, LockerOneShotResult_t *out)
{
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_TEST_FAST);   /* 安全回退: 黄快闪 */
    out->err    = err;
    out->phase  = phase;
    out->fault  = App_Stepper_GetFault();
    out->diag1  = App_Stepper_GetDiag1();
    out->diag2  = App_Stepper_GetDiag2();
    out->steps  = App_Stepper_GetStepsDone();

    /* 下段失败时已在回退动作中, 不再重复回退 */
    if (phase == 2u) { out->retreat = ONE_RETREAT_FAIL; return; }
    uint32_t dummy = 0u;
    s_retreatImmune = 1u;   /* 安全回退不受 CANCEL 打断 */
    out->retreat = (one_seek_phase(ONE_DIR_DOWN, ONE_LOWER_MAX, &dummy) == 0)
                   ? ONE_RETREAT_OK : ONE_RETREAT_FAIL;
    s_retreatImmune = 0u;
}

static void one_fill_busy(LockerOneShotResult_t *out)
{
    out->err          = ONE_ERR_BUSY;
    out->lockerState  = (uint8_t)App_Locker_GetState();
    out->uhfState     = (uint8_t)App_UHF_GetState();
    out->stepperState = (uint8_t)App_Stepper_GetState();
}

static uint8_t one_epc_eq(const uint8_t *want, uint8_t wantLen,
                          const uint8_t *got, uint8_t gotLen)
{
    if (wantLen != gotLen) return 0;
    for (uint8_t i = 0; i < wantLen; i++) if (want[i] != got[i]) return 0;
    return 1;
}

static void one_run(const uint8_t *epc, uint8_t epcLen,
                    uint16_t tmoMs, uint16_t maxHoldMs, uint8_t demagCnt,
                    LockerOneShotResult_t *out);

void App_LockerOneShot_Run(const uint8_t *epc, uint8_t epcLen,
                           uint16_t tmoMs, uint16_t maxHoldMs, uint8_t demagCnt,
                           LockerOneShotResult_t *out)
{
    one_run(epc, epcLen, tmoMs, maxHoldMs, demagCnt, out);
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), 交主循环仲裁器接管 */
    App_RgbLedPat_Clear(RGBSRC_ONESHOT);
}

static void one_run(const uint8_t *epc, uint8_t epcLen,
                    uint16_t tmoMs, uint16_t maxHoldMs, uint8_t demagCnt,
                    LockerOneShotResult_t *out)
{
    if (!out || !epc) return;
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = ONE_RETREAT_NONE;
    out->demagCnt = demagCnt;
    s_busy = 1u;
    s_abort = 0u;
    s_phase = ONE_PH_PRECHK;
    s_retreatImmune = 0u;
    s_tagPresent = 0u;
    s_demagBase = 0xFFFFFFFFu;   /* 哨兵: 消磁未请求 */
    s_progEpcLen = epcLen;
    for (uint8_t i = 0; i < epcLen; i++) s_progEpc[i] = epc[i];
    if (epcLen == 0u || epcLen > 12u) { out->err = ONE_ERR_PARAM; return; }
    if (tmoMs == 0u) tmoMs = 1000u;
    if (tmoMs > 10000u) tmoMs = 10000u;
    if (maxHoldMs == 0u) maxHoldMs = ONE_HOLD_DEFAULT_MS;
    if (maxHoldMs > ONE_HOLD_MAX_MS) maxHoldMs = ONE_HOLD_MAX_MS;

    /* ---- ⑴ 前置检查 ---- */
    if (App_Locker_IsIdle() == 0)                 { one_fill_busy(out); return; }
    if (App_UHF_GetState() == APP_UHF_SCAN)       { one_fill_busy(out); return; }
    if (App_UHF_IsBusy())                        { one_fill_busy(out); return; }
    if (App_Stepper_GetState() != APP_STEPPER_IDLE) { one_fill_busy(out); return; }
    if (App_MotorHoming_IsReady() == 0) {
        out->err = ONE_ERR_HOMING;
        out->switchErr = App_Stepper_GetSwitchErr();
        return;
    }

    /* ---- ⑴.5 消磁准备: demagCnt=0 跳过消磁流程, 不触碰 AM ---- */
    uint32_t demagBase = 0u;
    if (demagCnt > 0u) {
        if (App_AM_Query() != APP_AM_ERR_OK) { out->err = ONE_ERR_AM_LINK; return; }
        AppAMConfig_t amc;
        (void)App_AM_GetConfig(&amc);
        if (amc.mode != AM_MODE_DEACTIVATE &&
            App_AM_SetParam(AM_CMD_MODE, AM_MODE_DEACTIVATE) != APP_AM_ERR_OK) {
            out->err = ONE_ERR_AM_LINK;
            return;
        }
        demagBase = App_AM_GetDeactCount();
        s_demagBase = demagBase;
    }

    /* ---- ⑵ UHF 就绪 ---- */
    s_phase = ONE_PH_UHF_READY;
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_SCAN_WAIT);   /* 扫描等待: 青慢闪 */
    if (App_UHF_GetState() == APP_UHF_ERROR) {
        /* ERROR 态(已上电但链路坏): Stop 不清 ERROR, 彻底下电重上 */
        (void)App_UHF_Close();
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) { out->err = ONE_ERR_UHF_OPEN; return; }
    } else if (App_UHF_GetState() != APP_UHF_READY) {
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) { out->err = ONE_ERR_UHF_OPEN; return; }
    }
    if (s_abort) { out->err = ONE_ERR_OK; out->endReason = ONE_END_ABORTED; return; }

    /* ---- ⑶ 初始同步盘点 + 单标签判断 ---- */
    s_phase = ONE_PH_INVENTORY;
    {
        int r = App_UHF_InventorySync(tmoMs);
        if (r == APP_UHF_ERR_NO_TAG || r == 0) { out->err = ONE_ERR_NO_TAG; out->uhfRawErr = r; return; }
        if (r < 0) { out->err = ONE_ERR_UHF_LINK; out->uhfRawErr = r; return; }

        uint8_t matched = 0u;
        AppUHFTag_t tag;
        while (App_UHF_TagTake(&tag) == 0) {
            out->tagsFound++;
            if (!matched && one_epc_eq(epc, epcLen, tag.epc, tag.epcLen)) {
                matched = 1u;
            } else if (out->epcLen == 0u) {
                /* 记录读到的第一张非期望 EPC 供 MISMATCH 诊断 */
                out->epcLen = (tag.epcLen > 12u) ? 12u : tag.epcLen;
                for (uint8_t i = 0; i < out->epcLen; i++) out->epc[i] = tag.epc[i];
            }
        }
        if (!matched) {
            out->err = ONE_ERR_MISMATCH;
            App_RgbLedPat_Flash(RGBFLASH_MISMATCH_3S);   /* 失配: RGB 红闪 3s */
            return;
        }
    }
    out->epcLen = epcLen;
    for (uint8_t i = 0; i < epcLen; i++) out->epc[i] = epc[i];
    if (s_abort) { out->err = ONE_ERR_OK; out->endReason = ONE_END_ABORTED; return; }

    /* ---- ⑷ 升起: 上行至 KEY_UP 触点 ---- */
    s_phase = ONE_PH_RISE;
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_RISE_HOLD_GREEN);   /* 升起: 绿常亮 */
    (void)App_Stepper_SetSpeedHz(ONE_SEEK_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(ONE_SEEK_TORQUE_PCT);
    {
        uint32_t rise = 0u;
        int r = one_seek_phase(ONE_DIR_UP, ONE_RISE_MAX, &rise);
        if (r == 2) {
            /* CANCEL 打断: 免疫回退至 KEY_DOWN 后以 ABORTED 正常回帧 */
            uint32_t riseNow = App_Stepper_GetStepsDone();
            uint32_t lower = 0u;
            out->riseSteps = (riseNow > 0xFFFFu) ? 0xFFFFu : (uint16_t)riseNow;
            App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_OFF);   /* 正常回降: 灭 */
            s_retreatImmune = 1u;
            (void)one_seek_phase(ONE_DIR_DOWN, ONE_LOWER_MAX, &lower);
            s_retreatImmune = 0u;
            out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
            out->err = ONE_ERR_OK;
            out->endReason = ONE_END_ABORTED;
            return;
        }
        if (r != 0) {
            uint8_t merr = (App_Stepper_GetFault() != 0u) ? ONE_ERR_MOTOR_FAULT
                                                          : ONE_ERR_MOTOR_TIMEOUT;
            one_motor_fail(merr, 1u, out);
            return;
        }
        out->riseSteps = (rise > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
    }

    /* ---- ⑸ 保持: UHF 持续盘点在线监控 ---- */
    {
        uint32_t holdStart = SysTickHl_GetMs();
        uint32_t lastSeen  = holdStart;   /* 期望 EPC 最近被读到时刻 */
        uint32_t lostStart = 0u;          /* UHF 链路异常起始时刻 (0=正常) */
        uint8_t  otherSeen = 0u;          /* 自上次见到期望标签以来读到过其他 EPC */
        s_phase = ONE_PH_HOLD;
        App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_SOFT_WAIT_WHITE);  /* 保持: 白常亮 */
        s_holdStart = holdStart;
        s_tagPresent = 1u;

        (void)App_UHF_Inventory();       /* 立即起一轮, 后续由泵循环接力 */
        while (1) {
            one_pump();

            /* CANCEL 打断: 与其他结束原因同级, 走正常回降 */
            if (s_abort) { out->endReason = ONE_END_ABORTED; break; }

            /* 取走本轮读到的标签, 更新在场判定 */
            AppUHFTag_t tag;
            while (App_UHF_TagTake(&tag) == 0) {
                if (one_epc_eq(epc, epcLen, tag.epc, tag.epcLen)) {
                    lastSeen = SysTickHl_GetMs();
                    otherSeen = 0u;
                } else {
                    otherSeen = 1u;
                }
            }

            uint32_t now = SysTickHl_GetMs();
            s_tagPresent = ((now - lastSeen) < ONE_REMOVED_CONFIRM_MS) ? 1u : 0u;

            /* 消磁完成: 成功消磁事件数达标 (demagCnt=0 时恒跳过, 不触碰 AM) */
            if (demagCnt > 0u &&
                (App_AM_GetDeactCount() - demagBase) >= demagCnt) {
                out->endReason = ONE_END_DEMAG_DONE;
                break;
            }

            /* 保持窗超时 (上位机给的最坏期限) */
            if ((now - holdStart) >= maxHoldMs) { out->endReason = ONE_END_HOLD_TIMEOUT; break; }

            /* 期望标签稳定移除: 防抖确认窗内再未读到 */
            if ((now - lastSeen) >= ONE_REMOVED_CONFIRM_MS) {
                out->endReason = otherSeen ? ONE_END_TAG_CHANGED : ONE_END_TAG_REMOVED;
                break;
            }

            /* UHF 链路失联: 异常持续确认窗仍坏 -> 无法证实标签在场 */
            if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0) {
                if (lostStart == 0u) lostStart = now;
                if ((now - lostStart) >= ONE_UHF_LOST_CONFIRM_MS) {
                    out->endReason = ONE_END_UHF_LOST;
                    break;
                }
            } else {
                lostStart = 0u;
            }

            /* 一轮结束 (空闲) -> 接力下一轮盘点 */
            if (!App_UHF_IsBusy() && App_UHF_GetState() == APP_UHF_READY)
                (void)App_UHF_Inventory();
        }
        (void)App_UHF_Stop();
        if (demagCnt > 0u) {
            uint32_t done = App_AM_GetDeactCount() - demagBase;
            out->demagDone = (done > 255u) ? 255u : (uint8_t)done;
        }
    }

    /* ---- ⑹ 回降: 下行回退至 KEY_DOWN 触点 (打断免疫: 本身即安全回退) ---- */
    s_phase = ONE_PH_LOWER;
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_OFF);   /* 回降: 灭 */
    {
        uint32_t lower = 0u;
        s_retreatImmune = 1u;
        int r = one_seek_phase(ONE_DIR_DOWN, ONE_LOWER_MAX, &lower);
        s_retreatImmune = 0u;
        if (r != 0) {
            uint8_t merr = (App_Stepper_GetFault() != 0u) ? ONE_ERR_MOTOR_FAULT
                                                          : ONE_ERR_MOTOR_TIMEOUT;
            one_motor_fail(merr, 2u, out);
            return;
        }
        out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
    }

    /* ---- ⑺ 完成 ---- */
    out->err = ONE_ERR_OK;
}
