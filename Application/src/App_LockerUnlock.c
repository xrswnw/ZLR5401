#include "App_LockerUnlock.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_MotorHoming.h"
#include "App_Locker.h"
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
 * 解锁流程 (LOCKER_SUB_UNLOCK_MULTI 0x0A) 实现 — Agent/Round_012/Plan.html
 * Round_011 用户裁决: 0x08 单标签流程已废除, 本流程为唯一开锁通道
 * (epcCnt=1 即单标)。灯语独占 RGBSRC_UNLOCK 槽位。
 * Round_013 用户裁决: 0x0A 分发层注释关闭 (回 PARAM, 实现保留),
 * 解锁业务改走 0x10 UNLOCK_EPC (App_LockerUnlock_EpcRun, 文件末尾);
 * 灯语槽位与 UNLK_ERR_* 错误码移交新流程。
 * ===================================================================== */

typedef struct {
    uint8_t  epc[UNLK_EPC_MAX];
    uint8_t  len;
    uint8_t  state;       /* UTAG_* */
    uint8_t  hits;
    uint32_t trackStart;  /* TRACKING 起点 (判据计时基准) */
    uint32_t lastSeen;    /* 最近一次读到 */
} UnlkTagRec_t;

/* 外来标签记录 (裁决3): 独立缓存多张, 稳定判据同期望标签,
 * 稳定一张推一帧失配 (reported 单向掩码不重报); len=0 空槽。 */
typedef struct {
    uint8_t  epc[UNLK_EPC_MAX];
    uint8_t  len;
    uint8_t  hits;
    uint8_t  reported;
    uint32_t trackStart;
    uint32_t lastSeen;
} FgnTagRec_t;

#define UTAG_PENDING    0u
#define UTAG_TRACKING   1u
#define UTAG_CONFIRMED  2u   /* 单向掩码: 后续读到静默忽略 */

static volatile uint8_t s_busy;
static volatile uint8_t s_abort;
static volatile uint8_t s_immune;     /* 回退/回降段免疫 CANCEL */
static volatile uint8_t s_phase;
static uint8_t  s_channel;
static UnlkTagRec_t s_tag[UNLK_MAX_TAGS];
static FgnTagRec_t  s_fgn[UNLK_MAX_FGN_TAGS];
static uint8_t  s_total, s_confirmed, s_bitmap;
static uint8_t  s_softCnt, s_softDone;
static uint32_t s_irMs;               /* IR 触发时刻 (W 计时基准) */
static uint32_t s_demagBase;           /* 消磁计数基线 (softCnt>0) */
static uint8_t  s_amDemagOn;           /* 软标段已切消磁模式 (Run 出口切回检测) */
static uint8_t  s_risen;
/* ---- EPC 解锁 (0x10) 循环周期记账 ---- */
static uint8_t  s_cycle, s_cycleOk, s_lastCyc; /* 周期数/成功数/上轮结果(1成功2移除3更换) */
static uint16_t s_oneTmo;                      /* 0x21 单发超时 (监守钩子用) */
static uint8_t  s_msMiss, s_msFgn;              /* 升起/保持期监守: 连续丢失计数 / 外来EPC标志 */
static uint8_t  s_vMiss;                        /* 调试: 校对段 rec 未命中计数 (IO_DIAG 带出) */
static uint8_t  s_reHits;                       /* 回退段监守: 期望标签回归连击计数 */

/* 调试: 校对段匹配状态快照 (IO_DIAG 带出) */
void App_LockerUnlock_GetEpcDbg(uint8_t *v)
{
    v[0] = s_total; v[1] = s_tag[0].len; v[2] = s_phase; v[3] = s_vMiss;
    for (uint8_t i = 0; i < 8; i++) v[4 + i] = s_tag[0].epc[i];
}

uint8_t App_LockerUnlock_IsBusy(void) { return s_busy; }

void App_LockerUnlock_Abort(void) { if (s_busy) s_abort = 1u; }

void App_LockerUnlock_Finish(void)
{
    s_busy = 0u;
    s_abort = 0u;
    s_phase = UNLK_PH_NONE;
    s_immune = 0u;
    LockerSeek_BindAbort(NULL, NULL);     /* 解绑共享寻触的打断标志 */
    App_RgbLedPat_Clear(RGBSRC_UNLOCK);  /* 流程结束撤销灯语声明 */
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
    p->lastCycle = s_lastCyc;   /* 0x10: 上轮结果 1成功/2移除/3更换 */
    if (s_irMs != 0u) {
        uint32_t h = SysTickHl_GetMs() - s_irMs;
        p->holdMs = h;
    }
    uint32_t now = SysTickHl_GetMs();
    for (uint8_t i = 0; i < s_total; i++) {
        /* Round_013: 0x10 保持期 CONFIRMED 仍刷 lastSeen (在场监守),
         * 在场位一并覆盖之; 0x0A (关闭) 语义不变。 */
        if ((s_tag[i].state == UTAG_TRACKING || s_tag[i].state == UTAG_CONFIRMED) &&
            (now - s_tag[i].lastSeen) <= UNLK_PRESENT_WINDOW_MS) {
            p->tagPresent = 1u;
            break;
        }
    }
}

/* ---- 推送帧 (阻塞期间随时经原通道直发, func=FC_LOCKER_CTRL^0xFF) ---- */

static void push_start(uint32_t winMs, uint8_t phase)
{
    uint8_t r[6];
    r[0] = LOCKER_SUB_EVT_START;
    r[1] = UNLK_ERR_OK;
    r[2] = phase;
    r[3] = (uint8_t)(winMs & 0xFF);
    r[4] = (uint8_t)((winMs >> 8) & 0xFF);
    r[5] = (uint8_t)((winMs >> 16) & 0xFF);
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, sizeof(r));
}

/* 确认帧 0x0B: seq + EPC + 已确认/总数 + 判据耗时(ms) + 流程耗时(秒, 统一) */
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
    /* 判据耗时保 ms (亚秒诊断量); 流程耗时改秒 (u16 ms 长窗封顶, 统一) */
    uint16_t v1 = (c1 > 0xFFFFu) ? 0xFFFFu : (uint16_t)c1;
    uint16_t v2 = (c2 / 1000u > 0xFFFFu) ? 0xFFFFu : (uint16_t)(c2 / 1000u);
    r[pos++] = (uint8_t)(v1 & 0xFF); r[pos++] = (uint8_t)(v1 >> 8);
    r[pos++] = (uint8_t)(v2 & 0xFF); r[pos++] = (uint8_t)(v2 >> 8);
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, pos);
}

/* 失配事件帧 0x0C: 外来标签稳定确认一张一帧 (判据同期望标签),
 * 推帧不终止 (客户可能换正确标签, 窗口兜底) */
static void push_mismatch(const uint8_t *epc, uint8_t len, uint8_t hits)
{
    if (len > UNLK_EPC_MAX) len = UNLK_EPC_MAX;
    uint8_t r[2 + UNLK_EPC_MAX + 1];
    uint16_t pos = 0;
    r[pos++] = LOCKER_SUB_EVT_MISMATCH;
    r[pos++] = len;
    for (uint8_t i = 0; i < len; i++) r[pos++] = epc[i];
    r[pos++] = hits;                            /* 稳定确认期间累计读到次数 */
    Proto_TxResponse(s_channel, FC_LOCKER_CTRL, r, pos);
}

