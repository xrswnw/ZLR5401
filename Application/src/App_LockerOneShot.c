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
#include "App_LockerSeek.h"      /* 共享泵循环 + 寻触驱动 (Round_012 提取) */
#include "App_LockerUnlock.h"   /* 0x0A 多标签解锁流程 (互斥前置检查) */
#include <stddef.h>

/* =====================================================================
 * 单条同步开锁实现 — 设计依据: Agent/Round_074/Plan.html (v2)。
 * 寻触驱动的兜底判定 (停滞/超步/超时/瞬态重试) 与泵循环由
 * App_LockerSeek 提供 (与 App_MotorHoming 判据一致, 2000Hz / 40%)。
 * ===================================================================== */

/* ---- 流程中交互状态 (阻塞期间经 LockerSeek_Pump 内嵌 Proto_Poll 服务) ---- */
static volatile uint8_t  s_busy;         /* 流程进行中 */
static volatile uint8_t  s_abort;        /* CANCEL 打断请求 */
static volatile uint8_t  s_phase;        /* ONE_PH_* */
static volatile uint8_t  s_retreatImmune;/* 回退寻触不打断 (打断后的安全动作必须完成) */
static uint32_t s_holdStart;            /* 保持期起点 (ms) */
static uint32_t s_demagBase;             /* 消磁事件计数基线 */
static volatile uint8_t s_amDemagOn;    /* 已切消磁模式 (Run 出口切回检测) */
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
    LockerSeek_BindAbort(NULL, NULL);        /* 解绑共享寻触的打断标志 */
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

/* 泵循环与寻触驱动已提取至 App_LockerSeek (Round_012, 供 0x08/0x0A 共用)。 */

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
    out->retreat = (LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, &dummy) == 0)
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
                    uint16_t tmoMs, uint16_t irWaitMs, uint16_t holdMs,
                    uint8_t demagCnt, LockerOneShotResult_t *out);

void App_LockerOneShot_Run(const uint8_t *epc, uint8_t epcLen,
                           uint16_t tmoMs, uint16_t irWaitMs, uint16_t holdMs,
                           uint8_t demagCnt, LockerOneShotResult_t *out)
{
    /* Round_098 #21: 流程窗内强制会话 S0 (保持期持续盘点监控对 S2/S3
     * 同样敏感), 含全部提前失败出口统一还原. */
    (void)App_UHF_ScanSessionBegin();
    one_run(epc, epcLen, tmoMs, irWaitMs, holdMs, demagCnt, out);
    App_UHF_ScanSessionEnd();
    /* 流程结束 (含全部提前失败/打断出口): AM 切回检测模式, 不再消磁 */
    if (s_amDemagOn) {
        s_amDemagOn = 0u;
        (void)App_AM_SetParam(AM_CMD_MODE, AM_MODE_DETECT_ONLY);
    }
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), 交主循环仲裁器接管 */
    App_RgbLedPat_Clear(RGBSRC_ONESHOT);
}

