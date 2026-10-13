#include "App_LockerUnlock.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_MotorHoming.h"
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
static UnlkTagRec_t s_tag[EPC_MAX_TAGS];   /* Round_014: 名单容量 10 (0x0A 上限仍 4) */
static FgnTagRec_t  s_fgn[UNLK_MAX_FGN_TAGS];
static uint8_t  s_total, s_confirmed, s_bitmap;
static uint8_t  s_softCnt, s_softDone;
static uint32_t s_irMs;               /* 流程计时基准 (受理时刻) */
static uint32_t s_demagBase;           /* 消磁计数基线 (softCnt>0) */
static uint8_t  s_amDemagOn;           /* 软标段已切消磁模式 (Run 出口切回检测) */
static uint8_t  s_risen;
/* ---- 0x0A 保留实现专用 (unlk_run 监守路径; 流程已关闭) ---- */
static uint8_t  s_cycle, s_cycleOk, s_lastCyc;
static uint16_t s_oneTmo;
static uint8_t  s_msMiss, s_msFgn;
static uint8_t  s_vMiss;
static uint8_t  s_reHits;
/* ---- EPC 解锁 (0x10) Round_014 新状态机 ---- */
static volatile uint8_t s_mph;        /* UNLK_MPH_* 电机子状态 */
static uint8_t  s_validCnt;           /* 当前 CONFIRMED 名单标签数 (有效子集大小) */
static uint8_t  s_fgnActive;          /* 外来标签介入中 */
static uint8_t  s_alarmActive;        /* 异常报警中 */
static uint32_t s_alarmBeepMs;        /* 报警蜂鸣上次触发时刻 */
static uint8_t  s_riseCnt, s_alarmCnt, s_fgnSeen, s_tagsFound;
static volatile uint8_t s_amDemagFlag;  /* 0x11: 消磁模式运行中 (进度 flags) */

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
    if (!s_busy) return;                   /* 空闲: 全零 (status=0x00) */
    p->status |= UNLK_ST_BUSY;
    /* 0x10: tag=当前 CONFIRMED 有效子集张数 (s_validCnt 每拍重算;
     * 不用 s_confirmed — 它仅在 TRACKING->CONFIRMED 跃迁时递增,
     * 已确认标签的后续拍不重新计数, 不能反映"当前在场" (Round_016 修)。
     * 0x11: s_confirmed=已解锁数 (am_count_take 维护) */
    p->tag       = s_amDemagFlag ? s_confirmed : s_validCnt;
    p->riseCnt   = s_riseCnt;
    p->alarmCnt  = s_alarmCnt;
    p->lastCycle = s_lastCyc;
    if (s_irMs != 0u) {
        uint32_t es = (SysTickHl_GetMs() - s_irMs) / 1000u;
        p->elapsed = (es > 0xFFu) ? 0xFFu : (uint8_t)es;
    }
    /* Round_016 v3 statusBits: 0x10 全量; 0x11 仅 TAG(=消磁在线, am_run 维护
     * s_validCnt=0 使 TAG/IR/FGN/ALARM 自然为零) */
    if (s_amDemagFlag) {
        p->status |= UNLK_ST_TAG;           /* 0x11: 消磁模式在线+收帧中 */
        return;                             /* 0x11 其余位恒 0 */
    }
    if (s_validCnt > 0u)  p->status |= UNLK_ST_TAG;   /* 0x10: 有效子集在场 */
    if (App_NewPeriph_ReadIr() != 0u) p->status |= UNLK_ST_IR;
    p->status |= (uint8_t)(s_mph << 3);     /* bit3-4: 电机状态/位置 */
    if (s_fgnActive)   p->status |= UNLK_ST_FGN;
    if (s_alarmActive) p->status |= UNLK_ST_ALARM;
    if (App_UHF_GetState() == APP_UHF_ERROR || App_UHF_GetLinkStatus() != 0)
        p->status |= UNLK_ST_UHF;           /* 失联累计中 (5s 未恢复->终帧 4) */
}

/* ---- 推送帧 (阻塞期间随时经原通道直发, func=FC_LOCKER_CTRL^0xFF) ---- */