/* 硬标完成帧 0x0D (进入回降前) */
static void push_hard(uint8_t endReason)
{
    uint8_t r[8];
    uint32_t e = (s_irMs != 0u) ? (SysTickHl_GetMs() - s_irMs) : 0u;
    uint16_t v = (e / 1000u > 0xFFFFu) ? 0xFFFFu : (uint16_t)(e / 1000u); /* 秒 */
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

/* 电机段失败统一出口: 记录诊断 + 安全回退下端 */
static void motor_fail(uint8_t err, uint8_t mphase, LockerUnlockResult_t *out)
{
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_TEST_FAST);   /* 安全回退: 黄快闪 */
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

/* 整个流程结束统一回降 (裁决4: 磁块保持升起至流程末才缩回):
 * 磁块已升起时寻触 KEY_DOWN。返回 0=成功/未升起,
 * -1=失败 (out 已按 motor_fail 降段语义填好)。 */
static int lower_if_risen(uint32_t *lower, LockerUnlockResult_t *out)
{
    if (!s_risen) return 0;
    s_phase = UNLK_PH_LOWER;
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_OFF);   /* 回降: 灭 */
    s_immune = 1u;   /* 回降本身即安全回退, 免疫打断 */
    int r = LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, lower);
    s_immune = 0u;
    if (r != 0u) {
        motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                  : UNLK_ERR_MOTOR_TIMEOUT,
                   2u, out);
        return -1;
    }
    return 0;
}

/* 周期失败回退 (带"标签回归"监守, 循环模式专用): 期望标签连续
 * EPC_CONFIRM_READS 发读回 -> 中停回退。返回 0=回退到底, 1=标签稳定
 * 回归中停 (调用方跳再入场闸直接再升), -1=电机失败 (out 已填)。
 * Round_013 用户裁决: 升起中标签移走收起后, 回退途中标签放回即再升。 */
static int retract_monitor(void);
static int lower_if_risen_watched(uint32_t *lower, LockerUnlockResult_t *out)
{
    if (!s_risen) return 0;
    s_phase = UNLK_PH_LOWER;
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_OFF);   /* 回降: 灭 */
    s_immune = 1u;   /* 回退本身即安全回退, 免疫打断 */
    s_reHits = 0u;
    LockerSeek_SetMonitor(retract_monitor);
    int r = LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, lower);
    LockerSeek_SetMonitor(NULL);
    s_immune = 0u;
    if (r == 3u) return 1;               /* 标签稳定回归: 中停 */
    if (r != 0u) {
        motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                  : UNLK_ERR_MOTOR_TIMEOUT,
                   2u, out);
        return -1;
    }
    return 0;
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

/* 外来标签入缓存 (裁决3): 命中返回既有记录; 未命中占空槽新建;
 * 缓存满返回 NULL (只累计 tagsFound, 不参与稳定上报)。 */
static FgnTagRec_t *fgn_track(const AppUHFTag_t *tag, uint32_t now)
{
    uint8_t freeSlot = UNLK_MAX_FGN_TAGS;
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) {
        FgnTagRec_t *g = &s_fgn[i];
        if (g->len != 0u && g->len == tag->epcLen) {
            uint8_t k;
            for (k = 0; k < g->len; k++)
                if (g->epc[k] != tag->epc[k]) break;
            if (k >= g->len) return g;
        }
        if (g->len == 0u && freeSlot == UNLK_MAX_FGN_TAGS) freeSlot = i;
    }
    if (freeSlot == UNLK_MAX_FGN_TAGS) return NULL;
    FgnTagRec_t *g = &s_fgn[freeSlot];
    g->len = tag->epcLen;
    for (uint8_t k = 0; k < g->len; k++) g->epc[k] = tag->epc[k];
    g->hits = 0u;
    g->reported = 0u;
    g->trackStart = now;
    g->lastSeen = now;
    return g;
}

