#include "App_LockerUnlock.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_MotorHoming.h"
#include "App_Locker.h"
#include "App_LockerOneShot.h"
#include "App_LockerSeek.h"
#include "App_NewPeriph_HL.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"
#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_CustomProtocol.h"
#include "App_RgbLed_Pattern.h"
#include <stddef.h>

/* =====================================================================
 * 多标签解锁 (LOCKER_SUB_UNLOCK_MULTI 0x0A) 实现 — Agent/Round_012/Plan.html
 * 与 0x08 单标签通道互斥 (双方前置检查 + 分发层白名单双保险)。
 * 灯语复用 RGBSRC_ONESHOT 槽位 (两流程互斥, 不新增仲裁源)。
 * ===================================================================== */

typedef struct {
    uint8_t  epc[UNLK_EPC_MAX];
    uint8_t  len;
    uint8_t  state;       /* UTAG_* */
    uint8_t  hits;
    uint32_t trackStart;  /* TRACKING 起点 (判据计时基准) */
    uint32_t lastSeen;    /* 最近一次读到 */
} UnlkTagRec_t;

#define UTAG_PENDING    0u
#define UTAG_TRACKING   1u
#define UTAG_CONFIRMED  2u   /* 单向掩码: 后续读到静默忽略 */

static volatile uint8_t s_busy;
static volatile uint8_t s_abort;
static volatile uint8_t s_immune;     /* 回退/回降段免疫 CANCEL */
static volatile uint8_t s_phase;
static uint8_t  s_channel;
static UnlkTagRec_t s_tag[UNLK_MAX_TAGS];
static uint8_t  s_total, s_confirmed, s_bitmap;
static uint8_t  s_softCnt, s_softDone;
static uint32_t s_irMs;               /* IR 触发时刻 (W 计时基准) */
static uint32_t s_demagBase;           /* 消磁计数基线 (softCnt>0) */
static uint8_t  s_amDemagOn;           /* 软标段已切消磁模式 (Run 出口切回检测) */
static uint8_t  s_risen;

uint8_t App_LockerUnlock_IsBusy(void) { return s_busy; }

void App_LockerUnlock_Abort(void) { if (s_busy) s_abort = 1u; }

void App_LockerUnlock_Finish(void)
{
    s_busy = 0u;
    s_abort = 0u;
    s_phase = UNLK_PH_NONE;
    s_immune = 0u;
    LockerSeek_BindAbort(NULL, NULL);     /* 解绑共享寻触的打断标志 */
    App_RgbLedPat_Clear(RGBSRC_ONESHOT);  /* 流程结束撤销灯语声明 */
}

void App_LockerUnlock_GetProgress(LockerUnlockProgress_t *p)
{
    if (!p) return;
    Memset8((void*)p, 0, sizeof(*p));
    p->phase = s_phase;
    if (!s_busy) return;
    p->total = s_total;
    p->confirmed = s_confirmed;
    p->confirmedBitmap = s_bitmap;
    p->softCnt = s_softCnt;
    p->softDone = s_softDone;
    if (s_irMs != 0u) {
        uint32_t h = SysTickHl_GetMs() - s_irMs;
        p->holdMs = (h > 0xFFFFu) ? 0xFFFFu : (uint16_t)h;
    }
    uint32_t now = SysTickHl_GetMs();
    for (uint8_t i = 0; i < s_total; i++) {
        if (s_tag[i].state == UTAG_TRACKING &&
            (now - s_tag[i].lastSeen) <= UNLK_PRESENT_WINDOW_MS) {
            p->tagPresent = 1u;
            break;
        }
    }
}

/* ---- 推送帧 (阻塞期间随时经原通道直发, func=FC_LOCKER_CTRL^0xFF) ---- */

static void push_start(uint32_t winMs)
{
    uint8_t r[6];
    r[0] = LOCKER_SUB_EVT_START;
    r[1] = UNLK_ERR_OK;
    r[2] = UNLK_PH_WAIT_TAG;
    r[3] = (uint8_t)(winMs & 0xFF);
    r[4] = (uint8_t)((winMs >> 8) & 0xFF);
    r[5] = (uint8_t)((winMs >> 16) & 0xFF);
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, sizeof(r));
}

