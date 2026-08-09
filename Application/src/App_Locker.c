#include "App_Locker.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_AM.h"
#include "App_Led_HL.h"
#include "App_SysTick_HL.h"
#include "App_CustomProtocol.h"   /* Memset8 */
#include <stddef.h>

/* =====================================================================
 * 开锁器业务编排层 — 依《约束/Link.txt》运行逻辑实现的状态机。
 *
 *  事件流 (事件经 App_Locker_TrackEvent 入环形缓冲, 上位机经
 *  LOCKER_SUB_GET_EVENT 拉取, 匹配结果/异常均上报):
 *    LOCKER_EVT_MATCH_OK    硬标签匹配, 亮绿灯升起开锁
 *    LOCKER_EVT_MISMATCH    读到非清单 EPC, 红灯闪烁 + 不升起
 *    LOCKER_EVT_HARD_DONE   硬标签 m==n 全部解锁, 进入软标阶段
 *    LOCKER_EVT_SOFT_USED   消耗一次软标解码数量
 *    LOCKER_EVT_TIMEOUT     开锁保持窗口超时
 *    LOCKER_EVT_DONE        软标次数用尽, 结账完成
 *    LOCKER_EVT_FAULT       电机/链路故障
 *
 *  状态机迁移:
 *    IDLE --SET/ADD+START--> CONFIGURED --m==n--> SOFT_DECODE --次数尽--> DONE --> IDLE
 *    CONFIGURED/SOFT --CANCEL或TIMEOUT--> IDLE
 *    电机/链路故障 --> FAULT --恢复+清障--> IDLE
 * ===================================================================== */

#define LOCKER_EVT_QUEUE   8u

typedef struct {
    uint8_t used;
    uint8_t head, tail;
    struct {
        uint8_t code;                       /* AppLockerEvent_t */
        uint8_t epc[APP_LOCKER_MAX_EPC];
        uint8_t epcLen;
        uint16_t hardMatched, softUsed, softCount, hardCount;
        uint32_t tMs;
    } ev[LOCKER_EVT_QUEUE];
} LockerEventRing_t;

static AppLockerCtx_t  s_ctx;
static LockerEventRing_t s_ring;
static uint32_t        s_stageStartMs;   /* 进入硬/软阶段的时刻 */
static uint8_t         s_lastRed;

/* 事件环形缓冲 */
static void evr_init(void) { s_ring.used = 0; s_ring.head = 0; s_ring.tail = 0; }
static void evr_push(uint8_t code, const uint8_t *epc, uint8_t epcLen)
{
    if (s_ring.used >= LOCKER_EVT_QUEUE) {   /* 满则丢弃最旧 */
        s_ring.tail = (s_ring.tail + 1u) % LOCKER_EVT_QUEUE;
        s_ring.used--;
    }
    s_ring.ev[s_ring.head].code = code;
    s_ring.ev[s_ring.head].hardMatched = s_ctx.hardMatched;
    s_ring.ev[s_ring.head].softUsed    = s_ctx.softUsed;
    s_ring.ev[s_ring.head].softCount   = s_ctx.softCount;
    s_ring.ev[s_ring.head].hardCount   = s_ctx.hardCount;
    s_ring.ev[s_ring.head].tMs         = SysTickHl_GetMs();
    s_ring.ev[s_ring.head].epcLen = (epcLen > APP_LOCKER_MAX_EPC) ? APP_LOCKER_MAX_EPC : epcLen;
    for (uint8_t i = 0; i < s_ring.ev[s_ring.head].epcLen; i++)
        s_ring.ev[s_ring.head].epc[i] = epc[i];
    s_ring.head = (s_ring.head + 1u) % LOCKER_EVT_QUEUE;
    s_ring.used++;
}

static void set_state(AppLockerState_t st)
{
    s_ctx.state = st;
    s_stageStartMs = SysTickHl_GetMs();
}

/* 磁块控制 */
static void rise_lock(void)
{
    if (!s_ctx.lockRisen) {
        AppStepperMove_t mv = { 0u, APP_LOCKER_STEP_RISE };  /* dir 0=CW 升起 */
        if (App_Stepper_Move(&mv) == 0) s_ctx.lockRisen = 1;
    }
}
static void lower_lock(void)
{
    if (s_ctx.lockRisen) {
        AppStepperMove_t mv = { 1u, APP_LOCKER_STEP_LOWER }; /* dir 1=CCW 下降 */
        (void)App_Stepper_Move(&mv);
        s_ctx.lockRisen = 0;
    }
}

/* EPC 相等判断 */
static uint8_t epc_eq(const AppLockerItem_t *it, const uint8_t *epc, uint8_t len)
{
    if (it->epcLen != len) return 0;
    for (uint8_t i = 0; i < len; i++) if (it->epc[i] != epc[i]) return 0;
    return 1;
}