static void unlk_run(const uint8_t *epc, uint8_t epcLen, uint8_t epcCnt,
                     uint16_t tmoMs, uint16_t holdMaxMs, uint8_t softCnt,
                     uint8_t channel, LockerUnlockResult_t *out)
{
    /* epcCnt=0 (纯软标) 仅需 softCnt>0; epcCnt>0 才要求 EPC 数据齐 */
    if (!out || epcCnt > UNLK_MAX_TAGS || epcLen > UNLK_EPC_MAX ||
        (epcCnt == 0u && softCnt == 0u) ||
        (epcCnt > 0u && (!epc || epcLen == 0u))) {
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
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) s_fgn[i].len = 0u;
    if (tmoMs == 0u) tmoMs = UNLK_INVENTORY_TMO_MS;
    if (tmoMs > 10000u) tmoMs = 10000u;

    /* ---- ⑴ 前置检查 ---- */
    if (App_Locker_IsIdle() == 0)                    { fill_busy(out); return; }
    if (App_UHF_GetState() == APP_UHF_SCAN)          { fill_busy(out); return; }
    if (App_UHF_IsBusy())                           { fill_busy(out); return; }
    if (App_Stepper_GetState() != APP_STEPPER_IDLE)  { fill_busy(out); return; }
    if (App_MotorHoming_GetStatus() == HOMING_STAT_RUNNING) {
        fill_busy(out);                 /* Round_098 #11: 后台回零进行中 -> BUSY */
        return;
    }
    if (App_MotorHoming_IsReady() == 0) {
        out->err = UNLK_ERR_HOMING;
        out->switchErr = App_Stepper_GetSwitchErr();
        return;
    }

    /* ---- W 解锁窗: holdMaxMs=0 -> 公式, 非零 -> min(host, 上限) ---- */
    uint32_t winMs;
    if (epcCnt == 0u) {
        winMs = UNLK_SOFT_WINDOW_MS;         /* 纯软标: 无硬标窗, 窗口即软标窗 */
    } else {
        if (holdMaxMs == 0u)
            winMs = UNLK_HOLD_BASE_MS + (uint32_t)(epcCnt - 1u) * UNLK_HOLD_EXTRA_PER_TAG_MS;
        else
            winMs = holdMaxMs;
        if (winMs > UNLK_HOLD_MAX_MS) winMs = UNLK_HOLD_MAX_MS;
    }

    push_start(winMs,
               (epcCnt == 0u) ? UNLK_PH_SOFT : UNLK_PH_WAIT_TAG);   /* 受理帧 */

    if (epcCnt == 0u) {
        /* ---- 纯软标通道 (epcCnt=0, 裁决"直接启动软解码"): 跳过
         * 光电门控与 EPC 校验, 受理即计时, 直接进入 升起+消磁+软解码 ---- */
        s_irMs = SysTickHl_GetMs();          /* 流程计时基准 = 受理时刻 */
        s_phase = UNLK_PH_SOFT;
    } else {
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_IR_WAIT);   /* 待放标: 白慢闪 (Round_011 A) */

    /* ---- ⑵ 光电门控: 等待放标 (PC11, 去抖) ---- */
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
            if ((now - t0) >= winMs) {
                out->err = UNLK_ERR_NO_IR;
                App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 未放标: 黄慢闪 2s (Round_011 D4) */
                return;
            }
        }
    }
        s_irMs = SysTickHl_GetMs();          /* W 计时基准 = IR 触发时刻 */
    }

    /* ---- ⑶ 消磁准备: softCnt=0 跳过, 不触碰 AM ----
     * EPC 校验通过前 AM 只检测不消磁: 此处仅探链 + 强制检测模式
     * (防上次流程残留消磁模式在校对期误消软标); 消磁模式待全部
     * 确认升起时切 (裁决4: 电机至上行程开关同时开启), 整个流程
     * 结束 (Run 出口) 再切回检测模式。*/
    if (softCnt > 0u) {
        if (App_AM_Query() != APP_AM_ERR_OK) {
            out->err = UNLK_ERR_AM_LINK;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 (Round_011 D4) */
            return;
        }
        AppAMConfig_t amc;
        (void)App_AM_GetConfig(&amc);
        if (amc.mode != AM_MODE_DETECT_ONLY &&
            App_AM_SetParam(AM_CMD_MODE, AM_MODE_DETECT_ONLY) != APP_AM_ERR_OK) {
            out->err = UNLK_ERR_AM_LINK;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    }

    /* ---- ⑷ UHF 就绪 (纯软标 epcCnt=0 跳过: 不盘点) ---- */
    if (epcCnt > 0u) {
    s_phase = UNLK_PH_VERIFY;
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_SCAN_ACTIVE);   /* 盘点校对中: 蓝慢闪 (Round_011 A) */
    if (App_UHF_GetState() == APP_UHF_ERROR) {
        (void)App_UHF_Close();   /* ERROR 态: 彻底下电重上 */
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = UNLK_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    } else if (App_UHF_GetState() != APP_UHF_READY) {
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = UNLK_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    }
    App_UHF_ClearTags();                     /* 任务边界: 清陈旧标签 */
    if (s_abort) { out->err = UNLK_ERR_OK; out->endReason = UNLK_END_ABORTED; return; }
    }   /* epcCnt>0 UHF 就绪 */

    /* ---- ⑸ 持续校对: 逐张确认推帧+蜂鸣, n==m/窗满收尾 ----
     * (全确认门控: 所有 EPC 均校验通过才动电机, 升起移至 ⑸ 末尾) */
    (void)App_Stepper_SetSpeedHz(LSEEK_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(LSEEK_TORQUE_PCT);
    if (epcCnt > 0u)
        (void)App_UHF_InventoryTimeout(tmoMs);

    uint32_t lostStart = 0u;
    uint8_t  hardEnd = (epcCnt == 0u) ? UNLK_END_ALL_OK : 0u;  /* 纯软标直通 */
    uint32_t rise = 0u, lower = 0u;

    while (hardEnd == 0u) {
        LockerSeek_Pump();
        if (s_abort) { hardEnd = UNLK_END_ABORTED; break; }

        uint32_t now = SysTickHl_GetMs();

        /* 取走本轮读到的标签, 更新各记录 */
        AppUHFTag_t tag;
        while (App_UHF_TagTake(&tag) == 0) {
            App_RgbLedPat_Flash(RGBFLASH_TAG_SEEN);   /* 读到一张标签: 蓝单闪 (Round_011 A) */
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
            } else {
                if (out->tagsFound < 255u) out->tagsFound++;
                FgnTagRec_t *g = fgn_track(&tag, now);   /* 裁决3: 多标签缓存 */
                if (g != NULL && g->reported == 0u) {    /* 已上报: 静默忽略 */
                    g->hits++;
                    g->lastSeen = now;
                }
                out->fgnEpcLen = (tag.epcLen > UNLK_EPC_MAX) ? UNLK_EPC_MAX : tag.epcLen;
                for (uint8_t k = 0; k < out->fgnEpcLen; k++) out->fgnEpc[k] = tag.epc[k];
            }
        }

        /* 在场判定 + 双门限确认 (1.5s 连续在场 + >=2 hits, 裁决2) */
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
            }
        }

        if (s_confirmed >= s_total)      { hardEnd = UNLK_END_ALL_OK; break; }
        if ((now - s_irMs) >= winMs)     { hardEnd = UNLK_END_PARTIAL_TIMEOUT; break; }

        /* UHF 链路失联: 确认窗仍坏 -> 无法证实标签在场 */
        if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0) {
            if (lostStart == 0u) lostStart = now;
            if ((now - lostStart) >= UNLK_UHF_LOST_CONFIRM_MS) {
                hardEnd = UNLK_END_UHF_LOST;
                App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 (Round_011 D4) */
                break;
            }
        } else {
            lostStart = 0u;
        }

        /* 外来标签稳定确认 (裁决3): 判据同期望标签双门限, 稳定一张
         * 推一帧失配 (单向掩码不重报); 离场超窗释放缓存槽 */
        for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) {
            FgnTagRec_t *g = &s_fgn[i];
            if (g->len == 0u || g->reported != 0u) continue;
            if ((now - g->lastSeen) > UNLK_PRESENT_WINDOW_MS) {
                g->len = 0u;                    /* 离场超窗: 释放缓存槽 */
                continue;
            }
            if (g->hits >= UNLK_CONFIRM_MIN_HITS &&
                (now - g->trackStart) >= UNLK_STABLE_CONFIRM_MS) {
                g->reported = 1u;                /* 单向掩码, 不再重报 */
                push_mismatch(g->epc, g->len, g->hits);
                App_RgbLedPat_Flash(RGBFLASH_MISMATCH_3S);   /* 失配: 红快闪 3s */
            }
        }

        /* 一轮盘点结束 -> 接力下一轮 (失配已改逐张稳定确认, 见上) */
        if (!App_UHF_IsBusy() && App_UHF_GetState() == APP_UHF_READY) {
            (void)App_UHF_InventoryTimeout(tmoMs);
        }
    }

    /* ---- 全部确认 (n==m) 才升起: softCnt>0 同时切 AM 消磁 (裁决4);
     * 全确认门控: 任一 EPC 未通过则磁块全程不动 (PARTIAL/UHF_LOST
     * 出口无升降无消磁, CANCEL 出口未动过磁块)。升起中 CANCEL ->
     * 免疫回降后按 ABORTED 结账 (交统一出口)。 ---- */
    if (hardEnd == UNLK_END_ALL_OK) {
        s_risen = 1u;
        s_phase = UNLK_PH_RISE;
        App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_RISE_HOLD_GREEN);
        if (softCnt > 0u &&
            App_AM_SetParam(AM_CMD_MODE, AM_MODE_DEACTIVATE) == APP_AM_ERR_OK) {
            s_amDemagOn = 1u;
            s_demagBase = App_AM_GetDeactCount();
        }
        int r = LockerSeek_RunRetry(LSEEK_DIR_UP, LSEEK_RISE_MAX, &rise);
        if (r == 2u) {
            hardEnd = UNLK_END_ABORTED;   /* 免疫回降交统一出口 */
        } else if (r != 0u) {
            motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                      : UNLK_ERR_MOTOR_TIMEOUT,
                       1u, out);
            return;
        }
    }

    /* ---- ⑹ 硬标段收尾: 完成帧 (裁决4: 磁块保持升起, 回降移至流程末) ---- */
    push_hard(hardEnd);
    if (epcCnt > 0u) (void)App_UHF_Stop();   /* 纯软标未开盘点, 不停 */
    out->endReason = hardEnd;
    out->confirmed = s_confirmed;
    out->total = s_total;
    out->confirmedBitmap = s_bitmap;
    if (hardEnd == UNLK_END_PARTIAL_TIMEOUT)
        App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 窗满未收齐: 黄慢闪 2s (Round_011 D4) */

    if (hardEnd == UNLK_END_ABORTED) {
        /* CANCEL: 跳过软标, 回降后以 ABORTED 结账 */
        out->err = UNLK_ERR_OK;
        if (lower_if_risen(&lower, out) != 0) return;
        out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
        out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
        return;
    }

    /* ---- ⑺ 软标解码 (全确认升起时已开消磁, 裁决4), 此处仅计数收账;
     * 未全确认 (PARTIAL/UHF_LOST) 不升未消, 直接结账;
     * 窗满未校验完成按超时失败结账 (裁决5) ---- */
    if (softCnt > 0u && hardEnd == UNLK_END_ALL_OK) {
        s_phase = UNLK_PH_SOFT;
        App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_SOFT_WAIT_WHITE);   /* 软标: 白常亮 */
        if (!s_amDemagOn) {   /* 升起时切消磁失败: 入口再试一次, 再败按 AM_LINK 结账 */
            if (App_AM_SetParam(AM_CMD_MODE, AM_MODE_DEACTIVATE)
                    == APP_AM_ERR_OK) {
                s_amDemagOn = 1u;
                s_demagBase = App_AM_GetDeactCount();
            } else {
                out->err = UNLK_ERR_AM_LINK;
                App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 (Round_011 D4) */
                if (lower_if_risen(&lower, out) != 0) return;
                out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
                out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
                return;
            }
        }
        uint32_t t0 = SysTickHl_GetMs();
        while (1) {
            LockerSeek_Pump();
            if (s_abort) {   /* CANCEL: 回降后以 ABORTED 结账 */
                out->err = UNLK_ERR_OK;
                out->endReason = UNLK_END_ABORTED;
                if (lower_if_risen(&lower, out) != 0) return;
                out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
                out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
                return;
            }
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
        if (s_softDone < softCnt) {   /* 软标窗满未校验完成: 按超时失败结账 (裁决5) */
            out->endReason = UNLK_END_SOFT_TIMEOUT;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 消磁失败: 红双闪 */
        }
    }

    /* ---- ⑻ 结账: 整个流程结束统一回降 (裁决4) + 终帧 ---- */
    s_phase = UNLK_PH_DONE;
    if (lower_if_risen(&lower, out) != 0) return;
    out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
    out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
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
    /* Round_098 #21: 流程窗内强制会话 S0 (连续盘存对 S2/S3 敏感),
     * 含全部提前失败出口统一还原. */
    (void)App_UHF_ScanSessionBegin();
    if (out) {
        unlk_run(epc, epcLen, epcCnt, tmoMs, holdMaxMs, softCnt, channel, out);
        if (out->elapsedMs == 0u && s_irMs != 0u)
            out->elapsedMs = SysTickHl_GetMs() - s_irMs;
    }
    App_UHF_ScanSessionEnd();
    /* 消磁期结束 (升起开消磁起至整个流程结束/任意出口): 切回
     * AM 仅检测模式 (裁决1: 固定仅检测, 不恢复流程前预设)。 */
    if (s_amDemagOn) {
        s_amDemagOn = 0u;
        (void)App_AM_SetParam(AM_CMD_MODE, AM_MODE_DETECT_ONLY);
    }
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), Finish 再做终清 */
    App_RgbLedPat_Clear(RGBSRC_UNLOCK);
}