static void push_start(uint32_t winMs, uint8_t status)
{
    uint8_t r[6];
    r[0] = LOCKER_SUB_EVT_START;
    r[1] = UNLK_ERR_OK;
    r[2] = status;                  /* Round_016 v3: statusBits (原 phase) */
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
    /* [Round_017] 老清单业务废除: lockerState 恒 0 (终帧 BUSY 诊断字节
     * 布局不变, 上位机兼容) */
    out->lockerState  = 0u;
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

    /* ---- ⑴ 前置检查 (Round_017: 老清单业务废除, Locker 占用检查移除;
     * 互斥由本流程 s_busy + Dispatch 门禁承担) ---- */
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

    push_start(winMs, UNLK_ST_BUSY);            /* 受理帧 (0x0A 关闭路径, 保留实现) */

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

/* 回退段监守 (0x0A 保留实现专用, 流程已关闭): 期望标签连续
 * EPC_CONFIRM_READS 发读回 -> 中停回退; 红外未遮挡不计回归。 */
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

/* =====================================================================
 * EPC 解锁流程 (LOCKER_SUB_UNLOCK_EPC 0x10) — Round_013 独立通道,
 * Round_014 (2026-09-29) 按客户联调诉求整体重设计:
 *   受理 (tmo 秒流程窗 + 名单 1~10 张) -> 0x22 多标签连续盘点 ->
 *   名单任意非空子集稳定在场 (EPC_STABLE_MS) + 红外遮挡 + 无外来介入
 *   -> 升起 (监守) -> 在场即保持 (不设固定时长) ->
 *   异常 (外来介入 / 名单全离场): 报警 (红 50/50 闪 + 蜂鸣 50ms/500ms)
 *   + 快速下行 -> 恢复后经红外门再升, 循环直至超窗。
 *   超窗在顶 -> 回降 + ALL_OK + 蜂鸣 1s; 否则 TIMEOUT。
 *   CANCEL -> 安全回降 + 全状态复原 + ABORTED。
 * ===================================================================== */

/* 名单标签记录: PENDING -> TRACKING (首读起持续在场 EPC_STABLE_MS)
 * -> CONFIRMED (计入有效子集); 断读超 EPC_ABSENT_MS 回 PENDING (离场)。
 * CONFIRMED 非单向掩码 (Round_014): 离场即撤销, 重新稳定再计入。 */
static void epc2_tag_update(uint32_t now)
{
    /* ---- 收割本轮盘点结果, 刷新在读记录 ---- */
    AppUHFTag_t tag;
    while (App_UHF_TagTake(&tag) == 0) {
        UnlkTagRec_t *t = rec_find(&tag);
        if (t != NULL) {
            if (t->state == UTAG_PENDING) {
                t->state = UTAG_TRACKING;
                t->trackStart = now;
                t->hits = 0u;
            }
            t->hits++;
            t->lastSeen = now;
        } else {
            if (s_tagsFound < 255u) s_tagsFound++;
            FgnTagRec_t *g = fgn_track(&tag, now);
            if (g != NULL) {
                g->hits++;
                g->lastSeen = now;
                if (g->reported == 0u && g->hits >= EPC_FGN_MIN_HITS) {
                    /* 外来介入成立: 一事件一帧 0x0C (槽释放后再次介入重报) */
                    g->reported = 1u;
                    push_mismatch(g->epc, g->len, g->hits);
                    s_fgnSeen = 1u;
                }
            }
        }
    }

    /* ---- 名单状态推进 + 有效子集统计 ---- */
    s_validCnt = 0u;
    s_confirmed = 0u;
    s_bitmap = 0u;
    for (uint8_t i = 0; i < s_total; i++) {
        UnlkTagRec_t *t = &s_tag[i];
        if (t->state == UTAG_TRACKING) {
            if ((now - t->lastSeen) > EPC_ABSENT_MS) {   /* 断读: 重新累计 */
                t->state = UTAG_PENDING;
                t->hits = 0u;
            } else if ((now - t->trackStart) >= EPC_STABLE_MS) {
                t->state = UTAG_CONFIRMED;               /* 稳定在场 */
                s_confirmed++;
                s_bitmap |= (uint8_t)(1u << i);
                push_tag(t);                             /* 0x0B 确认帧 */
                App_RgbLedPat_Flash(RGBFLASH_MATCH_OK);
            }
        }
        if (t->state == UTAG_CONFIRMED) {
            if ((now - t->lastSeen) > EPC_ABSENT_MS) {   /* 离场: 撤销确认 */
                t->state = UTAG_PENDING;
                t->hits = 0u;
            } else {
                s_validCnt++;                            /* 有效子集成员 */
                s_bitmap |= (uint8_t)(1u << i);
            }
        }
    }

    /* ---- 外来介入判定 (槽释放 = 稳定消失) ---- */
    s_fgnActive = 0u;
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) {
        FgnTagRec_t *g = &s_fgn[i];
        if (g->len == 0u) continue;
        if ((now - g->lastSeen) > EPC_ABSENT_MS) {      /* 稳定消失: 释放槽 */
            g->len = 0u;
            continue;
        }
        if (g->hits >= EPC_FGN_MIN_HITS) s_fgnActive = 1u;
    }

    /* ---- 盘点接力: 本函数在每个泵拍路径 (主循环/电机腿监守) 都被调用,
     * 在此处接力保证升起/回降腿期间盘点不停 (0x22 一轮结束 -> 下一轮) ---- */
    if (!App_UHF_IsBusy() && App_UHF_GetState() == APP_UHF_READY)
        (void)App_UHF_InventoryTimeout(EPC_ONE_TMO_MS);
}