static void one_run(const uint8_t *epc, uint8_t epcLen,
                    uint16_t tmoMs, uint16_t irWaitMs, uint16_t holdMs,
                    uint8_t demagCnt, LockerOneShotResult_t *out)
{
    if (!out || !epc) return;
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = ONE_RETREAT_NONE;
    out->demagCnt = demagCnt;
    s_busy = 1u;
    s_abort = 0u;
    s_phase = ONE_PH_PRECHK;
    s_retreatImmune = 0u;
    LockerSeek_BindAbort(&s_abort, &s_retreatImmune);   /* 共享寻触读本流程标志 */
    s_tagPresent = 0u;
    s_demagBase = 0xFFFFFFFFu;   /* 哨兵: 消磁未请求 */
    s_amDemagOn = 0u;
    s_progEpcLen = epcLen;
    for (uint8_t i = 0; i < epcLen; i++) s_progEpc[i] = epc[i];
    if (epcLen == 0u || epcLen > 12u) { out->err = ONE_ERR_PARAM; return; }
    if (tmoMs == 0u) tmoMs = 1000u;
    if (tmoMs > 10000u) tmoMs = 10000u;
    /* Round_098 #20: 原 maxHoldMs 身兼 IR 等待窗/校对预算/保持窗三职,
     * 拆为 irWaitMs (等待放标+标签出现窗) 与 holdMs (升起后保持窗). */
    if (irWaitMs == 0u) irWaitMs = ONE_IRWAIT_DEFAULT_MS;
    if (irWaitMs > ONE_IRWAIT_MAX_MS) irWaitMs = ONE_IRWAIT_MAX_MS;
    if (holdMs == 0u) holdMs = ONE_HOLD_DEFAULT_MS;
    if (holdMs > ONE_HOLD_MAX_MS) holdMs = ONE_HOLD_MAX_MS;

    /* ---- ⑴ 前置检查 ---- */
    if (App_LockerUnlock_IsBusy() != 0u)          { one_fill_busy(out); return; }
    if (App_Locker_IsIdle() == 0)                 { one_fill_busy(out); return; }
    if (App_UHF_GetState() == APP_UHF_SCAN)       { one_fill_busy(out); return; }
    if (App_UHF_IsBusy())                        { one_fill_busy(out); return; }
    if (App_Stepper_GetState() != APP_STEPPER_IDLE) { one_fill_busy(out); return; }
    if (App_MotorHoming_GetStatus() == HOMING_STAT_RUNNING) {
        one_fill_busy(out);            /* Round_098 #11: 后台回零进行中 -> BUSY */
        return;
    }
    if (App_MotorHoming_IsReady() == 0) {
        out->err = ONE_ERR_HOMING;
        out->switchErr = App_Stepper_GetSwitchErr();
        return;
    }

    /* ---- ⑴.5 光电门控: 等待客户放置标签 (PC4 高=检测到) ----
     * 连续 ONE_IR_CONFIRM_MS 高电平才算触发 (去抖); 等待窗 = irWaitMs
     * (Round_098 #20 拆分: 原 maxHoldMs 双语义之一),
     * 窗满未触发 -> NO_IR 失败 (不动磁块, 不碰 UHF/AM)。 */
    s_phase = ONE_PH_IR_WAIT;
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_IR_WAIT);   /* 等待放标: 白慢闪 (Round_011 A) */
    {
        uint32_t t0 = SysTickHl_GetMs();
        uint32_t irHighSince = 0u;
        for (;;) {
            LockerSeek_Pump();
            if (s_abort) { out->err = ONE_ERR_OK; out->endReason = ONE_END_ABORTED; return; }
            uint32_t now = SysTickHl_GetMs();
            if (App_NewPeriph_ReadIr() != 0u) {
                if (irHighSince == 0u) irHighSince = now;
                if ((now - irHighSince) >= ONE_IR_CONFIRM_MS) break;   /* 放标触发 */
            } else {
                irHighSince = 0u;
            }
            if ((now - t0) >= irWaitMs) {
                out->err = ONE_ERR_NO_IR;
                App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 未放标: 黄慢闪 2s (Round_011 D4) */
                return;
            }
        }
    }

    /* ---- ⑴.6 消磁准备: demagCnt=0 跳过消磁流程, 不触碰 AM ----
     * EPC 校验通过前 AM 只检测不消磁: 此处仅探链 + 强制检测模式
     * (防上次流程残留消磁模式在校对期误消软标); 消磁模式待期望 EPC
     * 命中后 (⑷ 升起前) 才切, 流程结束切回检测模式。*/
    uint32_t demagBase = 0u;
    if (demagCnt > 0u) {
        if (App_AM_Query() != APP_AM_ERR_OK) {
            out->err = ONE_ERR_AM_LINK;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 (Round_011 D4) */
            return;
        }
        AppAMConfig_t amc;
        (void)App_AM_GetConfig(&amc);
        if (amc.mode != AM_MODE_DETECT_ONLY &&
            App_AM_SetParam(AM_CMD_MODE, AM_MODE_DETECT_ONLY) != APP_AM_ERR_OK) {
            out->err = ONE_ERR_AM_LINK;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    }

    /* ---- ⑵ UHF 就绪 ---- */
    s_phase = ONE_PH_UHF_READY;
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_SCAN_ACTIVE);   /* 盘点校对中: 蓝慢闪 (Round_011 A) */
    if (App_UHF_GetState() == APP_UHF_ERROR) {
        /* ERROR 态(已上电但链路坏): Stop 不清 ERROR, 彻底下电重上 */
        (void)App_UHF_Close();
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = ONE_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    } else if (App_UHF_GetState() != APP_UHF_READY) {
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = ONE_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    }
    if (s_abort) { out->err = ONE_ERR_OK; out->endReason = ONE_END_ABORTED; return; }

    /* ---- ⑶ 持续校对: 反复盘点比对期望 EPC, 直至判定 ----
     * 单轮 1s 盘点 ~1/4 漏读属正常, 期望标签未读到时继续下一轮;
     * 读到标签但连续 ONE_MISMATCH_CONFIRM_ROUNDS 轮均无期望 EPC ->
     * MISMATCH (红闪不升起); 校对预算 (maxHoldMs) 内无任何标签 -> NO_TAG。 */
    s_phase = ONE_PH_INVENTORY;
    {
        uint32_t invT0 = SysTickHl_GetMs();
        uint8_t  mismatchRounds = 0u;
        uint8_t  matched = 0u;
        while (matched == 0u) {
            int r = App_UHF_InventorySync(tmoMs);
            if (r == APP_UHF_ERR_NO_TAG) {
                mismatchRounds = 0u;         /* 本轮无标签: 清失配计数, 继续轮 */
            } else if (r < 0) {
                out->err = ONE_ERR_UHF_LINK; out->uhfRawErr = r;
                App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
                return;
            } else {
                uint8_t roundTag = 0u;
                AppUHFTag_t tag;
                while (App_UHF_TagTake(&tag) == 0) {
                    roundTag = 1u;
                    out->tagsFound++;
                    App_RgbLedPat_Flash(RGBFLASH_TAG_SEEN);  /* 读到一张标签: 蓝单闪 (Round_011 A) */
                    if (one_epc_eq(epc, epcLen, tag.epc, tag.epcLen)) {
                        matched = 1u;
                    } else if (out->epcLen == 0u) {
                        /* 记录读到的第一张非期望 EPC 供 MISMATCH 诊断 */
                        out->epcLen = (tag.epcLen > 12u) ? 12u : (uint8_t)tag.epcLen;
                        for (uint8_t i = 0; i < out->epcLen; i++) out->epc[i] = tag.epc[i];
                    }
                }
                if (matched) break;
                if (roundTag && ++mismatchRounds >= ONE_MISMATCH_CONFIRM_ROUNDS) {
                    out->err = ONE_ERR_MISMATCH;
                    App_RgbLedPat_Flash(RGBFLASH_MISMATCH_3S);   /* 失配: RGB 红闪 3s */
                    return;
                }
            }
            LockerSeek_Pump();
            if (s_abort) { out->err = ONE_ERR_OK; out->endReason = ONE_END_ABORTED; return; }
            if ((SysTickHl_GetMs() - invT0) >= irWaitMs) {
                out->err = ONE_ERR_NO_TAG;
                out->uhfRawErr = APP_UHF_ERR_NO_TAG;
                App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 无标签收尾: 黄慢闪 2s (Round_011 D4) */
                return;
            }
        }
    }
    out->epcLen = epcLen;
    for (uint8_t i = 0; i < epcLen; i++) out->epc[i] = epc[i];
    if (s_abort) { out->err = ONE_ERR_OK; out->endReason = ONE_END_ABORTED; return; }

    /* 期望 EPC 已命中 (校验通过): 此刻才切 AM 消磁模式 + 记计数基线 */
    if (demagCnt > 0u) {
        if (App_AM_SetParam(AM_CMD_MODE, AM_MODE_DEACTIVATE) != APP_AM_ERR_OK) {
            out->err = ONE_ERR_AM_LINK;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 (Round_011 D4) */
            return;
        }
        s_amDemagOn = 1u;
        demagBase = App_AM_GetDeactCount();
        s_demagBase = demagBase;
    }

    /* ---- ⑷ 升起: 上行至 KEY_UP 触点 ---- */
    s_phase = ONE_PH_RISE;
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_RISE_HOLD_GREEN);   /* 升起: 绿常亮 */
    (void)App_Stepper_SetSpeedHz(LSEEK_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(LSEEK_TORQUE_PCT);
    {
        uint32_t rise = 0u;
        int r = LockerSeek_RunRetry(LSEEK_DIR_UP, LSEEK_RISE_MAX, &rise);
        if (r == 2) {
            /* CANCEL 打断: 免疫回退至 KEY_DOWN 后以 ABORTED 正常回帧 */
            uint32_t riseNow = App_Stepper_GetStepsDone();
            uint32_t lower = 0u;
            out->riseSteps = (riseNow > 0xFFFFu) ? 0xFFFFu : (uint16_t)riseNow;
            App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_OFF);   /* 正常回降: 灭 */
            s_retreatImmune = 1u;
            (void)LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, &lower);
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
        uint32_t stableSince = 0u;        /* EPC 连续在场起点 (0=需重新累计, demagCnt=0 判定用) */
        uint8_t  otherSeen = 0u;          /* 自上次见到期望标签以来读到过其他 EPC */
        s_phase = ONE_PH_HOLD;
        App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_SOFT_WAIT_WHITE);  /* 保持: 白常亮 */
        s_holdStart = holdStart;
        s_tagPresent = 1u;

        (void)App_UHF_Inventory();       /* 立即起一轮, 后续由泵循环接力 */
        while (1) {
            LockerSeek_Pump();

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

            /* EPC 稳定判定 (demagCnt=0): 升起后标签连续在场
             * ONE_STABLE_CONFIRM_MS -> 结账成功, 不等取走/窗满;
             * 中途离场 (防抖窗内未见) 则重置累计。 */
            if (demagCnt == 0u) {
                if (s_tagPresent) {
                    if (stableSince == 0u) stableSince = now;
                    if ((now - stableSince) >= ONE_STABLE_CONFIRM_MS) {
                        out->endReason = ONE_END_STABLE_OK;
                        break;
                    }
                } else {
                    stableSince = 0u;
                }
            }

            /* 保持窗超时 (Round_098 #20: 独立 holdMs, 原与 IR 等待窗共用) */
            if ((now - holdStart) >= holdMs) { out->endReason = ONE_END_HOLD_TIMEOUT; break; }

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
                    App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 (Round_011 D4) */
                    break;
                }
            } else {
                lostStart = 0u;
            }

            /* 一轮结束 (空闲) -> 接力下一轮盘点 */
            if (!App_UHF_IsBusy() && App_UHF_GetState() == APP_UHF_READY)
                (void)App_UHF_Inventory();
        }
        if (out->endReason == ONE_END_STABLE_OK)
            App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);   /* 结账成功: 绿三连闪 */
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
        int r = LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, &lower);
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