/* =====================================================================
 * EPC 解锁流程 (LOCKER_SUB_UNLOCK_EPC 0x10) — Round_013 新增独立通道。
 * 单周期: IR 门控 -> 0x21 逐发 EPC 稳定确认 (计数判据, Round_013 裁决) ->
 * 升起寻触 KEY_UP (期间盘点不停, 移除/更换即收起) -> 顶部保持 holdTopMs
 * -> 回降。周期失败不推帧, 状态由 GET_PROGRESS 拉取 (Round_013 裁决)。
 *
 * 循环模式 (帧 rsv0=timeoutSec>0): 受理起 timeoutSec 秒内周期往复; 每轮
 * 回退后须先观察到 标签移走 (连续 EPC_LOST_READS 发空射) 且红外回落,
 * 再经红外触发 + EPC 稳定才开下一轮; 流程窗在周期边界判定 (进行中的
 * 周期跑完)。timeoutSec=0 单轮模式: 周期失败即流程结束 (原语义)。
 * 无软标/无消磁; 旧 0x0A 关闭后灯语槽位 (RGBSRC_UNLOCK) 与 UNLK_ERR_*
 * 错误码由本流程接管。
 * ===================================================================== */

/* 升起段监守 (LockerSeek 泵拍回调): 0x21 逐发, 连续 EPC_LOST_READS 发
 * 未读到期望 EPC -> 返回 1 请求停机收起; 读到外来 EPC 记更换语义。 */
static int epc_monitor(void)
{
    AppUHFTag_t tag;
    uint8_t hit = (App_UHF_InventoryOneSync(s_oneTmo, &tag, NULL)
                  == APP_UHF_ERR_OK) ? 1u : 0u;
    if (hit && rec_find(&tag) != NULL) {
        s_msMiss = 0u;
        return 0;
    }
    if (hit) s_msFgn = 1u;              /* 读到外来: 更换 */
    return (++s_msMiss >= EPC_LOST_READS) ? 1 : 0;
}

/* 回退段监守 (周期失败回退): 期望标签连续 EPC_CONFIRM_READS 发读回 ->
 * 返回 1 请求中停回退, 跳过再入场闸直接再升起 (Round_013 用户裁决:
 * 回退中标签放回即再升)。空射/外来清零 — 外来标签不得触发再升。
 * 2026-09-19 收紧 (乱跑实测): 计数仅在红外遮挡 (标签在门上) 时累积
 * — 远距边缘标签的偶发读回爆发不得驱动电机; 合法放回必然挡光。 */
static int retract_monitor(void)
{
    if (App_NewPeriph_ReadIr() == 0u) {   /* 红外未遮挡: 不计回归 */
        s_reHits = 0u;
        return 0;
    }
    AppUHFTag_t tag;
    uint8_t hit = (App_UHF_InventoryOneSync(s_oneTmo, &tag, NULL)
                  == APP_UHF_ERR_OK) ? 1u : 0u;
    if (hit && rec_find(&tag) != NULL)
        return (++s_reHits >= EPC_CONFIRM_READS) ? 1 : 0;
    s_reHits = 0u;
    return 0;
}