/* 异常报警节奏 (Round_014 裁决): 触发即红 50/50 闪 (RGBPAT_ALARM_BLINK
 * 稳态声明, 高于一切常规图样) + 蜂鸣 50ms 每 500ms; 解除即恢复阶段稳态。
 * Round_020 (2026-09-30) 用户裁决: 报警 (红闪+蜂鸣) 仅外来介入触发;
 * 名单全离场 = 静默正常回降, 不再闪红。 */
static void epc2_steady_led(uint8_t flowPh, uint8_t irStable)
{
    if (flowPh != 0u)
        App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_RISE_HOLD_GREEN);   /* 在顶: 绿常亮 */
    else
        App_RgbLedPat_Set(RGBSRC_UNLOCK,
                          irStable ? RGBPAT_SCAN_ACTIVE : RGBPAT_IR_WAIT);
}

static void epc2_alarm_poll(uint32_t now)
{
    /* Round_020: 红闪+蜂鸣仅外来介入; 名单全失静默 (由 UP 段独立判全失快降) */
    uint8_t alarmNow = s_fgnActive ? 1u : 0u;
    if (alarmNow != s_alarmActive) {
        s_alarmActive = alarmNow;
        if (alarmNow) {
            s_alarmCnt++;
            App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_ALARM_BLINK);
        } else {
            s_alarmBeepMs = 0u;                          /* 解除: 停蜂鸣 */
        }
    }
    if (s_alarmActive) {
        if (s_alarmBeepMs == 0u || (now - s_alarmBeepMs) >= EPC_ALARM_BEEP_PERIOD_MS) {
            App_NewPeriph_BeepPulse(EPC_ALARM_BEEP_MS);
            s_alarmBeepMs = now;
        }
    }
}

/* IR 双向去抖 (Round_020 裁决: 红外丢失立马下降, 红外回归+稳定标签才升;
 * 双向共用 UNLK_IR_CONFIRM_MS 去抖挡毛刺) */
static uint32_t s_irHighSince;     /* 遮挡持续起点 (0=无) */
static uint32_t s_irLowSince;      /* 丢失持续起点 (0=无) */
static uint8_t  s_irStable;        /* 遮挡去抖成立 (升起门) */
static uint8_t  s_irLost;          /* 丢失去抖成立 (回降门) */
static void epc2_ir_poll(uint32_t now)
{
    if (App_NewPeriph_ReadIr() != 0u) {
        if (s_irHighSince == 0u) s_irHighSince = now;
        s_irLowSince = 0u;
        s_irStable = ((now - s_irHighSince) >= UNLK_IR_CONFIRM_MS) ? 1u : 0u;
        s_irLost = 0u;
    } else {
        if (s_irLowSince == 0u) s_irLowSince = now;
        s_irHighSince = 0u;
        s_irLost = ((now - s_irLowSince) >= UNLK_IR_CONFIRM_MS) ? 1u : 0u;
        s_irStable = 0u;
    }
}