void App_Locker_Init(void)
{
    Memset8((void*)&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = LOCKER_IDLE;
    s_ring.used = 0; s_ring.head = 0; s_ring.tail = 0;
    s_stageStartMs = 0; s_lastRed = 0;
}

int App_Locker_Configure(const AppLockerItem_t *items, uint16_t hardCount,
                         uint16_t softCount)
{
    if (hardCount > APP_LOCKER_MAX_HARD) return -2;   /* 超上限 */
    if (hardCount == 0u && softCount == 0u) return -2;
    if (App_Locker_IsIdle() == 0) return -1;          /* 忙, 需先取消 */
    lower_lock();
    Memset8((void*)&s_ctx, 0, sizeof(s_ctx));
    s_ctx.hardCount  = hardCount;
    s_ctx.softCount  = softCount;
    for (uint16_t i = 0; i < hardCount && items; i++) {
        s_ctx.items[i] = items[i];
        if (s_ctx.items[i].epcLen > APP_LOCKER_MAX_EPC)
            s_ctx.items[i].epcLen = APP_LOCKER_MAX_EPC;
    }
    set_state(LOCKER_CONFIGURED);
    evr_init();
    return 0;
}

int App_Locker_AddTag(const AppLockerItem_t *item)
{
    if (!item) return -2;
    if (s_ctx.state != LOCKER_CONFIGURED && s_ctx.state != LOCKER_IDLE) return -1;
    if (s_ctx.hardCount >= APP_LOCKER_MAX_HARD) return -2;
    s_ctx.items[s_ctx.hardCount] = *item;
    if (s_ctx.items[s_ctx.hardCount].epcLen > APP_LOCKER_MAX_EPC)
        s_ctx.items[s_ctx.hardCount].epcLen = APP_LOCKER_MAX_EPC;
    s_ctx.items[s_ctx.hardCount].matched = 0u;
    s_ctx.hardCount++;
    set_state(LOCKER_CONFIGURED);
    return 0;
}

int App_Locker_Start(void)
{
    if (s_ctx.hardCount == 0u && s_ctx.softCount == 0u) return -2;
    /* 上电 UHF 并扫描 */
    if (App_UHF_GetState() != APP_UHF_READY) {
        (void)App_UHF_Open();
    }
    (void)App_UHF_Inventory();
    /* 开锁保持窗口: 2min + 每多一标签 +30s */
    uint32_t win = APP_LOCKER_BASE_HOLD_MS +
                   (uint32_t)(s_ctx.hardCount > 0u ? s_ctx.hardCount - 1u : 0u)
                   * APP_LOCKER_EXTRA_PER_TAG_MS;
    s_ctx.holdDeadlineMs = SysTickHl_GetMs() + win;
    LedHl_GOn();               /* 绿灯: 可开始 */
    set_state(LOCKER_CONFIGURED);
    return 0;
}

int App_Locker_Cancel(void)
{
    (void)App_UHF_Stop();
    lower_lock();
    LedHl_GOff();
    set_state(LOCKER_IDLE);
    return 0;
}

int App_Locker_StopDecode(void)
{
    if (s_ctx.state != LOCKER_SOFT_DECODE) return -1;
    if (s_ctx.softUsed >= s_ctx.softCount) return -1;
    s_ctx.softUsed++;
    evr_push(LOCKER_EVT_SOFT_USED, (const uint8_t *)0, 0u);
    if (s_ctx.softUsed >= s_ctx.softCount) {
        evr_push(LOCKER_EVT_DONE, (const uint8_t *)0, 0u);
        s_ctx.holdDeadlineMs = SysTickHl_GetMs() + APP_LOCKER_DONE_IDLE_MS;
        set_state(LOCKER_DONE);
    }
    return 0;
}

void App_Locker_NotifyTag(void) { /* 光电触发预留: v1 由 UHF 缓冲轮询驱动 */ }

/* ---- 主循环节拍 ---- */
static void locker_process_idle(void) { }

static void locker_process_configured(void)
{
    /* 保持 UHF 扫描 */
    if (App_UHF_GetLinkStatus() != 0 || App_UHF_GetState() == APP_UHF_ERROR) {
        evr_push(LOCKER_EVT_FAULT, (const uint8_t *)0, 0u);
        set_state(LOCKER_FAULT);
        return;
    }
    if (!App_UHF_IsBusy()) (void)App_UHF_Inventory();

    /* 取一条放置的硬标签 (一次一个) */
    AppUHFTag_t tag;
    if (App_UHF_TagTake(&tag) != 0) return;

    /* 在未解锁清单中找匹配 */
    int idx = -1;
    for (uint16_t i = 0; i < s_ctx.hardCount; i++) {
        if (!s_ctx.items[i].matched && epc_eq(&s_ctx.items[i], tag.epc, tag.epcLen)) {
            idx = (int)i; break;
        }
    }
    if (idx >= 0) {
        s_ctx.items[idx].matched = 1u;
        s_ctx.hardMatched++;
        s_ctx.holdTagIndex = (uint16_t)idx;
        evr_push(LOCKER_EVT_MATCH_OK, tag.epc, tag.epcLen);
        LedHl_GOn();
        /* 刷新保持截止: 匹配即升起, 窗口内保持 */
        uint32_t win = APP_LOCKER_BASE_HOLD_MS +
                       (s_ctx.hardCount > 0u ? s_ctx.hardCount - 1u : 0u)
                       * APP_LOCKER_EXTRA_PER_TAG_MS;
        s_ctx.holdDeadlineMs = SysTickHl_GetMs() + win;
        if ((App_Stepper_GetState() == APP_STEPPER_IDLE) && !s_ctx.lockRisen) {
            rise_lock();
        }
        if (s_ctx.hardMatched >= s_ctx.hardCount) {
            evr_push(LOCKER_EVT_HARD_DONE, (const uint8_t *)0, 0u);
            s_ctx.holdDeadlineMs = 0;   /* 硬阶段结束, 解除窗口 */
            set_state(LOCKER_SOFT_DECODE);
            if (s_ctx.softCount == 0u) {
                /* 无软标需求 → 直接结账完成 */
                evr_push(LOCKER_EVT_DONE, (const uint8_t *)0, 0u);
                s_ctx.holdDeadlineMs = SysTickHl_GetMs() + APP_LOCKER_DONE_IDLE_MS;
                set_state(LOCKER_DONE);
            }
        }
    } else {
        /* 非清单 EPC: 红灯闪烁, 不升起, 上报 */
        evr_push(LOCKER_EVT_MISMATCH, tag.epc, tag.epcLen);
        s_lastRed = !s_lastRed;
        LedHl_GSetBrightness(s_lastRed ? 999u : 0u);
    }

    /* 开锁窗口超时: 回降并结束 */
    if (s_ctx.holdDeadlineMs != 0u &&
        (SysTickHl_GetMs() >= s_ctx.holdDeadlineMs)) {
        evr_push(LOCKER_EVT_TIMEOUT, (const uint8_t *)0, 0u);
        App_Locker_Cancel();
    }
}

static void locker_process_soft(void)
{
    /* 软标阶段: v1 依赖上位机 LOCKER_SUB_CONSUME_SOFT 触发消耗。
     * 此处仅维持绿灯表示可继续使用解码器。 */
    LedHl_GOn();
    if (s_ctx.holdDeadlineMs != 0u &&
        (SysTickHl_GetMs() >= s_ctx.holdDeadlineMs)) {
        evr_push(LOCKER_EVT_TIMEOUT, (const uint8_t *)0, 0u);
        App_Locker_Cancel();
    }
}

static void locker_process_done(void)
{
    LedHl_GOn();
    if (s_ctx.holdDeadlineMs != 0u &&
        (SysTickHl_GetMs() >= s_ctx.holdDeadlineMs)) {
        App_Locker_Cancel();   /* 结账完成后回 IDLE */
    }
}

static void locker_process_fault(void)
{
    lower_lock();
    LedHl_GOn();               /* 故障: 常亮指示 (无独立红灯, 用绿灯全亮区分)*/
}

void App_Locker_Process(void)
{
    switch (s_ctx.state) {
    case LOCKER_IDLE:        locker_process_idle(); break;
    case LOCKER_CONFIGURED:  locker_process_configured(); break;
    case LOCKER_SOFT_DECODE: locker_process_soft(); break;
    case LOCKER_DONE:        locker_process_done(); break;
    case LOCKER_FAULT:       locker_process_fault(); break;
    default: break;
    }
}

AppLockerState_t App_Locker_GetState(void) { return s_ctx.state; }
void App_Locker_GetCtx(AppLockerCtx_t *ctx) { if (ctx) *ctx = s_ctx; }
int  App_Locker_IsIdle(void) { return (s_ctx.state == LOCKER_IDLE) ? 1 : 0; }

/* ---- 事件拉取 (协议层) ---- */
int App_Locker_PopEvent(uint8_t *code, uint8_t *epc, uint8_t *epcLen,
                        uint16_t *hardMatched, uint16_t *softUsed,
                        uint16_t *softCount, uint16_t *hardCount)
{
    if (s_ring.used == 0u) return -1;
    s_ring.used--;
    uint8_t i = s_ring.tail;
    if (code)        *code        = s_ring.ev[i].code;
    if (epcLen)      *epcLen      = s_ring.ev[i].epcLen;
    if (epc)         for (uint8_t k = 0; k < s_ring.ev[i].epcLen; k++) epc[k] = s_ring.ev[i].epc[k];
    if (hardMatched) *hardMatched = s_ring.ev[i].hardMatched;
    if (softUsed)    *softUsed    = s_ring.ev[i].softUsed;
    if (softCount)   *softCount   = s_ring.ev[i].softCount;
    if (hardCount)   *hardCount   = s_ring.ev[i].hardCount;
    s_ring.tail = (s_ring.tail + 1u) % LOCKER_EVT_QUEUE;
    return 0;
}