static void epc_run(const uint8_t *epc, uint8_t epcLen, uint16_t tmoMs,
                    uint32_t winMs, uint32_t holdTopMs, uint8_t timeoutSec,
                    uint8_t channel, LockerUnlockResult_t *out)
{
    s_busy = 1u;
    s_abort = 0u;
    s_immune = 0u;
    s_phase = UNLK_PH_WAIT_TAG;
    s_channel = channel;
    s_total = 1u;
    s_confirmed = 0u;
    s_bitmap = 0u;
    s_softCnt = 0u;
    s_softDone = 0u;
    s_risen = 0u;
    s_irMs = 0u;
    s_demagBase = 0u;
    s_amDemagOn = 0u;
    s_cycle = 0u;
    s_cycleOk = 0u;
    s_lastCyc = 0u;
    s_oneTmo = tmoMs;
    s_msMiss = 0u;
    s_msFgn = 0u;
    s_vMiss = 0u;
    s_reHits = 0u;
    LockerSeek_BindAbort(&s_abort, &s_immune);
    s_tag[0].len = epcLen;
    s_tag[0].state = UTAG_PENDING;
    s_tag[0].hits = 0u;
    s_tag[0].trackStart = 0u;
    s_tag[0].lastSeen = 0u;
    for (uint8_t k = 0; k < epcLen; k++) s_tag[0].epc[k] = epc[k];
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) s_fgn[i].len = 0u;

    /* ---- 前置检查 (同 0x0A ⑴: Locker/UHF/电机占用与回零就绪) ---- */
    if (App_Locker_IsIdle() == 0)                    { fill_busy(out); return; }
    if (App_UHF_GetState() == APP_UHF_SCAN)          { fill_busy(out); return; }
    if (App_UHF_IsBusy())                            { fill_busy(out); return; }
    if (App_Stepper_GetState() != APP_STEPPER_IDLE)  { fill_busy(out); return; }
    if (App_MotorHoming_GetStatus() == HOMING_STAT_RUNNING) {
        fill_busy(out);                 /* 后台回零进行中 -> BUSY */
        return;
    }
    if (App_MotorHoming_IsReady() == 0) {
        out->err = UNLK_ERR_HOMING;
        out->switchErr = App_Stepper_GetSwitchErr();
        return;
    }

    /* ---- UHF 就绪 + 开盘 (ERROR 态彻底下电重上, 同 0x0A ⑷; 循环模式全程需要) ---- */
    if (App_UHF_GetState() == APP_UHF_ERROR) {
        (void)App_UHF_Close();
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = UNLK_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    } else if (App_UHF_GetState() != APP_UHF_READY) {
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = UNLK_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);   /* 链路断: 红双闪 */
            return;
        }
    }
    App_UHF_ClearTags();                     /* 任务边界: 清陈旧标签 */
    if (s_abort) { out->err = UNLK_ERR_OK; out->endReason = EPC_END_ABORTED; return; }

    /* ---- 受理: 循环窗 timeoutSec / 单轮窗 W (回显实际窗) ---- */
    uint8_t  repeat = (timeoutSec != 0u);
    uint32_t flowMs = (uint32_t)timeoutSec * 1000u;
    uint32_t flowT0 = SysTickHl_GetMs();
    push_start(repeat ? flowMs : winMs, UNLK_PH_WAIT_TAG);   /* 受理帧 0x0F */
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_IR_WAIT);   /* 待放标: 白慢闪 */
    (void)App_Stepper_SetSpeedHz(LSEEK_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(LSEEK_TORQUE_PCT);
    if (repeat) s_irMs = flowT0;           /* 循环模式: 流程耗时基准 = 受理 */

    uint32_t rise = 0u, lower = 0u;
    uint8_t  endErr = UNLK_ERR_OK;          /* 非0: err 出口 (NO_IR) */
    uint8_t  endR = 0u;                     /* 流程结束码 EPC_END_* */
    uint8_t  firstCycle = 1u;
    uint8_t  skipGate = 0u;  /* 回退中标签放回中停: 下一轮跳再入场闸直接再升 */
    uint8_t  lastFail = 0u;  /* 上一周期失败: 下轮免"移走+红外回落"手势,
                              * 标签重新稳定在场 (校对段) 即直接开下一轮 */

    while (endErr == UNLK_ERR_OK && endR == 0u) {

        /* ---- (a) 再入场闸 (非首轮): 标签移走 + 红外回落 ----
         * 用户裁决: 必须标签移走 (连续 EPC_LOST_READS 发空射) 且红外
         * 回落, 才允许下一轮触发; 否则停在白闪等待。防搁置标签无限
         * 自动循环。
         * 例外 (Round_013 裁决): ① 再升路径 (skipGate, 回退中标签放回
         * 中停); ② 上一周期失败 (lastFail) — 移走已发生过, 期望标签
         * 重新稳定在场 (校对段判定) 即直接开下一轮, 与再升语义连成
         * 一体。 */
        if (!firstCycle && skipGate == 0u && lastFail == 0u) {
            s_phase = UNLK_PH_WAIT_TAG;   /* 再入场闸: 等待标签移走+红外回落 */
            App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_IR_WAIT);
            uint8_t absent = 0u, irLow = 0u, miss = 0u;
            uint32_t lostStart = 0u;
            while (!absent || !irLow) {
                LockerSeek_Pump();
                if (s_abort) { endR = EPC_END_ABORTED; break; }
                AppUHFTag_t tag;
                uint8_t hit = (App_UHF_InventoryOneSync(tmoMs, &tag, NULL)
                              == APP_UHF_ERR_OK) ? 1u : 0u;
                uint32_t now = SysTickHl_GetMs();
                if (hit && rec_find(&tag) != NULL) miss = 0u;
                else if (++miss >= EPC_LOST_READS) absent = 1u;
                if (App_NewPeriph_ReadIr() == 0u) irLow = 1u;
                if ((now - flowT0) >= flowMs) {
                    endR = (s_cycleOk > 0u) ? EPC_END_ALL_OK : EPC_END_TIMEOUT;
                    break;
                }
                if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0) {
                    if (lostStart == 0u) lostStart = now;
                    if ((now - lostStart) >= UNLK_UHF_LOST_CONFIRM_MS) {
                        endR = EPC_END_UHF_LOST;
                        break;
                    }
                } else {
                    lostStart = 0u;
                }
            }
            if (endR != 0u) break;
        }

        /* ---- (b) 红外门控: 等待放标 (PC11 去抖; 单轮=窗 W, 循环=流程窗) ----
         * 再升路径 (skipGate, 回退中标签放回) 免红外触发 — 回归监守已
         认证稳定在场且红外遮挡 (Round_013 裁决: 标签放回即再升)。
         上轮失败后再入场 (lastFail) **必须重新过红外门** (2026-09-19
         收紧): 失败免的只是"移走"手势, 开盘仍由红外触发 — 远距边缘
         标签偶发凑齐判据不得驱动电机乱跑 (乱跑实测修正)。 */
        if (skipGate == 0u) {
            uint32_t irHighSince = 0u;
            for (;;) {
                LockerSeek_Pump();
                if (s_abort) { endR = EPC_END_ABORTED; break; }
                uint32_t now = SysTickHl_GetMs();
                if (App_NewPeriph_ReadIr() != 0u) {
                    if (irHighSince == 0u) irHighSince = now;
                    if ((now - irHighSince) >= UNLK_IR_CONFIRM_MS) break;   /* 放标触发 */
                } else {
                    irHighSince = 0u;
                }
                if (repeat) {
                    if ((now - flowT0) >= flowMs) {
                        endR = (s_cycleOk > 0u) ? EPC_END_ALL_OK : EPC_END_TIMEOUT;
                        break;
                    }
                } else if ((now - flowT0) >= winMs) {
                    endErr = UNLK_ERR_NO_IR;
                    App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 未放标: 黄慢闪 2s */
                    break;
                }
            }
            if (endR != 0u || endErr != UNLK_ERR_OK) break;
            if (!repeat) s_irMs = SysTickHl_GetMs();   /* 单轮: W 计时基准 = IR 触发 */
        }
        skipGate = 0u;                     /* 闸/红外跳过仅生效一轮 (再升路径) */

        /* ---- (c) EPC 稳定确认 (蓝慢闪; 计数判据, Round_013 裁决) ----
         * 0x21 单标签逐发轮询 (~27ms/发, 命中即返): 连续 EPC_CONFIRM_READS
         * 次读到且 EPC 一致 -> 稳定在场; 空射/读到外来 EPC 打断连续计数;
         * 单轮模式 W 窗与 UHF 失联窗兜底, 循环模式以流程窗兜底。 */
        s_phase = UNLK_PH_VERIFY;
        App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_SCAN_ACTIVE);
        {
            uint32_t lostStart = 0u;
            uint8_t  consec = 0u;               /* 连续命中 (读到且 EPC 一致) */
            uint8_t  cycConfirmed = 0u;
            s_tag[0].state = UTAG_TRACKING;
            s_tag[0].trackStart = SysTickHl_GetMs();   /* 确认帧判据耗时基准 */
            while (cycConfirmed == 0u) {
                LockerSeek_Pump();
                if (s_abort) { endR = EPC_END_ABORTED; break; }
                AppUHFTag_t tag;
                uint8_t hit = (App_UHF_InventoryOneSync(tmoMs, &tag, NULL)
                              == APP_UHF_ERR_OK) ? 1u : 0u;
                uint32_t now = SysTickHl_GetMs();
                if (hit && rec_find(&tag) != NULL) {
                    s_tag[0].lastSeen = now;
                    if (++consec >= EPC_CONFIRM_READS) {   /* 连续一致: 稳定在场 */
                        cycConfirmed = 1u;
                        break;
                    }
                } else {
                    consec = 0u;                   /* 空射/读到外来: 计数打断 */
                    s_vMiss++;                    /* 调试: rec 未命中计数 */
                    if (hit) {                      /* 外来标签: 仅诊断记账 */
                        if (out->tagsFound < 255u) out->tagsFound++;
                        out->fgnEpcLen = (tag.epcLen > UNLK_EPC_MAX) ? UNLK_EPC_MAX : tag.epcLen;
                        for (uint8_t k = 0; k < out->fgnEpcLen; k++) out->fgnEpc[k] = tag.epc[k];
                    }
                }
                if (repeat) {
                    if ((now - flowT0) >= flowMs) {
                        endR = (s_cycleOk > 0u) ? EPC_END_ALL_OK : EPC_END_TIMEOUT;
                        break;
                    }
                } else if ((now - s_irMs) >= winMs) {   /* W 窗满未确认 */
                    endR = EPC_END_TIMEOUT;
                    App_RgbLedPat_Flash(RGBFLASH_WARN_2S);
                    break;
                }
                if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0) {
                    if (lostStart == 0u) lostStart = now;
                    if ((now - lostStart) >= UNLK_UHF_LOST_CONFIRM_MS) {
                        endR = EPC_END_UHF_LOST;
                        App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);
                        break;
                    }
                } else {
                    lostStart = 0u;
                }
            }
            if (endR != 0u) break;

            /* 稳定确认 (每周期): 0x0B 确认帧 + 蜂鸣 + 绿单闪 (裁决②) */
            s_tag[0].state = UTAG_CONFIRMED;
            if (s_confirmed == 0u) {           /* 首周期: 流程级确认位 */
                s_confirmed = 1u;
                s_bitmap = 1u;
            }
            s_cycle++;
            s_softCnt = s_cycle;               /* GET_PROGRESS 位: 周期数 */
            push_tag(&s_tag[0]);              /* 0x0B 确认帧 (无蜂鸣: 确认鸣
                                               * 已废除, 反馈集中到收回 800ms +
                                               * 流程结束 1s, 2026-09-19 裁决) */
            App_RgbLedPat_Flash(RGBFLASH_MATCH_OK);
        }

        /* ---- (d) 升起: 寻触 KEY_UP (全确认才动磁块, 蓝转绿常亮) ----
         * 期间盘点不停 (监守钩子): 标签移除/更换即停机收起 (裁决①)。 */
        s_risen = 1u;
        s_phase = UNLK_PH_RISE;
        App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_RISE_HOLD_GREEN);
        s_msMiss = 0u;
        s_msFgn = 0u;
        LockerSeek_SetMonitor(epc_monitor);
        int r = LockerSeek_RunRetry(LSEEK_DIR_UP, LSEEK_RISE_MAX, &rise);
        LockerSeek_SetMonitor(NULL);
        uint8_t cycFail = 0u;
        if (r == 3u) {           /* 标签移除/更换: 收起, 状态由拉取获知 */
            cycFail = 1u;
            s_lastCyc = s_msFgn ? 3u : 2u;   /* 3=更换 2=移除 */
        } else if (r == 2u) {    /* 升起中 CANCEL: 免疫回降交统一出口 */
            endR = EPC_END_ABORTED;
            break;
        } else if (r != 0u) {
            (void)App_UHF_Stop();
            motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                      : UNLK_ERR_MOTOR_TIMEOUT,
                       1u, out);
            return;
        }

        /* ---- (e) 顶部保持: EPC 在场监守 (默认 holdTopMs) ----
         * 移除判据 (Round_013 裁决): 连续 EPC_LOST_READS 次未读到一致的
         * EPC 即稳定移除; 读到即清零计数 (0x21 命中即返, 保持期监守粒度
         * ~27ms/发)。保持期满 EPC 仍在场 -> 周期成功。 */
        if (cycFail == 0u) {
            s_phase = UNLK_PH_HOLD;
            uint32_t hold0 = SysTickHl_GetMs();
            uint32_t lostStart = 0u;
            uint8_t  holdDone = 0u;
            while (1) {
                LockerSeek_Pump();
                if (s_abort) { endR = EPC_END_ABORTED; break; }
                AppUHFTag_t tag;
                uint8_t hit = (App_UHF_InventoryOneSync(tmoMs, &tag, NULL)
                              == APP_UHF_ERR_OK) ? 1u : 0u;
                uint32_t now = SysTickHl_GetMs();
                if (hit && rec_find(&tag) != NULL) {
                    s_msMiss = 0u;               /* 读到且一致: 计数清零 */
                    s_tag[0].lastSeen = now;
                } else {
                    if (hit) s_msFgn = 1u;       /* 读到外来: 更换 */
                    if (++s_msMiss >= EPC_LOST_READS) {   /* 稳定移除/更换 */
                        cycFail = 1u;
                        s_lastCyc = s_msFgn ? 3u : 2u;
                        break;
                    }
                }
                if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0) {
                    if (lostStart == 0u) lostStart = now;
                    if ((now - lostStart) >= UNLK_UHF_LOST_CONFIRM_MS) {
                        endR = EPC_END_UHF_LOST;
                        App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);
                        break;
                    }
                } else {
                    lostStart = 0u;
                }
                if ((now - hold0) >= holdTopMs) { holdDone = 1u; break; }   /* 保持期满 */
            }
            if (endR != 0u) break;
            if (holdDone != 0u) {           /* 周期成功: 状态可拉取 (无蜂鸣,
                s_lastCyc = 1u;              * 收回完成时统一 800ms, 2026-09-19 裁决) */
                s_cycleOk++;
                s_softDone = s_cycleOk;     /* GET_PROGRESS 位: 成功周期数 */
            }
        }

        /* ---- (f) 周期末回退 (免疫), 流程窗在回退后判定 ----
         * 周期失败 (循环模式): 回退挂"标签回归"监守 — 期望标签放回
         * (连续 EPC_CONFIRM_READS 发) 即中停回退, 免再入场闸直接
         * 再升 (Round_013 用户裁决); 回退到底则由 lastFail 免下轮
         * "移走"手势, 校对段等稳定回归即开下一轮。 */
        lastFail = cycFail;               /* 下轮闸条件: 失败免移走手势 */
        if (cycFail != 0u) {
            App_RgbLedPat_Flash(RGBFLASH_WARN_2S);       /* 周期失败: 黄慢闪 */
            if (!repeat) endR = EPC_END_EPC_LOST;        /* 单轮: 失败即结束 */
        }
        uint8_t reRise = 0u;
        if (cycFail != 0u && repeat && endR == 0u) {
            /* 周期失败下回不鸣 (2026-09-19 用户裁决: 蜂鸣只留
             * 保持满回降 + 流程结束两处, 移走/更换静默回退) */
            int lr = lower_if_risen_watched(&lower, out);  /* 带回归监守回退 */
            if (lr < 0) { (void)App_UHF_Stop(); return; }
            if (lr > 0) {                  /* 标签放回: 周期末窗检, 未满即再升 */
                if ((SysTickHl_GetMs() - flowT0) >= flowMs) {
                    endR = (s_cycleOk > 0u) ? EPC_END_ALL_OK : EPC_END_TIMEOUT;
                    /* 窗已满: 从中停位回退到底再结 */
                    if (lower_if_risen(&lower, out) != 0) {
                        (void)App_UHF_Stop();
                        return;
                    }
                    s_risen = 0u;
                } else {
                    reRise = 1u;           /* 保持半程, 跳闸直接下一轮 */
                }
            } else {
                s_risen = 0u;              /* 回退到底 */
            }
        } else {
            if (cycFail == 0u) {
                App_NewPeriph_BeepPulse(EPC_BEEP_CYCLE_END_MS);  /* 周期成功
                    * (保持满在场) 开始下回才鸣 — 2026-09-19 裁决:
                    * 失败路径 (单轮 EPC_LOST 下回) 一律不鸣 */
            }
            if (lower_if_risen(&lower, out) != 0) {      /* 回退失败: 电机出口 */
                (void)App_UHF_Stop();
                return;
            }
            s_risen = 0u;                  /* 已回退: 统一出口不再重复降 */
        }
        if (reRise != 0u) { skipGate = 1u; continue; }    /* 跳闸: 直接再升 */
        if (endR != 0u) break;
        if (!repeat) { endR = EPC_END_ALL_OK; break; }   /* 单轮: 成功即结束 */
        if ((SysTickHl_GetMs() - flowT0) >= flowMs) {    /* 回退后窗满 */
            endR = (s_cycleOk > 0u) ? EPC_END_ALL_OK : EPC_END_TIMEOUT;
            break;
        }
        firstCycle = 0u;                   /* 循环模式: 等待再入场 */
    }

    /* ---- 流程统一出口 ---- */
    (void)App_UHF_Stop();
    if (endErr != UNLK_ERR_OK) {           /* NO_IR: err 出口 */
        out->err = endErr;
        return;
    }
    out->err = UNLK_ERR_OK;
    out->endReason = endR;
    if (lower_if_risen(&lower, out) != 0) return;   /* CANCEL/UHF 断链中断时回降 */
    out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
    out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
    if (endR == EPC_END_ALL_OK) {
        App_NewPeriph_BeepPulse(EPC_BEEP_FLOW_DONE_MS);    /* 流程结束 (时间到): 1s */
        App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);
    } else if (endR == EPC_END_TIMEOUT && repeat) {
        App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 流程窗满无成功周期 */
    }
}