/* 升起段监守: 收割标签 + 报警节奏; 外来介入/名单全失/红外丢失即停机收起。 */
static int epc2_rise_monitor(void)
{
    uint32_t now = SysTickHl_GetMs();
    epc2_tag_update(now);
    epc2_alarm_poll(now);
    epc2_ir_poll(now);
    return (s_fgnActive || s_validCnt == 0u || s_irLost) ? 1 : 0;
}

/* 回降段监守: 只收割/报警 (恢复判定交回主循环), 不中停回降。 */
static int epc2_passive_monitor(void)
{
    uint32_t now = SysTickHl_GetMs();
    epc2_tag_update(now);
    epc2_alarm_poll(now);
    return 0;
}

/* 异常快降: 免疫 CANCEL, 到底后回 WAIT (报警解除由主循环判定)。 */
static int epc2_alarm_lower(uint32_t *lower, LockerUnlockResult_t *out)
{
    s_mph = UNLK_MPH_LOWERING;
    s_phase = UNLK_PH_LOWER;
    /* 回降原因: 3=外来介入 2=名单全失 4=红外丢失 (Round_020 新增) */
    s_lastCyc = s_fgnActive ? 3u : ((s_validCnt == 0u) ? 2u : 4u);
    s_immune = 1u;
    LockerSeek_SetMonitor(epc2_passive_monitor);
    int r = LockerSeek_RunRetry(LSEEK_DIR_DOWN, LSEEK_LOWER_MAX, lower);
    LockerSeek_SetMonitor(NULL);
    s_immune = 0u;
    s_risen = 0u;
    s_mph = UNLK_MPH_NONE;
    if (r != 0u) {
        motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                  : UNLK_ERR_MOTOR_TIMEOUT,
                   2u, out);
        return -1;
    }
    return 0;
}

