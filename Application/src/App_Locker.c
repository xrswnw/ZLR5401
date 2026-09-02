#include "App_Locker.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_MotorHoming.h"
#include "App_NewPeriph_HL.h"
#include "App_AM.h"
#include "App_Led_HL.h"
#include "App_RgbLed_Pattern.h"
#include "App_BootSelfTest.h"   /* App_SelfTest_SetErrBits: 故障联动设备错误位 */
#include "App_SysTick_HL.h"
#include "App_CustomProtocol.h"   /* Memset8 */
#include <stddef.h>

/* =====================================================================
 * 开锁器业务编排层 — 依《约束/Link.txt》运行逻辑实现的状态机。
 *
 *  事件流 (事件经环形缓冲入队, 上位机经 LOCKER_SUB_GET_EVENT 拉取):
 *    LOCKER_EVT_MATCH_OK    硬标签匹配, 亮绿灯升起开锁
 *    LOCKER_EVT_MISMATCH    读到非清单 EPC, 红灯闪烁 + 不升起
 *    LOCKER_EVT_HARD_DONE   硬标签 m==n 全部解锁, 进入软标阶段
 *    LOCKER_EVT_SOFT_USED   消耗一次软标解码数量
 *    LOCKER_EVT_TIMEOUT     阶段窗口超时
 *    LOCKER_EVT_DONE        软标次数用尽, 结账完成
 *    LOCKER_EVT_FAULT       电机/链路故障
 *
 *  状态机迁移 (非阻塞节拍驱动):
 *    IDLE --CONFIGURE/ADD--> CONFIGURED --START--> CONFIGURED(扫描中)
 *    CONFIGURED --首匹配--> UNLOCK_HOLD --m==n--> SOFT_DECODE
 *    SOFT_DECODE --softUsed==softCount--> DONE --5s--> LOWERING --> IDLE
 *    CONFIGURED/UNLOCK_HOLD/SOFT --超时/取消--> LOWERING(升起中) --> IDLE
 *    任意运行态 --电机/链路故障--> FAULT --CANCEL--> LOWERING/IDLE
 *
 *  磁块升降: KEY_UP/KEY_DOWN 行程开关寻触 (s_motor 子状态机, 判据与
 *  App_MotorHoming / App_LockerOneShot 一致: 2000Hz/40%, 停滞/超步/
 *  超时兜底 + 失败重试一次)。旧定步数方案已废弃。
 * ===================================================================== */

#define LOCKER_EVT_QUEUE   8u

/* 电机子状态 */
#define M_NONE    0u
#define M_RISE    1u
#define M_LOWER   2u

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
static uint32_t        s_stageStartMs;   /* 进入当前状态的时刻 */
static uint8_t         s_started;        /* START 已激活 (ADD 门控) */
static uint8_t         s_motor;          /* M_NONE/M_RISE/M_LOWER */
static uint8_t         s_seekRetried;    /* 起步瞬态误停已重试一次 */
static uint32_t        s_seekT0, s_seekStallMs, s_seekSeq;
static uint32_t        s_amDeactSeen;    /* 软标阶段 AM 消磁成功计数基线 (闪显判定) */
static uint32_t        s_amFailSeen;     /* 软标阶段 AM 消磁失败计数基线 */
static uint8_t         s_uhfRecover;       /* UHF ERROR 链路恢复尝试次数 */

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

/* 阶段窗口到期 (差值式比较, SysTick 回绕安全) */
static uint8_t deadline_hit(uint32_t now)
{
    return (s_ctx.holdDeadlineMs != 0u &&
            (int32_t)(now - s_ctx.holdDeadlineMs) >= 0);
}

static void window_refresh(uint32_t now)
{
    uint32_t win = APP_LOCKER_BASE_HOLD_MS +
                   (uint32_t)(s_ctx.hardCount > 0u ? s_ctx.hardCount - 1u : 0u)
                   * APP_LOCKER_EXTRA_PER_TAG_MS;
    s_ctx.holdDeadlineMs = now + win;
}