void App_LockerUnlock_EpcRun(const uint8_t *epc, uint8_t epcLen, uint16_t tmoMs,
                             uint16_t winMaxMs, uint16_t holdTopMs, uint8_t timeoutSec,
                             uint8_t channel, LockerUnlockResult_t *out)
{
    if (!out || !epc || epcLen == 0u || epcLen > UNLK_EPC_MAX) {
        if (out) { Memset8((void*)out, 0, sizeof(*out)); out->err = UNLK_ERR_PARAM; }
        return;
    }
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = UNLK_RETREAT_NONE;
    /* 参数缺省/钳位 (tmo 为 0x21 单发超时, 缺省 EPC_ONE_TMO_MS; timeoutSec
     * =0 单轮 / >0 循环模式流程窗; winMs 仅单轮模式生效) */
    if (tmoMs == 0u) tmoMs = EPC_ONE_TMO_MS;
    if (tmoMs > 10000u) tmoMs = 10000u;
    uint32_t winMs = (winMaxMs == 0u) ? UNLK_HOLD_BASE_MS : winMaxMs;
    if (winMs > UNLK_HOLD_MAX_MS) winMs = UNLK_HOLD_MAX_MS;
    uint32_t holdMs = (holdTopMs == 0u) ? EPC_HOLD_TOP_MS : holdTopMs;
    if (holdMs > EPC_HOLD_TOP_MAX_MS) holdMs = EPC_HOLD_TOP_MAX_MS;

    /* 保持期连续盘存强制会话 S0 (同 0x0A, Round_098 #21) */
    (void)App_UHF_ScanSessionBegin();
    epc_run(epc, epcLen, tmoMs, winMs, holdMs, timeoutSec, channel, out);
    out->total = 1u;
    out->confirmed = s_confirmed;
    out->confirmedBitmap = s_bitmap;
    out->softCnt = s_cycle;                 /* 终帧: 周期数 */
    out->softDone = s_cycleOk;              /* 终帧: 成功周期数 */
    if (out->elapsedMs == 0u && s_irMs != 0u)
        out->elapsedMs = SysTickHl_GetMs() - s_irMs;
    App_UHF_ScanSessionEnd();
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), Finish 再做终清 */
    App_RgbLedPat_Clear(RGBSRC_UNLOCK);
}

