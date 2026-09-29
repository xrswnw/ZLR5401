/* sim_stubs.c — 宿主机模拟环境: 虚拟时钟/剧本驱动的外设桩。
 * App_LockerUnlock.c 原样编译, 依赖的全部外部符号在此实现。 */
#include "sim_env.h"
#include "App_LockerUnlock.h"
#include "App_LockerSeek.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_MotorHoming.h"
#include "App_Locker.h"
#include "App_NewPeriph_HL.h"
#include "App_SysTick_HL.h"
#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_CustomProtocol.h"
#include "App_RgbLed_Pattern.h"
#include <stddef.h>
#include <stdio.h>

/* ================= 虚拟时钟 ================= */
uint32_t g_ms = 0u;
uint32_t SysTickHl_GetMs(void) { return g_ms; }

/* ================= 剧本 ================= */
SimScript g_script;
SimFrame  g_frames[256]; int g_nFrames;
SimBeep   g_beeps[2048];  int g_nBeeps;
SimLed    g_leds[16384];   int g_nLeds;
SimLeg    g_legs[64];    int g_nLegs;
SimAmMode g_amModes[32]; int g_nAmModes;
/* AM mode 确认重试也记 — 只记成功的 */
SimSample g_samples[32]; int g_nSamples;
uint32_t g_amEventCount;
uint16_t g_amMode;
uint32_t g_motorPos;
uint32_t g_speedHz = 4000u;   /* Round_015: 电机腿统一 4000µ步/s */

int g_checks, g_fails;
void CHECK(int cond, const char *msg)
{
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", msg); }
}
void CHECK_EQ(long got, long want, const char *msg)
{
    g_checks++;
    if (got != want) { g_fails++; printf("  FAIL: %s (got %ld want %ld)\n", msg, got, want); }
}

/* ================= 时间钩子 ================= */
typedef struct { uint32_t at; SimHookFn fn; int done; } SimHook;
static SimHook s_hooks[32]; static int s_nHooks;
void sim_at(uint32_t ms, SimHookFn fn)
{ s_hooks[s_nHooks].at = ms; s_hooks[s_nHooks].fn = fn; s_hooks[s_nHooks].done = 0; s_nHooks++; }
void sim_reset_hooks(void) { s_nHooks = 0; }

/* 进度采样钩子 (读 App_LockerUnlock_GetProgress 快照) */
static void sample_hook(void)
{
    LockerUnlockProgress_t p;
    App_LockerUnlock_GetProgress(&p);
    g_samples[g_nSamples].ms = g_ms;
    g_samples[g_nSamples].phase = p.phase;
    g_samples[g_nSamples].mph = p.motorPhase;
    g_samples[g_nSamples].flags = p.flags;
    g_samples[g_nSamples].valid = (p.flags & UNLK_FLG_VALID_STABLE) ? 1 : 0;
    g_samples[g_nSamples].fgn = (p.flags & UNLK_FLG_FGN_ACTIVE) ? 1 : 0;
    g_samples[g_nSamples].alarm = (p.flags & UNLK_FLG_ALARM) ? 1 : 0;
    g_nSamples++;
}
void sim_progress_at(uint32_t ms) { sim_at(ms, sample_hook); }
int  sim_n_samples(void) { return g_nSamples; }
uint32_t sim_sample_ms(int i) { return g_samples[i].ms; }

/* ================= 泵循环 (时序引擎) ================= */
static volatile uint8_t *s_abortFlag, *s_immuneFlag;
static int (*s_monitor)(void);

/* UHF 盘点轮次 */
static uint8_t  s_roundBusy;
static uint32_t s_roundDoneMs;
static int      s_tagQCnt;
static AppUHFTag_t s_tagQ[20];

/* 电机腿 */
static uint8_t  s_legActive;
static uint8_t  s_legDir;
static uint32_t s_legSteps;
static int      s_legResult;
static uint32_t s_legMaxSteps;

#define SIM_TOP_POS     8170u     /* 触点行程 (4085×2) */
#define SIM_BOTTOM_POS  0u
#define PUMP_MS         20u

static uint8_t ir_blocked(uint32_t now)
{
    if (g_script.nIr > 0u) {
        for (uint8_t i = 0; i < g_script.nIr; i++)
            if (now >= g_script.ir[i].t0 && now <= g_script.ir[i].t1) return 1u;
        return 0u;
    }
    for (uint8_t i = 0; i < g_script.nTags; i++)          /* 缺省: 有标签即遮挡 */
        if (now >= g_script.tags[i].t0 && now <= g_script.tags[i].t1) return 1u;
    return 0u;
}

static uint8_t uhf_bad(uint32_t now)
{
    for (uint8_t i = 0; i < g_script.nLink; i++)
        if (now >= g_script.link[i].t0 && now <= g_script.link[i].t1) return 1u;
    return 0u;
}

static void pump_tags_present(uint32_t now)
{
    for (uint8_t i = 0; i < g_script.nTags; i++) {
        SimTagSpan *s = &g_script.tags[i];
        if (now < s->t0 || now > s->t1) continue;
        AppUHFTag_t *t = &s_tagQ[s_tagQCnt++];
        t->epcLen = s->len;
        for (uint8_t k = 0; k < s->len; k++) t->epc[k] = s->epc[k];
        t->rssi = 200;
    }
}