/* 确认帧 0x0B: seq + EPC + 已确认/总数 + 判据耗时 + 流程耗时 */
static void push_tag(const UnlkTagRec_t *t)
{
    uint8_t r[3 + UNLK_EPC_MAX + 6];
    uint16_t pos = 0;
    uint32_t now = SysTickHl_GetMs();
    r[pos++] = LOCKER_SUB_EVT_TAG;
    r[pos++] = s_confirmed;               /* seq = 确认序号 (1..m) */
    r[pos++] = t->len;
    for (uint8_t i = 0; i < t->len; i++) r[pos++] = t->epc[i];
    r[pos++] = s_confirmed;
    r[pos++] = s_total;
    uint32_t c1 = now - t->trackStart;
    uint32_t c2 = now - s_irMs;
    uint16_t v1 = (c1 > 0xFFFFu) ? 0xFFFFu : (uint16_t)c1;
    uint16_t v2 = (c2 > 0xFFFFu) ? 0xFFFFu : (uint16_t)c2;
    r[pos++] = (uint8_t)(v1 & 0xFF); r[pos++] = (uint8_t)(v1 >> 8);
    r[pos++] = (uint8_t)(v2 & 0xFF); r[pos++] = (uint8_t)(v2 >> 8);
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, pos);
}

/* 失配事件帧 0x0C: 推帧不终止 (客户可能换正确标签, 窗口兜底) */
static void push_mismatch(const uint8_t *epc, uint8_t len, uint8_t rounds)
{
    if (len > UNLK_EPC_MAX) len = UNLK_EPC_MAX;
    uint8_t r[2 + UNLK_EPC_MAX + 1];
    uint16_t pos = 0;
    r[pos++] = LOCKER_SUB_EVT_MISMATCH;
    r[pos++] = len;
    for (uint8_t i = 0; i < len; i++) r[pos++] = epc[i];
    r[pos++] = rounds;
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, pos);
}

/* 硬标完成帧 0x0D (进入回降前) */
static void push_hard(uint8_t endReason)
{
    uint8_t r[8];
    uint32_t e = (s_irMs != 0u) ? (SysTickHl_GetMs() - s_irMs) : 0u;
    uint16_t v = (e > 0xFFFFu) ? 0xFFFFu : (uint16_t)e;
    r[0] = LOCKER_SUB_EVT_HARD_DONE;
    r[1] = endReason;
    r[2] = s_bitmap;
    r[3] = s_confirmed;
    r[4] = s_total;
    r[5] = (uint8_t)(v & 0xFF);
    r[6] = (uint8_t)(v >> 8);
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, 7);
}

/* 软标解码帧 0x0E */
static void push_soft(uint8_t done)
{
    uint8_t r[3];
    r[0] = LOCKER_SUB_EVT_SOFT;
    r[1] = done;
    r[2] = s_softCnt;
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, sizeof(r));
}

static void fill_busy(LockerUnlockResult_t *out)
{
    out->err          = UNLK_ERR_BUSY;
    out->lockerState  = (uint8_t)App_Locker_GetState();
    out->uhfState     = (uint8_t)App_UHF_GetState();
    out->stepperState = (uint8_t)App_Stepper_GetState();
}

/* 电机段失败统一出口 (同 OneShot one_motor_fail 语义) */
static void motor_fail(uint8_t err, uint8_t mphase, LockerUnlockResult_t *out)
{
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_TEST_FAST);   /* 安全回退: 黄快闪 */
    out->err        = err;
    out->motorPhase = mphase;
    out->fault      = App_Stepper_GetFault();
    out->diag1      = App_Stepper_GetDiag1();
    out->diag2      = App_Stepper_GetDiag2();
    out->steps      = App_Stepper_GetStepsDone();

    if (mphase == 2u) { out->retreat = UNLK_RETREAT_FAIL; return; }
    uint32_t dummy = 0u;
    s_immune = 1u;   /* 安全回退不受 CANCEL 打断 */
    out->retreat = (LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, &dummy) == 0)
                   ? UNLK_RETREAT_OK : UNLK_RETREAT_FAIL;
    s_immune = 0u;
}