/* =====================================================================
 * AM 标签解锁流程 (LOCKER_SUB_UNLOCK_AM 0x11) — Round_013 新增独立通道。
 * 受理起 tmo 秒窗内被动计数 AM 消磁成功事件 (cmd17 突发结算,
 * deactCount 增量), 达标 (>=amCnt) 即结账 — 全程不动电机 (2026-09-19
 * 裁决 "AM 解锁完成后不要动作电机, 直接蜂鸣器动作即可"): 蜂鸣 1s +
 * 绿三闪, rise/lower 恒 0; 窗满未达标按超时结账 (部分完成数如实上报)。
 * amCnt=0 支路: 无计数门 (窗循环 0>=0 立即出窗), 受理即达标结账 ALL_OK
 * (不耗窗, tmo 仅作格式合法校验)。
 * AM 解码器零控制 (2026-09-19 裁决 "AM 设备从上电到结束不要控制, 按照
 * 默认参数即可, 仅在解锁期间监控解锁帧"): 受理不探链/不切模式/不下发
 * 参数, 流程仅被动收 cmd17 (LockerSeek_Pump 内 App_AM_Process)。
 * 解码器静默 (含挂起/自锁) 只能按窗满结账, 不再报 AM_LINK。
 * CANCEL: 窗内即回 ABORTED (无电机, 无安全回落问题)。
 * 全程不触碰 UHF (前置检查不含 UHF, 无 ScanSession/失联监测)。
 * cmd17 事件须静默 AM_BURST_GAP_MS(3s) 才结算入账: 窗满时若有
 * 突发未结算, 宽限至多 3s 等其入账 (窗末消磁的标签不得漏计)。
 * ===================================================================== */