/* EpcRun 已完成名单展开 (s_tag/s_total) 与参数校验 */
static void epc_run(uint16_t tmoSec, uint8_t channel, LockerUnlockResult_t *out)
{
    s_busy = 1u;
    s_abort = 0u;
    s_immune = 0u;
    s_phase = UNLK_PH_WAIT_TAG;
    s_channel = channel;
    s_confirmed = 0u;
    s_bitmap = 0u;
    s_softCnt = 0u;
    s_softDone = 0u;
    s_risen = 0u;
    s_irMs = 0u;
    s_demagBase = 0u;
    s_amDemagOn = 0u;
    s_lastCyc = 0u;
    s_mph = UNLK_MPH_NONE;
    s_validCnt = 0u;
    s_fgnActive = 0u;
    s_alarmActive = 0u;
    s_alarmBeepMs = 0u;
    s_irHighSince = 0u;
    s_irLowSince = 0u;
    s_irStable = 0u;
    s_irLost = 0u;
    s_riseCnt = 0u;
    s_alarmCnt = 0u;
    s_fgnSeen = 0u;
    s_tagsFound = 0u;
    LockerSeek_BindAbort(&s_abort, &s_immune);
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) s_fgn[i].len = 0u;

    /* ---- 前置检查 (UHF/电机占用与回零就绪; Round_017 起 Locker 占用
     * 检查移除 — 老清单业务废除, 互斥由 s_busy + 门禁承担) ---- */
    if (App_UHF_GetState() == APP_UHF_SCAN)          { fill_busy(out); return; }
    if (App_UHF_IsBusy())                            { fill_busy(out); return; }
    if (App_Stepper_GetState() != APP_STEPPER_IDLE)  { fill_busy(out); return; }
    if (App_MotorHoming_GetStatus() == HOMING_STAT_RUNNING) {
        fill_busy(out);
        return;
    }
    if (App_MotorHoming_IsReady() == 0) {
        out->err = UNLK_ERR_HOMING;
        out->switchErr = App_Stepper_GetSwitchErr();
        return;
    }

    /* ---- UHF 就绪 + 开盘 (ERROR 态彻底下电重上) ---- */
    if (App_UHF_GetState() == APP_UHF_ERROR) {
        (void)App_UHF_Close();
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = UNLK_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);
            return;
        }
    } else if (App_UHF_GetState() != APP_UHF_READY) {
        out->uhfRawErr = App_UHF_Open();
        if (out->uhfRawErr != APP_UHF_ERR_OK) {
            out->err = UNLK_ERR_UHF_OPEN;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);
            return;
        }
    }
    App_UHF_ClearTags();
    if (s_abort) { out->err = UNLK_ERR_OK; out->endReason = EPC_END_ABORTED; return; }

    uint32_t flowMs = (uint32_t)tmoSec * 1000u;
    uint32_t flowT0 = SysTickHl_GetMs();
    s_irMs = flowT0;                     /* 流程耗时基准 = 受理时刻 */
    push_start(flowMs, UNLK_ST_BUSY);            /* 受理帧 0x0F (v3: statusBits) */
    (void)App_Stepper_SetSpeedHz(LSEEK_SPEED_HZ);   /* Round_014: 2000µ步/s */
    (void)App_Stepper_SetTorquePercent(LSEEK_TORQUE_PCT);
    (void)App_UHF_InventoryTimeout(EPC_ONE_TMO_MS); /* 常态连续多标签盘点 */

    uint32_t rise = 0u, lower = 0u;
    uint8_t  endR = 0u;                   /* EPC_END_* (0=未结束) */
    uint8_t  flowPh = 0u;                 /* 0=WAIT 1=UP */
    uint32_t lostStart = 0u;
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_IR_WAIT);   /* 待放标: 白慢闪 */

    while (endR == 0u) {
        LockerSeek_Pump();
        if (s_abort) { endR = EPC_END_ABORTED; break; }
        uint32_t now = SysTickHl_GetMs();

        epc2_tag_update(now);

        /* IR 双向去抖 (Round_020 裁决: 丢失 -> 回降门; 回归+稳定标签 -> 升起门) */
        epc2_ir_poll(now);
        uint8_t irStable = s_irStable;
        epc2_alarm_poll(now);
        if (!s_alarmActive) epc2_steady_led(flowPh, irStable);  /* 解除后恢复 */

        /* UHF 失联 5s 确认 -> UHF_LOST */
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

        /* 超窗: 在顶 (含升起中腿完成) -> ALL_OK (统一出口回降);
         * 未升起 -> TIMEOUT (磁块未动/异常回降后未恢复) */
        if ((now - flowT0) >= flowMs) {
            endR = s_risen ? EPC_END_ALL_OK : EPC_END_TIMEOUT;
            break;
        }

        if (flowPh == 0u) {
            /* ---- WAIT: 有效子集 + IR 遮挡 + 无介入 -> 升起 ---- */
            if (!s_alarmActive && s_validCnt > 0u && irStable) {
                s_risen = 1u;
                s_mph = UNLK_MPH_RISING;
                s_phase = UNLK_PH_RISE;
                App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_RISE_HOLD_GREEN);
                LockerSeek_SetMonitor(epc2_rise_monitor);
                int r = LockerSeek_RunRetry(LSEEK_DIR_UP, LSEEK_RISE_MAX, &rise);
                LockerSeek_SetMonitor(NULL);
                s_mph = UNLK_MPH_NONE;
                if (r == 3u) {           /* 升起中介入/全失: 报警快降回 WAIT */
                    if (epc2_alarm_lower(&lower, out) != 0) {
                        (void)App_UHF_Stop();
                        return;
                    }
                } else if (r == 2u) {    /* 升起中 CANCEL: 免疫回降交统一出口 */
                    endR = EPC_END_ABORTED;
                    break;
                } else if (r != 0u) {
                    (void)App_UHF_Stop();
                    motor_fail((App_Stepper_GetFault() != 0u) ? UNLK_ERR_MOTOR_FAULT
                                                              : UNLK_ERR_MOTOR_TIMEOUT,
                               1u, out);
                    return;
                } else {                 /* 到顶: 在场即保持 */
                    s_riseCnt++;
                    s_softCnt = s_riseCnt;
                    s_lastCyc = 1u;      /* 1=升至顶 */
                    s_mph = UNLK_MPH_UP;
                    s_phase = UNLK_PH_HOLD;
                    flowPh = 1u;
                    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_RISE_HOLD_GREEN);
                }
            }
        } else {
            /* ---- UP: 在场+IR 遮挡即保持; 外来介入报警快降, 名单全失/
             * 红外丢失静默快降 (Round_020: IR 丢失立马下降) ---- */
            if (s_alarmActive || s_validCnt == 0u || s_irLost) {
                if (epc2_alarm_lower(&lower, out) != 0) {
                    (void)App_UHF_Stop();
                    return;
                }
                flowPh = 0u;
                s_phase = UNLK_PH_WAIT_TAG;
            }
        }

    }

    /* ---- 统一出口 ---- */
    (void)App_UHF_Stop();
    out->err = UNLK_ERR_OK;
    out->endReason = endR;
    s_phase = UNLK_PH_DONE;
    s_mph = UNLK_MPH_NONE;
    if (s_risen) {                       /* ABORTED/UHF_LOST/超窗在顶: 回降 */
        if (lower_if_risen(&lower, out) != 0) return;
        s_risen = 0u;
    }
    out->riseSteps  = (rise  > 0xFFFFu) ? 0xFFFFu : (uint16_t)rise;
    out->lowerSteps = (lower > 0xFFFFu) ? 0xFFFFu : (uint16_t)lower;
    if (endR == EPC_END_ALL_OK) {
        App_NewPeriph_BeepPulse(EPC_BEEP_FLOW_DONE_MS);    /* 结束: 1s */
        App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);
    } else if (endR == EPC_END_TIMEOUT) {
        App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 超窗未达成升起: 黄慢闪 */
    }
}