/* ---- EPC 相等判断 ---- */
static uint8_t epc_eq(const AppLockerItem_t *it, const uint8_t *epc, uint8_t len)
{
    if (it->epcLen != len) return 0;
    for (uint8_t i = 0; i < len; i++) if (it->epc[i] != epc[i]) return 0;
    return 1;
}

/* ---- 电机寻触子状态机 (非阻塞节拍版, 判据同 App_MotorHoming) ---- */

/* 朝 dir 寻触启动 (dir 0=上 KEY_UP, 1=下 KEY_DOWN)。0=已启动, -1=启动失败 */
static int seek_start(uint8_t dir)
{
    (void)App_Stepper_Stop();
    (void)App_Stepper_SetSpeedHz(APP_LOCKER_SEEK_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(APP_LOCKER_SEEK_TORQUE_PCT);
    AppStepperMove_t mv;
    mv.dir = dir;
    mv.steps = 0u;                       /* 持续运行, 由触点/兜底停 */
    s_seekT0 = SysTickHl_GetMs();
    s_seekStallMs = s_seekT0;
    s_seekSeq = App_Stepper_GetStepsDone();
    s_seekRetried = 0u;
    if (App_Stepper_Move(&mv) != 0) { s_motor = M_NONE; return -1; }
    s_motor = (dir != 0u) ? M_LOWER : M_RISE;
    return 0;
}

/* 寻触节拍: 0=运行中 1=触到并停住 2=失败 (停滞/超步/超时/保护停机) */
static uint8_t seek_tick(uint8_t dir, uint32_t maxSteps)
{
    int hit = (dir != 0u) ? (App_NewPeriph_ReadKeyDown() == 0u)
                          : (App_NewPeriph_ReadKeyUp()   == 0u);
    if (hit) {
        (void)App_Stepper_DcBrakeStop();
        (void)App_Stepper_ClearFault();
        s_motor = M_NONE;
        return 1u;
    }
    uint32_t now = SysTickHl_GetMs();
    uint32_t seq = App_Stepper_GetStepsDone();
    if (seq != s_seekSeq) { s_seekSeq = seq; s_seekStallMs = now; }
    if ((now - s_seekStallMs) >= APP_LOCKER_SEEK_STALL_MS ||
        seq >= maxSteps ||
        App_Stepper_GetState() == APP_STEPPER_IDLE ||
        (now - s_seekT0) >= APP_LOCKER_SEEK_TMO_MS) {
        (void)App_Stepper_Stop();
        /* 失败重试一次: 覆盖上电首驱 nFAULT 瞬态误停与启动全失步 (实测脉冲
         * 走满整腿而机构未动, TRQ/腿长兜底停机; 紧随其后的同参数驱动即正常,
         * MotorTest 8 腿全过佐证机构无卡死)。重试仍失败按真实故障上报。 */
        if (s_seekRetried == 0u) {
            s_seekRetried = 1u;
            (void)App_Stepper_ClearFault();
            AppStepperMove_t mv;
            mv.dir = dir;
            mv.steps = 0u;
            s_seekT0 = s_seekStallMs = now;
            if (App_Stepper_Move(&mv) == 0) return 0u;
        }
        s_motor = M_NONE;
        return 2u;
    }
    return 0u;
}

/* ---- 故障入口: 停机停扫, 上报, FAULT 常亮; CANCEL 可退出 ----
 * reason (临时诊断, 定位后可移除): 1=UHF链路 2=未回零 3=seek启动失败
 * 4=升寻触失败(CONFIGURED/HOLD) 5=升寻触失败(SOFT) 6=升寻触失败(DONE)
 * 7=回降寻触失败 8=teardown回降启动失败 */
static uint8_t s_faultReason;
static void locker_enter_fault(uint8_t reason)
{
    s_faultReason = reason;
    /* 设备级自检错误位联动: UHF 链路故障 -> UHF_COMM;
     * 未回零(行程自检未通过) -> TRAVEL_SW. 供 FC_SELFTEST_CTRL (0x0F) 读. */
    if (reason == 1u) App_SelfTest_SetErrBits(SELF_ERR_UHF_COMM);
    if (reason == 2u) App_SelfTest_SetErrBits(SELF_ERR_TRAVEL_SW);
    if (s_motor != M_NONE) s_ctx.lockRisen = 1u;  /* 升/降途中故障: CANCEL 仍须回降 */
    (void)App_Stepper_Stop();
    s_motor = M_NONE;
    (void)App_UHF_Stop();
    App_UHF_ScanSessionEnd();     /* Round_098 #21: 任务终止, 还原用户 session */
    evr_push(LOCKER_EVT_FAULT, (const uint8_t *)0, 0u);
    LedHl_EOn();
    set_state(LOCKER_FAULT);
}

/* ---- 拆除到 IDLE: 磁块升起中先寻触回降 (LOWERING), 否则直接 IDLE ---- */
static void teardown_to_idle(void)
{
    (void)App_UHF_Stop();
    App_UHF_ScanSessionEnd();     /* Round_098 #21: 扫描窗结束, 还原用户 session */
    s_started = 0u;
    if (s_motor != M_NONE || s_ctx.lockRisen) {
        if (seek_start(1u) == 0) {
            LedHl_EOff();
            set_state(LOCKER_LOWERING);
            return;
        }
        locker_enter_fault(8u);       /* 回降都发不动 -> 故障 */
        return;
    }
    LedHl_EOff();
    set_state(LOCKER_IDLE);
}

/* MISMATCH 红闪指示已迁移至 RGB 灯带 (App_RgbLedPat_Flash RGBFLASH_MISMATCH_3S),
 * ERR 灯不再参与失配指示, 专职设备级故障常亮 (Round_010 决策 D1)。 */

void App_Locker_Init(void)
{
    Memset8((void*)&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = LOCKER_IDLE;
    s_ring.used = 0; s_ring.head = 0; s_ring.tail = 0;
    s_stageStartMs = 0;
    s_started = 0u; s_motor = M_NONE; s_seekRetried = 0u; s_faultReason = 0u;
    s_uhfRecover = 0u;
    s_seekT0 = 0u; s_seekStallMs = 0u; s_seekSeq = 0u;
    s_amDeactSeen = 0u; s_amFailSeen = 0u;
}

int App_Locker_Configure(const AppLockerItem_t *items, uint16_t hardCount,
                         uint16_t softCount)
{
    if (hardCount > APP_LOCKER_MAX_HARD) return -2;   /* 超上限 */
    if (App_Locker_IsIdle() == 0) return -1;          /* 忙, 需先取消 */
    Memset8((void*)&s_ctx, 0, sizeof(s_ctx));
    s_ctx.hardCount  = hardCount;
    s_ctx.softCount  = softCount;
    App_UHF_ClearTags();                     /* 新任务边界: 丢弃上一任务的残留标签 */
    for (uint16_t i = 0; i < hardCount && items; i++) {
        s_ctx.items[i] = items[i];
        if (s_ctx.items[i].epcLen > APP_LOCKER_MAX_EPC)
            s_ctx.items[i].epcLen = APP_LOCKER_MAX_EPC;
    }
    s_started = 0u;
    s_uhfRecover = 0u;
    set_state(LOCKER_CONFIGURED);
    evr_init();
    return 0;
}

int App_Locker_AddTag(const AppLockerItem_t *item)
{
    if (!item) return -2;
    if (s_started) return -1;                 /* START 后清单不可再改 */
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
    if (s_started) return -1;                 /* 已激活, 需先取消 */
    if (s_ctx.hardCount == 0u && s_ctx.softCount == 0u) return -2;
    if (App_MotorHoming_GetStatus() == HOMING_STAT_RUNNING) return -1;
                                             /* Round_098 #11: 后台回零中 -> BUSY
                                              * (避免扫描期 fault=2 硬故障) */
    s_started = 1u;
    if (s_ctx.hardCount == 0u) {
        /* 纯软标任务: 无硬标签可比对, 直入软标阶段 */
        s_ctx.holdDeadlineMs = SysTickHl_GetMs() + APP_LOCKER_SOFT_WINDOW_MS;
        s_amDeactSeen = App_AM_GetDeactCount();   /* AM 事件闪显基线 */
        s_amFailSeen  = App_AM_GetFailCount();
        LedHl_EOff();
        set_state(LOCKER_SOFT_DECODE);
        return 0;
    }
    /* Round_098 #21: 扫描窗内强制会话 S0 (S2/S3 下硬标签盘点标志在连续
     * 场脉冲间不复位, 首读后连续重扫永无匹配且无错误码), 窗结束还原.
     * 先于 Open: 模块未上电时仅改 RAM, Open 内的配置下发即为 S0. */
    (void)App_UHF_ScanSessionBegin();
    if (App_UHF_GetState() != APP_UHF_READY) {
        (void)App_UHF_Open();
    }
    (void)App_UHF_Inventory();
    window_refresh(SysTickHl_GetMs());
    LedHl_EOff();
    set_state(LOCKER_CONFIGURED);
    return 0;
}

int App_Locker_Cancel(void)
{
    teardown_to_idle();
    return 0;
}

int App_Locker_StopDecode(void)
{
    if (s_ctx.state != LOCKER_SOFT_DECODE) return -1;
    if (s_ctx.softUsed >= s_ctx.softCount) return -1;
    s_ctx.softUsed++;
    evr_push(LOCKER_EVT_SOFT_USED, (const uint8_t *)0, 0u);
    App_RgbLedPat_Flash(RGBFLASH_SOFT_OK);      /* 软标消耗一个: 白/黄单闪 */
    if (s_ctx.softUsed >= s_ctx.softCount) {
        evr_push(LOCKER_EVT_DONE, (const uint8_t *)0, 0u);
        App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);   /* 结账完成: 绿三连闪 */
        App_NewPeriph_BeepPulse(APP_LOCKER_DONE_BEEP_MS);
        s_ctx.holdDeadlineMs = SysTickHl_GetMs() + APP_LOCKER_DONE_IDLE_MS;
        set_state(LOCKER_DONE);
    }
    return 0;
}

/* ---- 扫描+比对节拍 (CONFIGURED / UNLOCK_HOLD 共用) ----
 * 清单匹配 -> MATCH_OK + 升起(首次); 已解锁标签重复读 -> 忽略;
 * 非清单 -> MISMATCH (红灯时间片闪烁); m==n -> 软标; 超时 -> 回降。 */
static void match_scan_process(uint32_t now)
{
    /* 链路故障. ERROR 常因上一轮被中止的 0x22 窗口未结束 (模块忙,
     * 新命令 700ms 无应答): 先探测恢复 (App_UHF_Query 成功自动
     * ERROR->READY), 3 次失败才判真链路故障。 */
    if (App_UHF_GetLinkStatus() != 0 || App_UHF_GetState() == APP_UHF_ERROR) {
        if (s_uhfRecover < 3u) {
            s_uhfRecover++;
            (void)App_UHF_Query();
            return;                     /* 本拍不推进, 恢复后下拍重扫 */
        }
        locker_enter_fault(1u);
        return;
    }
    s_uhfRecover = 0u;
    if (!App_UHF_IsBusy()) (void)App_UHF_Inventory();

    /* 升起寻触节拍 */
    if (s_motor == M_RISE) {
        uint8_t r = seek_tick(0u, APP_LOCKER_SEEK_RISE_MAX);
        if (r == 1u)      { s_ctx.lockRisen = 1u; }
        else if (r == 2u) { locker_enter_fault(4u); return; }
    }

    /* 取一条放置的硬标签 (一次一个) */
    AppUHFTag_t tag;
    if (App_UHF_TagTake(&tag) == 0) {
        App_RgbLedPat_Flash(RGBFLASH_TAG_SEEN);   /* 读到一张标签: 蓝单闪 (Round_011 A) */
        /* 清单比对: 先未解锁项 (新匹配), 再已解锁项 (重复读不算失配) */
        int idx = -1;
        uint8_t seenMatched = 0u;
        for (uint16_t i = 0; i < s_ctx.hardCount; i++) {
            if (!epc_eq(&s_ctx.items[i], tag.epc, tag.epcLen)) continue;
            if (s_ctx.items[i].matched) { seenMatched = 1u; }
            else                        { idx = (int)i; }
            break;
        }
        if (idx >= 0) {
            s_ctx.items[idx].matched = 1u;
            s_ctx.hardMatched++;
            s_ctx.holdTagIndex = (uint16_t)idx;
            evr_push(LOCKER_EVT_MATCH_OK, tag.epc, tag.epcLen);
            App_RgbLedPat_Flash(RGBFLASH_MATCH_OK);   /* 这张对了: 绿单闪 */
            window_refresh(now);           /* 匹配即刷新保持窗 */
            /* 首匹配: 寻触升起 (须已回零 + 电机空闲) */
            if (s_motor == M_NONE && !s_ctx.lockRisen) {
                if (App_MotorHoming_IsReady() == 0) { locker_enter_fault(2u); return; }
                if (seek_start(0u) != 0)           { locker_enter_fault(3u); return; }
                set_state(LOCKER_UNLOCK_HOLD);
            }
            if (s_ctx.hardMatched >= s_ctx.hardCount) {
                evr_push(LOCKER_EVT_HARD_DONE, (const uint8_t *)0, 0u);
                if (s_ctx.softCount == 0u) {
                    /* 无软标需求 -> 直接结账完成 */
                    evr_push(LOCKER_EVT_DONE, (const uint8_t *)0, 0u);
                    App_RgbLedPat_Flash(RGBFLASH_DONE_3GREEN);   /* 完成: 绿三连闪 */
                    App_NewPeriph_BeepPulse(APP_LOCKER_DONE_BEEP_MS);
                    s_ctx.holdDeadlineMs = now + APP_LOCKER_DONE_IDLE_MS;
                    set_state(LOCKER_DONE);
                } else {
                    /* 软标阶段兜底窗口 (磁块保持升起直至 DONE 回降) */
                    s_amDeactSeen = App_AM_GetDeactCount();   /* AM 事件闪显基线 */
                    s_amFailSeen  = App_AM_GetFailCount();
                    s_ctx.holdDeadlineMs = now + APP_LOCKER_SOFT_WINDOW_MS;
                    set_state(LOCKER_SOFT_DECODE);
                }
            }
        } else if (seenMatched == 0u) {
            /* 非清单 EPC: RGB 红闪 3s, 不升起, 上报 (ERR 灯不参与, D1) */
            evr_push(LOCKER_EVT_MISMATCH, tag.epc, tag.epcLen);
            App_RgbLedPat_Flash(RGBFLASH_MISMATCH_3S);
        }
        /* seenMatched: 已解锁标签重复读, 不算失配, 静默忽略 */
    }

    /* 阶段窗口超时: 回降并结束 */
    if (deadline_hit(now)) {
        evr_push(LOCKER_EVT_TIMEOUT, (const uint8_t *)0, 0u);
        teardown_to_idle();
    }
}

/* ---- 主循环节拍 ---- */
static void locker_process_idle(void) { }

static void locker_process_configured(void)
{
    match_scan_process(SysTickHl_GetMs());
}

static void locker_process_hold(void)
{
    /* UNLOCK_HOLD: 磁块已升起 (或寻触中), 继续比对剩余硬标签 */
    match_scan_process(SysTickHl_GetMs());
}

static void locker_process_soft(void)
{
    /* 软标阶段: v1 依赖上位机 LOCKER_SUB_CONSUME_SOFT 触发消耗。
     * 磁块保持升起; 若还在寻触上升, 继续走完。 */
    uint32_t now = SysTickHl_GetMs();
    if (s_motor == M_RISE) {
        uint8_t r = seek_tick(0u, APP_LOCKER_SEEK_RISE_MAX);
        if (r == 1u)      { s_ctx.lockRisen = 1u; }
        else if (r == 2u) { locker_enter_fault(5u); return; }
    }
    LedHl_EOff();
    /* AM 消磁事件闪显: 成功=白单闪(逐个反馈), 失败=红双闪(重放提示) */
    uint32_t deact = App_AM_GetDeactCount();
    if (deact != s_amDeactSeen) {
        s_amDeactSeen = deact;
        App_RgbLedPat_Flash(RGBFLASH_SOFT_OK);
    }
    uint32_t fail = App_AM_GetFailCount();
    if (fail != s_amFailSeen) {
        s_amFailSeen = fail;
        App_RgbLedPat_Flash(RGBFLASH_FAIL_DOUBLE);
    }
    if (deadline_hit(now)) {
        evr_push(LOCKER_EVT_TIMEOUT, (const uint8_t *)0, 0u);
        teardown_to_idle();
    }
}

static void locker_process_done(void)
{
    uint32_t now = SysTickHl_GetMs();
    if (s_motor == M_RISE) {   /* 最后一次匹配时仍在上升 -> 走完再回降 */
        uint8_t r = seek_tick(0u, APP_LOCKER_SEEK_RISE_MAX);
        if (r == 1u)      { s_ctx.lockRisen = 1u; }
        else if (r == 2u) { locker_enter_fault(6u); return; }
    }
    LedHl_EOff();
    if (deadline_hit(now)) {
        teardown_to_idle();   /* 结账完成后回降 -> IDLE */
    }
}

static void locker_process_lowering(void)
{
    /* 回降寻触: 到位回 IDLE; 失败 -> FAULT (CANCEL 可再试) */
    if (s_motor == M_LOWER) {
        uint8_t r = seek_tick(1u, APP_LOCKER_SEEK_LOWER_MAX);
        if (r == 1u) {
            s_ctx.lockRisen = 0u;
            s_started = 0u;
            LedHl_EOff();
            set_state(LOCKER_IDLE);
        } else if (r == 2u) {
            locker_enter_fault(7u);
        }
        return;
    }
    /* 寻触未在跑 (异常): 视为已回降 */
    s_ctx.lockRisen = 0u;
    s_started = 0u;
    LedHl_EOff();
    set_state(LOCKER_IDLE);
}

static void locker_process_fault(void)
{
    /* 故障保持: ERR 常亮; 停扫停机已在入口完成。CANCEL 退出
     * (磁块升起中会先走 LOWERING 回降)。 */
    LedHl_EOn();
}

/* ---- RGB 灯语稳态映射 (每拍声明, 幂等; 仲裁见 App_RgbLed_Pattern) ----
 *  扫描段(CONFIGURED/HOLD, 未在升起) -> SCAN_ACTIVE 蓝慢闪: 盘点校对进行中 (Round_011 A)
 *  寻触上升中 -> RISE_HOLD_GREEN 绿常亮 (升起到位后回扫描色, 继续等余下标签)
 *  软标阶段(未在上升) -> SOFT_WAIT_WHITE 白常亮: 轮到软标(红外/消磁)
 *  FAULT -> 红常亮; IDLE/DONE/LOWERING -> 灭 */
static void pat_sync(void)
{
    uint8_t pat;
    switch (s_ctx.state) {
    case LOCKER_CONFIGURED:
    case LOCKER_UNLOCK_HOLD:
        pat = (s_motor == M_RISE) ? RGBPAT_RISE_HOLD_GREEN : RGBPAT_SCAN_ACTIVE;
        break;
    case LOCKER_SOFT_DECODE:
        pat = (s_motor == M_RISE) ? RGBPAT_RISE_HOLD_GREEN : RGBPAT_SOFT_WAIT_WHITE;
        break;
    case LOCKER_FAULT:
        pat = RGBPAT_FAULT_SOLID;
        break;
    default:        /* IDLE / DONE / LOWERING */
        pat = RGBPAT_OFF;
        break;
    }
    App_RgbLedPat_Set(RGBSRC_LOCKER, pat);
}

void App_Locker_Process(void)
{
    switch (s_ctx.state) {
    case LOCKER_IDLE:        locker_process_idle(); break;
    case LOCKER_CONFIGURED:  locker_process_configured(); break;
    case LOCKER_UNLOCK_HOLD: locker_process_hold(); break;
    case LOCKER_SOFT_DECODE: locker_process_soft(); break;
    case LOCKER_DONE:        locker_process_done(); break;
    case LOCKER_FAULT:       locker_process_fault(); break;
    case LOCKER_LOWERING:    locker_process_lowering(); break;
    default: break;
    }
    pat_sync();
}

AppLockerState_t App_Locker_GetState(void) { return s_ctx.state; }
uint8_t App_Locker_GetFaultReason(void) { return s_faultReason; }
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