/* 计数入账: deactCount 增量超过已入账数 -> 逐张推 0x0E + 绿闪
 * (识别蜂鸣 2026-09-19 裁决取消, 用户 "不用了") */
static void am_count_take(uint32_t deactBase)
{
    uint32_t k = App_AM_GetDeactCount() - deactBase;
    if (k > 255u) k = 255u;
    if ((uint8_t)k > s_softDone) {
        uint8_t before = s_softDone;
        s_softDone = (uint8_t)k;
        for (uint8_t i = before + 1u; i <= s_softDone && i != 0u; i++)
            push_soft(i);                            /* 0x0E 每解锁一张 */
        App_RgbLedPat_Flash(RGBFLASH_SOFT_OK);
    }
    s_confirmed = s_softDone;        /* GET_PROGRESS: confirmed=已解锁数 */
}

static void am_run(uint8_t amCnt, uint16_t tmoSec, uint8_t channel,
                   LockerUnlockResult_t *out)
{
    s_busy = 1u;
    s_abort = 0u;
    s_immune = 0u;
    s_phase = UNLK_PH_SOFT;
    s_channel = channel;
    s_total = amCnt;
    s_confirmed = 0u;
    s_bitmap = 0u;
    s_softCnt = amCnt;               /* GET_PROGRESS/0x0E: softCnt=目标数 */
    s_softDone = 0u;
    s_risen = 0u;
    s_irMs = 0u;
    s_cycle = 0u; s_cycleOk = 0u; s_lastCyc = 0u;
    /* 0x10 记录残留清零 (GET_PROGRESS tagPresent / IO_DIAG 读这些槽) */
    s_tag[0].len = 0u;
    s_tag[0].state = UTAG_PENDING;
    s_tag[0].hits = 0u;
    s_tag[0].trackStart = 0u;
    s_tag[0].lastSeen = 0u;
    LockerSeek_BindAbort(&s_abort, &s_immune);
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) s_fgn[i].len = 0u;

    /* ---- 前置检查: 仅业务空闲 (2026-09-19 裁决全程不动电机 — 不再
     * 要求步进空闲/已回零, 电机状态与本流程无耦合) ---- */
    if (App_Locker_IsIdle() == 0)                    { fill_busy(out); return; }

    /* ---- AM 基线 (2026-09-19 裁决 "AM 设备从上电到结束不要控制"):
     * 不探链/不切模式/不下发任何参数 — 解码器按自身默认参数运行,
     * 流程仅被动监控 cmd17 结算 (LockerSeek_Pump 内 App_AM_Process
     * 收帧入账)。基线取受理时刻计数, 流程外自发消磁不入账。 ---- */
    uint32_t deactBase = App_AM_GetDeactCount();
    uint32_t failBase  = App_AM_GetFailCount();

    /* ---- 受理: 计时基准=受理时刻, 窗=tmo 秒; 计数窗白常亮 (0x0A 软标先例) ---- */
    uint32_t winMs = (uint32_t)tmoSec * 1000u;
    uint32_t t0 = SysTickHl_GetMs();
    s_irMs = t0;                     /* 流程耗时基准 = 受理时刻 */
    push_start(winMs, UNLK_PH_SOFT); /* 受理帧 0x0F (纯软标同款 phase=SOFT) */
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_SOFT_WAIT_WHITE);

    uint8_t endR = 0u;               /* 流程结束码 AM_END_* (0=未结束) */

    /* ---- 计数窗: 达标 (>=) 优先于窗满; 窗满时若有 cmd17 突发未结算,
     * 宽限至多 AM_BURST_GAP_MS 等其入账 (成功事件须静默 3s 才结算,
     * 窗末消磁的标签不得漏计; 无突发在途则立即按超时结账)。
     * amCnt=0: 0>=0 首查即出窗 (直接完成支路, 不耗窗) ---- */
    while (endR == 0u) {
        LockerSeek_Pump();
        if (s_abort) { endR = AM_END_ABORTED; break; }
        am_count_take(deactBase);
        if (s_softDone >= amCnt) break;              /* >=: 达标即触发 (超量亦然) */
        uint32_t el = SysTickHl_GetMs() - t0;
        if (el >= winMs &&
            (el >= winMs + AM_BURST_GAP_MS || App_AM_BurstActive() == 0u)) {
            endR = AM_END_TIMEOUT;
            break;
        }
    }

    if (endR == 0u) {
        /* ---- 达标: 不动作电机 (2026-09-19 裁决 "直接蜂鸣器动作
         * 即可") — 蜂鸣 1s + 绿三闪即结账 ---- */
        endR = AM_END_ALL_OK;
        App_NewPeriph_BeepPulse(EPC_BEEP_FLOW_DONE_MS);
        App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);
    } else if (endR == AM_END_TIMEOUT) {
        App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 窗满未收齐: 黄慢闪 2s */
    }

    /* ---- 统一结账: err=0 + endReason; 全程未动电机 rise/lower=0 ---- */
    s_phase = UNLK_PH_DONE;
    out->err = UNLK_ERR_OK;
    out->endReason = endR;
    out->riseSteps  = 0u;
    out->lowerSteps = 0u;
    {   /* 消磁失败事件数 (连发突发; 仅 err=0 终帧带出) */
        uint32_t f = App_AM_GetFailCount() - failBase;
        if (f > 255u) f = 255u;
        out->amFail = (uint8_t)f;
    }
}

void App_LockerUnlock_AmRun(uint8_t amCnt, uint16_t tmoSec, uint8_t channel,
                            LockerUnlockResult_t *out)
{
    /* 入口语义: amCnt 0~255 (0=直接完成支路: 0>=0 首查即达标结账
     * ALL_OK, 不耗窗), tmo u16 秒 (>0) */
    if (!out || tmoSec == 0u) {
        if (out) { Memset8((void*)out, 0, sizeof(*out)); out->err = UNLK_ERR_PARAM; }
        return;
    }
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = UNLK_RETREAT_NONE;
    if (tmoSec > AM_TMO_MAX_S) tmoSec = AM_TMO_MAX_S;   /* winMs 3 字节界内 */
    am_run(amCnt, tmoSec, channel, out);
    out->total = amCnt;                 /* 终帧: 目标数 */
    out->confirmed = s_softDone;        /* 终帧: 实际解锁数 */
    out->softCnt = amCnt;
    out->softDone = s_softDone;
    if (out->elapsedMs == 0u && s_irMs != 0u)
        out->elapsedMs = SysTickHl_GetMs() - s_irMs;
    /* 2026-09-19 裁决 "AM 设备从上电到结束不要控制": 出口不再切回
     * 仅检测 — 解码器全程按自身默认参数/自主状态运行。 */
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), Finish 再做终清 */
    App_RgbLedPat_Clear(RGBSRC_UNLOCK);
}