void App_LockerUnlock_EpcRun(const uint8_t *packed, uint8_t epcCnt,
                             uint16_t packedLen, uint16_t tmoSec,
                             uint8_t channel, LockerUnlockResult_t *out)
{
    /* 参数校验: epcCnt 1~10, 每张 len 1~12 且数据长度吻合, 名单不重复,
     * tmo>0 (上限 EPC_TMO_MAX_S 钳位) */
    if (!out || !packed || epcCnt == 0u || epcCnt > EPC_MAX_TAGS || tmoSec == 0u) {
        if (out) { Memset8((void*)out, 0, sizeof(*out)); out->err = UNLK_ERR_PARAM; }
        return;
    }
    uint16_t pos = 0u;
    for (uint8_t i = 0; i < epcCnt; i++) {
        uint8_t len = packed[pos++];     /* 长度前缀 */
        if (len == 0u || len > UNLK_EPC_MAX || (uint16_t)(pos + len) > packedLen) {
            Memset8((void*)out, 0, sizeof(*out));
            out->err = UNLK_ERR_PARAM;
            return;
        }
        s_tag[i].len = len;
        for (uint8_t k = 0; k < len; k++) s_tag[i].epc[k] = packed[pos + k];
        pos = (uint16_t)(pos + len);
    }
    for (uint8_t i = 0; i < epcCnt; i++)             /* 重复 EPC 拒绝 */
        for (uint8_t j = (uint8_t)(i + 1u); j < epcCnt; j++) {
            if (s_tag[i].len != s_tag[j].len) continue;
            uint8_t k;
            for (k = 0; k < s_tag[i].len; k++)
                if (s_tag[i].epc[k] != s_tag[j].epc[k]) break;
            if (k >= s_tag[i].len) {
                Memset8((void*)out, 0, sizeof(*out));
                out->err = UNLK_ERR_PARAM;
                return;
            }
        }
    for (uint8_t i = 0; i < epcCnt; i++) {           /* 记录复位 */
        s_tag[i].state = UTAG_PENDING;
        s_tag[i].hits = 0u;
        s_tag[i].trackStart = 0u;
        s_tag[i].lastSeen = 0u;
    }
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = UNLK_RETREAT_NONE;
    if (tmoSec > EPC_TMO_MAX_S) tmoSec = EPC_TMO_MAX_S;
    s_total = epcCnt;

    (void)App_UHF_ScanSessionBegin();    /* 连续盘存强制 S0 (Round_098 #21) */
    epc_run(tmoSec, channel, out);
    out->total = s_total;
    out->confirmed = s_confirmed;        /* 最终稳定在场名单标签数 */
    out->confirmedBitmap = s_bitmap;
    out->riseCnt = s_riseCnt;
    out->alarmCnt = s_alarmCnt;
    out->fgnSeen = s_fgnSeen;
    out->tagsFound = s_tagsFound;
    out->softCnt = s_riseCnt;            /* 终帧: 升至顶部次数 */
    out->softDone = s_alarmCnt;          /* 终帧: 报警次数 */
    if (out->elapsedMs == 0u && s_irMs != 0u)
        out->elapsedMs = SysTickHl_GetMs() - s_irMs;
    App_UHF_ScanSessionEnd();
    App_RgbLedPat_Clear(RGBSRC_UNLOCK);
}