void LockerSeek_Pump(void)
{
    uint32_t prev = g_ms;
    g_ms += PUMP_MS;

    /* 钩子 */
    for (int i = 0; i < s_nHooks; i++)
        if (!s_hooks[i].done && g_ms >= s_hooks[i].at &&
            prev <  s_hooks[i].at) { s_hooks[i].done = 1; s_hooks[i].fn(); }

    /* AM cmd17 帧 (时间驱动入账) */
    { uint32_t c = 0;
      for (uint8_t i = 0; i < g_script.nDemag; i++)
          if (g_script.demagMs[i] <= g_ms) c++;
      g_amEventCount = c; }

    /* UHF 盘点轮完成 -> 注入在场标签 */
    if (s_roundBusy && g_ms >= s_roundDoneMs) {
        s_roundBusy = 0u;
        pump_tags_present(g_ms);
    }

    /* 电机腿推进 */
    if (s_legActive) {
        uint32_t stepPerPump = (g_speedHz * PUMP_MS) / 1000u;
        uint32_t remain = (s_legDir == LSEEK_DIR_UP)
                          ? (SIM_TOP_POS - g_motorPos) : g_motorPos;
        if (stepPerPump > remain) stepPerPump = remain;
        g_motorPos = (s_legDir == LSEEK_DIR_UP) ? g_motorPos + stepPerPump
                                                : g_motorPos - stepPerPump;
        s_legSteps += stepPerPump;
        if (s_monitor && s_monitor()) {                 /* 监守请求停机 */
            s_legResult = 3; s_legActive = 0;
        } else if (s_abortFlag && *s_abortFlag && (!s_immuneFlag || !*s_immuneFlag)) {
            s_legResult = 2; s_legActive = 0;           /* CANCEL */
        } else if ((s_legDir == LSEEK_DIR_UP  && g_motorPos >= SIM_TOP_POS) ||
                   (s_legDir == LSEEK_DIR_DOWN && g_motorPos <= SIM_BOTTOM_POS)) {
            s_legResult = 0; s_legActive = 0;           /* 触点 */
        } else if (s_legSteps >= s_legMaxSteps) {
            s_legResult = 1; s_legActive = 0;           /* 超步 (不应发生) */
        }
    }
}

void LockerSeek_BindAbort(volatile uint8_t *a, volatile uint8_t *i)
{ s_abortFlag = a; s_immuneFlag = i; }
void LockerSeek_SetMonitor(int (*fn)(void)) { s_monitor = fn; }

int LockerSeek_RunRetry(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut)
{
    s_legActive = 1; s_legDir = dir; s_legSteps = 0;
    s_legResult = -1; s_legMaxSteps = maxSteps;
    while (s_legActive) LockerSeek_Pump();
    *stepsOut = s_legSteps;
    if (g_nLegs >= 64) { *stepsOut = s_legSteps; return s_legResult; }
    g_legs[g_nLegs].t0 = g_ms - s_legSteps * 1000u / (g_speedHz ? g_speedHz : 1u);
    g_legs[g_nLegs].t1 = g_ms;
    g_legs[g_nLegs].dir = dir;
    g_legs[g_nLegs].result = s_legResult;
    g_legs[g_nLegs].steps = s_legSteps;
    g_nLegs++;
    return s_legResult;
}

/* ================= UHF 桩 ================= */
static AppUHFState_t s_uhfState = APP_UHF_READY;
AppUHFState_t App_UHF_GetState(void)
{ return uhf_bad(g_ms) ? APP_UHF_ERROR : s_uhfState; }
int App_UHF_GetLinkStatus(void) { return uhf_bad(g_ms) ? 1 : 0; }
int App_UHF_IsBusy(void) { return s_roundBusy; }
int App_UHF_Open(void) { return g_script.uhfOpenFail ? APP_UHF_ERR_LINK : APP_UHF_ERR_OK; }
int App_UHF_Close(void) { s_uhfState = APP_UHF_IDLE; return APP_UHF_ERR_OK; }
int App_UHF_Stop(void) { s_roundBusy = 0; s_uhfState = APP_UHF_READY; return 0; }
void App_UHF_ClearTags(void) { s_tagQCnt = 0; }
int App_UHF_TagTake(AppUHFTag_t *t)
{
    if (s_tagQCnt <= 0) return -1;
    *t = s_tagQ[0];
    for (int i = 1; i < s_tagQCnt; i++) s_tagQ[i - 1] = s_tagQ[i];
    s_tagQCnt--;
    return 0;
}
int App_UHF_InventoryTimeout(uint16_t tmo)
{
    if (s_roundBusy) return APP_UHF_ERR_BUSY;
    s_tagQCnt = 0;                        /* 新一轮: 清上一轮残余 */
    s_roundBusy = 1;
    s_roundDoneMs = g_ms + (tmo ? tmo : 100u);
    return APP_UHF_ERR_OK;
}
int App_UHF_InventoryOneSync(uint16_t tmo, AppUHFTag_t *out, uint32_t *el)
{ (void)tmo; (void)out; (void)el; return APP_UHF_ERR_NO_TAG; }   /* 0x0A 保留路径 */
int App_UHF_ScanSessionBegin(void) { return 0; }
void App_UHF_ScanSessionEnd(void) {}