/* 在清单记录中找该标签 (含已掩码; 未命中=外来) */
static UnlkTagRec_t *rec_find(const AppUHFTag_t *tag)
{
    for (uint8_t i = 0; i < s_total; i++) {
        UnlkTagRec_t *t = &s_tag[i];
        if (t->len != tag->epcLen) continue;
        uint8_t k;
        for (k = 0; k < t->len; k++)
            if (t->epc[k] != tag->epc[k]) break;
        if (k >= t->len) return t;
    }
    return NULL;
}

static void unlk_run(const uint8_t *epc, uint8_t epcLen, uint8_t epcCnt,
                     uint16_t tmoMs, uint16_t holdMaxMs, uint8_t softCnt,
                     uint8_t channel, LockerUnlockResult_t *out)
{
    if (!out || !epc || epcCnt == 0u || epcCnt > UNLK_MAX_TAGS ||
        epcLen == 0u || epcLen > UNLK_EPC_MAX) {
        if (out) { Memset8((void*)out, 0, sizeof(*out)); out->err = UNLK_ERR_PARAM; }
        return;
    }
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = UNLK_RETREAT_NONE;
    out->softCnt = softCnt;
    s_busy = 1u;
    s_abort = 0u;
    s_immune = 0u;
    s_phase = UNLK_PH_WAIT_TAG;
    s_channel = channel;
    s_total = epcCnt;
    s_confirmed = 0u;
    s_bitmap = 0u;
    s_softCnt = softCnt;
    s_softDone = 0u;
    s_risen = 0u;
    s_irMs = 0u;
    s_demagBase = 0u;
    s_amDemagOn = 0u;
    LockerSeek_BindAbort(&s_abort, &s_immune);
    for (uint8_t i = 0; i < epcCnt; i++) {
        s_tag[i].len = epcLen;
        s_tag[i].state = UTAG_PENDING;
        s_tag[i].hits = 0u;
        s_tag[i].trackStart = 0u;
        s_tag[i].lastSeen = 0u;
        for (uint8_t k = 0; k < epcLen; k++)
            s_tag[i].epc[k] = epc[(uint16_t)i * epcLen + k];
    }
    if (tmoMs == 0u) tmoMs = UNLK_INVENTORY_TMO_MS;
    if (tmoMs > 10000u) tmoMs = 10000u;

    /* ---- ⑴ 前置检查 ---- */
    if (App_LockerOneShot_IsBusy() != 0u)            { fill_busy(out); return; }
    if (App_Locker_IsIdle() == 0)                    { fill_busy(out); return; }
    if (App_UHF_GetState() == APP_UHF_SCAN)          { fill_busy(out); return; }
    if (App_UHF_IsBusy())                           { fill_busy(out); return; }
    if (App_Stepper_GetState() != APP_STEPPER_IDLE)  { fill_busy(out); return; }
    if (App_MotorHoming_IsReady() == 0) {
        out->err = UNLK_ERR_HOMING;
        out->switchErr = App_Stepper_GetSwitchErr();
        return;
    }

    /* ---- W 解锁窗: holdMaxMs=0 -> 公式, 非零 -> min(host, 上限) ---- */
    uint32_t winMs;
    if (holdMaxMs == 0u)
        winMs = UNLK_HOLD_BASE_MS + (uint32_t)(epcCnt - 1u) * UNLK_HOLD_EXTRA_PER_TAG_MS;
    else
        winMs = holdMaxMs;
    if (winMs > UNLK_HOLD_MAX_MS) winMs = UNLK_HOLD_MAX_MS;

    push_start(winMs);                       /* 受理帧 (含实际 W) */
    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_SCAN_WAIT);   /* 待放标: 青慢闪 */

    /* ---- ⑵ 光电门控: 等待放标 (PC4, 去抖) ---- */
    {
        uint32_t t0 = SysTickHl_GetMs();
        uint32_t irHighSince = 0u;
        for (;;) {
            LockerSeek_Pump();
            if (s_abort) { out->err = UNLK_ERR_OK; out->endReason = UNLK_END_ABORTED; return; }
            uint32_t now = SysTickHl_GetMs();
            if (App_NewPeriph_ReadIr() != 0u) {
                if (irHighSince == 0u) irHighSince = now;
                if ((now - irHighSince) >= UNLK_IR_CONFIRM_MS) break;   /* 放标触发 */
            } else {
                irHighSince = 0u;
            }
            if ((now - t0) >= winMs) { out->err = UNLK_ERR_NO_IR; return; }
        }
    }
    s_irMs = SysTickHl_GetMs();              /* W 计时基准 = IR 触发时刻 */

    /* ---- ⑶ 消磁准备: softCnt=0 跳过, 不触碰 AM ----
     * EPC 校验通过前 AM 只检测不消磁: 此处仅探链 + 强制检测模式
     * (防上次流程残留消磁模式在校对期误消软标); 消磁模式待硬标段
     * 结束进软标段 (⑦) 才切, 软标结束/窗满再切回检测模式。*/
    if (softCnt > 0u) {
        if (App_AM_Query() != APP_AM_ERR_OK) { out->err = UNLK_ERR_AM_LINK; return; }
        AppAMConfig_t amc;
        (void)App_AM_GetConfig(&amc);
        if (amc.mode != AM_MODE_DETECT_ONLY &&
            App_AM_SetParam(AM_CMD_MODE, AM_MODE_DETECT_ONLY) != APP_AM_ERR_OK) {
            out->err = UNLK_ERR_AM_LINK;
            return;
        }
    }

    /* ---- ⑷ UHF 就绪 ---- */
    s_phase = UNLK_PH_VERIFY;
    if (App_UHF_GetState() == APP_UHF_ERROR) {
        (void)App_UHF_Close();   /* ERROR 态: 彻底下电重上 */
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) { out->err = UNLK_ERR_UHF_OPEN; return; }
    } else if (App_UHF_GetState() != APP_UHF_READY) {
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) { out->err = UNLK_ERR_UHF_OPEN; return; }
    }
    App_UHF_ClearTags();                     /* 任务边界: 清陈旧标签 */
    if (s_abort) { out->err = UNLK_ERR_OK; out->endReason = UNLK_END_ABORTED; return; }

    /* ---- ⑸ 持续校对: 首确认升起, 逐张确认推帧+蜂鸣, n==m/窗满收尾 ---- */
    (void)App_Stepper_SetSpeedHz(LSEEK_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(LSEEK_TORQUE_PCT);
    (void)App_UHF_InventoryTimeout(tmoMs);

    uint8_t  roundExp = 0u, roundFgn = 0u;   /* 本盘点轮内读到清单/外来标签 */
    uint8_t  mismatchRounds = 0u, mismatchPushed = 0u;
    uint32_t lostStart = 0u;
    uint8_t  hardEnd = 0u;                    /* UNLK_END_* (0=未结束) */
    uint32_t rise = 0u, lower = 0u;

    while (hardEnd == 0u) {
        LockerSeek_Pump();
        if (s_abort) { hardEnd = UNLK_END_ABORTED; break; }

        uint32_t now = SysTickHl_GetMs();

        /* 取走本轮读到的标签, 更新各记录 */
        AppUHFTag_t tag;
        while (App_UHF_TagTake(&tag) == 0) {
            UnlkTagRec_t *t = rec_find(&tag);
            if (t != NULL) {
                if (t->state != UTAG_CONFIRMED) {   /* 已掩码: 静默忽略 */
                    if (t->state == UTAG_PENDING) {
                        t->state = UTAG_TRACKING;
                        t->trackStart = now;
                        t->hits = 0u;
                    }
                    t->hits++;
                    t->lastSeen = now;
                }
                roundExp = 1u;                      /* 清单标签在场 (含已掩码) */
            } else {
                roundFgn = 1u;
                if (out->tagsFound < 255u) out->tagsFound++;
                out->fgnEpcLen = (tag.epcLen > UNLK_EPC_MAX) ? UNLK_EPC_MAX : tag.epcLen;
                for (uint8_t k = 0; k < out->fgnEpcLen; k++) out->fgnEpc[k] = tag.epc[k];
            }
        }

        /* 在场判定 + 双门限确认 (3s 连续在场 + >=2 hits) */
        for (uint8_t i = 0; i < s_total; i++) {
            UnlkTagRec_t *t = &s_tag[i];
            if (t->state != UTAG_TRACKING) continue;
            if ((now - t->lastSeen) > UNLK_PRESENT_WINDOW_MS) {
                t->state = UTAG_PENDING;            /* 离场超窗: 重新累计 */
                t->hits = 0u;
                continue;
            }
            if (t->hits >= UNLK_CONFIRM_MIN_HITS &&
                (now - t->trackStart) >= UNLK_STABLE_CONFIRM_MS) {
                t->state = UTAG_CONFIRMED;          /* 单向掩码, 不再重报 */
                s_confirmed++;
                s_bitmap |= (uint8_t)(1u << i);
                push_tag(t);
                App_NewPeriph_BeepPulse(UNLK_BEEP_TAG_MS);   /* 每张确认: 短鸣 */
                App_RgbLedPat_Flash(RGBFLASH_MATCH_OK);
                if (!s_risen) {
                    /* 首确认: 升起 KEY_UP (模型甲: 保持至硬标段结束) */
                    s_risen = 1u;
                    s_phase = UNLK_PH_RISE;
                    App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_RISE_HOLD_GREEN);
                    int r = LockerSeek_RunRetry(LSEEK_DIR_UP, LSEEK_RISE_MAX, &rise);
                    if (r == 2u) {
                        hardEnd = UNLK_END_ABORTED;   /* 免疫回降交统一出口 */
                    } else if (r != 0u) {
                        motor_fail((App_Stepper_GetFault() != 0u)
                                   ? UNLK_ERR_MOTOR_FAULT : UNLK_ERR_MOTOR_TIMEOUT,
                                   1u, out);
                        return;
                    } else {
                        s_phase = UNLK_PH_VERIFY;
                    }
                    if (hardEnd != 0u) break;
                }
            }
        }
        if (hardEnd != 0u) break;

        if (s_confirmed >= s_total)      { hardEnd = UNLK_END_ALL_OK; break; }
        if ((now - s_irMs) >= winMs)     { hardEnd = UNLK_END_PARTIAL_TIMEOUT; break; }

        /* UHF 链路失联: 确认窗仍坏 -> 无法证实标签在场 */
        if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0) {
            if (lostStart == 0u) lostStart = now;
            if ((now - lostStart) >= UNLK_UHF_LOST_CONFIRM_MS) {
                hardEnd = UNLK_END_UHF_LOST;
                break;
            }
        } else {
            lostStart = 0u;
        }

        /* 一轮盘点结束 -> 失配结算 (轮口径, 非迭代口径) + 接力下一轮 */
        if (!App_UHF_IsBusy() && App_UHF_GetState() == APP_UHF_READY) {
            if (roundFgn && !roundExp) {
                if (!mismatchPushed &&
                    ++mismatchRounds >= UNLK_MISMATCH_CONFIRM_ROUNDS) {
                    mismatchPushed = 1u;          /* 一轮失配一帧, 再见清单标签后重新武装 */
                    push_mismatch(out->fgnEpc, out->fgnEpcLen, mismatchRounds);
                    App_RgbLedPat_Flash(RGBFLASH_MISMATCH_3S);
                }
            } else if (roundExp) {
                mismatchRounds = 0u;
                mismatchPushed = 0u;
            }
            roundExp = 0u;
            roundFgn = 0u;
            (void)App_UHF_InventoryTimeout(tmoMs);
        }
    }

    /* ---- ⑹ 硬标段收尾: 完成帧 + 回降 (磁块已升起时) ---- */
    push_hard(hardEnd);
    (void)App_UHF_Stop();
    out->endReason = hardEnd;
    out->confirmed = s_confirmed;
    out->total = s_total;
    out->confirmedBitmap = s_bitmap;

    if (s_risen) {
        s_phase = UNLK_PH_LOWER;
        App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_OFF);   /* 回降: 灭 */
        s_immune = 1u;   /* 回降本身即安全回退, 免疫打断 */
        int r = LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, &lower);
        s_immune = 0u;
        if (r != 0u) {
            motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                     : UNLK_ERR_MOTOR_TIMEOUT,
                       2u, out);
            return;
        }
        out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
        out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
    }

    if (hardEnd == UNLK_END_ABORTED) {
        out->err = UNLK_ERR_OK;
        out->endReason = UNLK_END_ABORTED;
        return;                    /* CANCEL: 跳过软标, 以 ABORTED 结账 */
    }

    /* ---- ⑺ 软标解码: 每次消磁成功计数推帧, 达标/窗满 (以实况结账) ---- */
    if (softCnt > 0u) {
        s_phase = UNLK_PH_SOFT;
        App_RgbLedPat_Set(RGBSRC_ONESHOT, RGBPAT_SOFT_WAIT_WHITE);   /* 软标: 白常亮 */
        /* EPC 校验已通过 (硬标段结束): 此刻才切消磁模式 + 记计数基线 */
        if (App_AM_SetParam(AM_CMD_MODE, AM_MODE_DEACTIVATE) != APP_AM_ERR_OK) {
            out->err = UNLK_ERR_AM_LINK;
            return;
        }
        s_amDemagOn = 1u;
        s_demagBase = App_AM_GetDeactCount();
        uint32_t t0 = SysTickHl_GetMs();
        while (1) {
            LockerSeek_Pump();
            if (s_abort) { out->err = UNLK_ERR_OK; out->endReason = UNLK_END_ABORTED; return; }
            uint32_t k = App_AM_GetDeactCount() - s_demagBase;
            if (k > 255u) k = 255u;
            if ((uint8_t)k > s_softDone) {
                uint8_t before = s_softDone;
                s_softDone = (uint8_t)k;
                for (uint8_t i = before + 1u; i <= s_softDone && i != 0u; i++)
                    push_soft(i);
                App_RgbLedPat_Flash(RGBFLASH_SOFT_OK);
            }
            if (s_softDone >= softCnt) break;
            if (SysTickHl_GetMs() - t0 >= UNLK_SOFT_WINDOW_MS) break;
        }
        out->softDone = s_softDone;
    }

    /* ---- ⑻ 结账完成 ---- */
    s_phase = UNLK_PH_DONE;
    if (softCnt == 0u || s_softDone >= softCnt) {
        App_NewPeriph_BeepPulse(UNLK_BEEP_DONE_MS);      /* 结账完成: 长鸣 300ms */
        App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);
    }
    out->err = UNLK_ERR_OK;
}

void App_LockerUnlock_Run(const uint8_t *epc, uint8_t epcLen, uint8_t epcCnt,
                          uint16_t tmoMs, uint16_t holdMaxMs, uint8_t softCnt,
                          uint8_t channel, LockerUnlockResult_t *out)
{
    if (out) {
        unlk_run(epc, epcLen, epcCnt, tmoMs, holdMaxMs, softCnt, channel, out);
        if (out->elapsedMs == 0u && s_irMs != 0u)
            out->elapsedMs = SysTickHl_GetMs() - s_irMs;
    }
    /* 软标段结束 (达标/窗满/任意出口): 切回 AM 检测模式, 不再消磁 */
    if (s_amDemagOn) {
        s_amDemagOn = 0u;
        (void)App_AM_SetParam(AM_CMD_MODE, AM_MODE_DETECT_ONLY);
    }
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), Finish 再做终清 */
    App_RgbLedPat_Clear(RGBSRC_ONESHOT);
}