/* =====================================================================
 * AM 标签解锁流程 (LOCKER_SUB_UNLOCK_AM 0x11) — Round_013 独立通道,
 * Round_014 (2026-09-29) 恢复 AM 控制逻辑 (客户裁决):
 *   受理 -> AM 切消磁 (SET_MODE 回显确认, 重试 AM_MODE_RETRY 次,
 *   全败 AM_LINK 结账) -> tmo 秒计数窗: 每条 cmd17 消磁帧立即入账一计
 *   ("收到一个消磁帧立马更新状态, 无需等待无需稳定, 它只有一次" —
 *   Round_014 裁决, 改按帧计数, 不再等 3s 突发静默结算) -> 达标/超窗 ->
 *   AM 挂起切仅检测 (确认失败不阻塞结账, amFail=1 如实上报) -> 终帧。
 *   全程不动电机 (2026-09-19 裁决维持): 蜂鸣 1s + 绿三闪,
 *   rise/lower 恒 0。amCnt=0: 不耗窗不碰 AM 直接 ALL_OK。
 *   CANCEL: 立即尽力挂起 + ABORTED。无 IR 门控; 全程不碰 UHF。
 * ===================================================================== */

/* SET_MODE 回显确认 (重试 AM_MODE_RETRY 次) */
static uint8_t am_set_mode_confirm(uint16_t mode)
{
    for (uint8_t i = 0; i < AM_MODE_RETRY; i++) {
        if (App_AM_SetParam(AM_CMD_MODE, mode) == APP_AM_ERR_OK) return 1u;
    }
    return 0u;
}

/* 计数入账 (Round_014 逐帧): cmd17 帧计数增量超过已入账数 -> 逐张推 0x0E
 * + 绿闪; 无需等待突发静默结算 */