/* ================= 电机桩 ================= */
static AppStepperState_t s_stepState = APP_STEPPER_IDLE;
AppStepperState_t App_Stepper_GetState(void) { return s_stepState; }
int App_Stepper_SetSpeedHz(uint32_t hz) { g_speedHz = hz; return 0; }
int App_Stepper_SetTorquePercent(uint8_t pct) { (void)pct; return 0; }
uint8_t App_Stepper_GetFault(void) { return 0; }
uint8_t App_Stepper_GetDiag1(void) { return 0; }
uint8_t App_Stepper_GetDiag2(void) { return 0; }
uint8_t App_Stepper_GetSwitchErr(void) { return 0; }
uint32_t App_Stepper_GetStepsDone(void) { return s_legSteps; }

/* ================= 回零/业务状态桩 ================= */
uint8_t App_MotorHoming_GetStatus(void) { return HOMING_STAT_READY; }
int  App_MotorHoming_IsReady(void) { return 1; }
int  App_Locker_IsIdle(void) { return 1; }
AppLockerState_t App_Locker_GetState(void) { return 0; }

/* ================= 外设桩 ================= */
uint8_t App_NewPeriph_ReadIr(void) { return ir_blocked(g_ms); }
void App_NewPeriph_BeepPulse(uint32_t ms)
{ if (g_nBeeps < 2048) { g_beeps[g_nBeeps].ms = g_ms; g_beeps[g_nBeeps].durMs = (uint16_t)ms; g_nBeeps++; } }
uint8_t App_NewPeriph_BeepPulseActive(void) { return 0; }
void App_NewPeriph_Beep(uint8_t on) { (void)on; }

/* ================= AM 桩 ================= */
int App_AM_SetParam(uint8_t cmd, uint16_t value)
{
    if (cmd != AM_CMD_MODE) return 0;
    if (g_script.amModeFail) return APP_AM_ERR_LINK;
    g_amMode = value;
    if (g_nAmModes < 32) { g_amModes[g_nAmModes].ms = g_ms; g_amModes[g_nAmModes].mode = value;
    g_amModes[g_nAmModes].ok = 1; g_nAmModes++; }
    return APP_AM_ERR_OK;
}
int  App_AM_GetParam(uint8_t cmd, uint16_t *v) { (void)cmd; *v = g_amMode; return 0; }
int  App_AM_Query(void) { return 0; }
int  App_AM_GetConfig(AppAMConfig_t *c) { c->mode = g_amMode; return 0; }
int  App_AM_SetConfig(const AppAMConfig_t *c, int save) { (void)c; (void)save; return 0; }
uint32_t App_AM_GetEventCount(void) { return g_amEventCount; }
uint32_t App_AM_GetDeactCount(void) { return g_amEventCount; }
uint32_t App_AM_GetFailCount(void) { return 0; }
uint8_t  App_AM_BurstActive(void) { return 0; }
AppAMDeactState_t App_AM_GetDeactState(void) { return AM_DEACT_IDLE; }
uint32_t App_AM_GetLastEventMs(void) { return 0; }
int  App_AM_GetLinkStatus(void) { return 0; }

/* ================= 协议/灯语桩 ================= */
void Proto_TxResponse(uint8_t ch, uint8_t fc, const uint8_t *data, uint16_t len)
{
    (void)ch; (void)fc;
    if (g_nFrames < 256) {
        g_frames[g_nFrames].ms = g_ms;
        g_frames[g_nFrames].len = (uint8_t)len;
        for (uint16_t i = 0; i < len && i < 40; i++) g_frames[g_nFrames].d[i] = data[i];
        g_nFrames++;
    }
}
void Memset8(void *dst, uint8_t val, uint32_t n)
{ uint8_t *p = (uint8_t*)dst; while (n--) *p++ = val; }
void Memcpy(void *dst, const void *src, uint32_t n)
{ uint8_t *d = (uint8_t*)dst; const uint8_t *s = (const uint8_t*)src; while (n--) *d++ = *s++; }
void App_RgbLedPat_Set(uint8_t src, uint8_t pat)
{ (void)src; if (g_nLeds < 16384) { g_leds[g_nLeds].ms = g_ms; g_leds[g_nLeds].ev = 1; g_leds[g_nLeds].val = pat; g_nLeds++; } }
void App_RgbLedPat_Clear(uint8_t src)
{ (void)src; if (g_nLeds < 16384) { g_leds[g_nLeds].ms = g_ms; g_leds[g_nLeds].ev = 3; g_leds[g_nLeds].val = 0; g_nLeds++; } }
void App_RgbLedPat_Flash(uint8_t id)
{ if (g_nLeds < 16384) { g_leds[g_nLeds].ms = g_ms; g_leds[g_nLeds].ev = 2; g_leds[g_nLeds].val = id; g_nLeds++; } }