static void am_count_take(uint32_t frameBase)
{
    uint32_t k = App_AM_GetEventCount() - frameBase;
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
    s_lastCyc = 0u;
    s_mph = UNLK_MPH_NONE;
    s_validCnt = 0u;
    s_fgnActive = 0u;
    s_alarmActive = 0u;
    /* 0x10 记录残留清零 (Round_016 补: v3 GetProgress 直接读这些槽) */
    s_riseCnt = 0u; s_alarmCnt = 0u; s_fgnSeen = 0u; s_tagsFound = 0u;
    s_tag[0].len = 0u;
    s_tag[0].state = UTAG_PENDING;
    s_tag[0].hits = 0u;
    s_tag[0].trackStart = 0u;
    s_tag[0].lastSeen = 0u;
    LockerSeek_BindAbort(&s_abort, &s_immune);
    for (uint8_t i = 0; i < UNLK_MAX_FGN_TAGS; i++) s_fgn[i].len = 0u;

    /* ---- 前置检查: 仅业务空闲 (全程不动电机; Round_017 起 Locker 检查移除) ---- */

    /* ---- Round_014 恢复控制: 受理即切消磁模式 (确认进入) ---- */
    uint8_t demagOn = 0u;
    if (amCnt > 0u) {
        if (am_set_mode_confirm(AM_MODE_DEACTIVATE) == 0u) {
            (void)am_set_mode_confirm(AM_MODE_DETECT_ONLY);   /* 尽力复原 */
            out->err = UNLK_ERR_AM_LINK;
            App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);       /* 链路断: 红双闪 */
            return;
        }
        demagOn = 1u;
        s_amDemagFlag = 1u;           /* GET_PROGRESS flags.DEMAG_ON */
    }

    /* ---- 计数窗 (逐帧即时入账, 无窗末宽限) ---- */
    uint32_t frameBase = App_AM_GetEventCount();   /* cmd17 帧计数基线 */
    uint32_t winMs = (uint32_t)tmoSec * 1000u;
    uint32_t t0 = SysTickHl_GetMs();
    s_irMs = t0;                     /* 流程耗时基准 = 受理时刻 */
    push_start(winMs, UNLK_ST_BUSY | UNLK_ST_TAG); /* 受理帧 0x0F (v3: 消磁在线) */
    App_RgbLedPat_Set(RGBSRC_UNLOCK, RGBPAT_SOFT_WAIT_WHITE);

    uint8_t endR = 0u;               /* AM_END_* (0=未结束) */
    /* Round_020 模式看门狗: 解码器可自退消磁模式 (热保护/自恢复, 真机证实
     * "计 2/3 后消磁场消失"), 窗内静默自愈 — 每逢无进行中消磁突发
     * (BurstActive=0 即距上一 cmd17 >=3s 结算静默) 重发 SET_MODE 保场。
     * 静默门控防 SetParam 的 RxFlush 吞活跃解码期的 cmd17 帧。 */
    uint32_t wdNextMs = SysTickHl_GetMs() + AM_MODE_WATCH_MS;
    while (endR == 0u) {
        LockerSeek_Pump();
        if (s_abort) { endR = AM_END_ABORTED; break; }
        am_count_take(frameBase);
        if (s_softDone >= amCnt) break;              /* >=: 达标即触发 (超量亦然) */
        if ((SysTickHl_GetMs() - t0) >= winMs) {
            endR = AM_END_TIMEOUT;
            break;
        }
        if (demagOn && s_softDone < amCnt &&
            (int32_t)(SysTickHl_GetMs() - wdNextMs) >= 0 &&
            App_AM_BurstActive() == 0u) {
            (void)am_set_mode_confirm(AM_MODE_DEACTIVATE);   /* 幂等重申 */
            wdNextMs = SysTickHl_GetMs() + AM_MODE_WATCH_MS;
        }
    }

    if (endR == 0u) {
        endR = AM_END_ALL_OK;
        App_NewPeriph_BeepPulse(EPC_BEEP_FLOW_DONE_MS);   /* 蜂鸣 1s */
        App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);
    } else if (endR == AM_END_TIMEOUT) {
        App_RgbLedPat_Flash(RGBFLASH_WARN_2S);   /* 窗满未收齐: 黄慢闪 2s */
    }

    /* ---- Round_014: 挂起切仅检测 (确认挂起; 失败 amFail=1 如实上报,
     * 解码器自行恢复消磁的风险客户已知, 无法根除) ---- */
    if (demagOn) {
        s_amDemagFlag = 0u;
        if (am_set_mode_confirm(AM_MODE_DETECT_ONLY) == 0u)
            out->amFail = 1u;        /* 挂起确认失败 (终帧带出) */
    }

    s_phase = UNLK_PH_DONE;
    out->err = UNLK_ERR_OK;
    out->endReason = endR;
    out->riseSteps  = 0u;
    out->lowerSteps = 0u;
}

void App_LockerUnlock_AmRun(uint8_t amCnt, uint16_t tmoSec, uint8_t channel,
                            LockerUnlockResult_t *out)
{
    /* 入口语义: amCnt 0~255 (0=直接完成支路: 不耗窗不碰 AM), tmo u16 秒 (>0) */
    if (!out || tmoSec == 0u) {
        if (out) { Memset8((void*)out, 0, sizeof(*out)); out->err = UNLK_ERR_PARAM; }
        return;
    }
    Memset8((void*)out, 0, sizeof(*out));
    out->retreat = UNLK_RETREAT_NONE;
    s_amDemagFlag = 0u;
    if (tmoSec > AM_TMO_MAX_S) tmoSec = AM_TMO_MAX_S;
    am_run(amCnt, tmoSec, channel, out);
    out->total = amCnt;                 /* 终帧: 目标数 */
    out->confirmed = s_softDone;        /* 终帧: 实际解锁数 */
    out->softCnt = amCnt;
    out->softDone = s_softDone;
    if (out->elapsedMs == 0u && s_irMs != 0u)
        out->elapsedMs = SysTickHl_GetMs() - s_irMs;
    /* 流程返回即撤销灯语声明 (含全部提前失败出口), Finish 再做终清 */
    App_RgbLedPat_Clear(RGBSRC_UNLOCK);
}
