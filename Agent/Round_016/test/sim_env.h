#ifndef SIM_ENV_H
#define SIM_ENV_H
#include <stdint.h>

/* ---- 虚拟时钟 ---- */
extern uint32_t g_ms;

/* ---- 剧本: 标签在场时间线 / IR 遮挡窗 / UHF 失联窗 / AM 消磁帧 ---- */
typedef struct { uint32_t t0, t1; uint8_t epc[12]; uint8_t len; } SimTagSpan;
typedef struct { uint32_t t0, t1; } SimSpan;

typedef struct {
    SimTagSpan tags[16];  uint8_t nTags;
    SimSpan    ir[4];     uint8_t nIr;      /* IR 遮挡窗; 空 = 跟随"有标签即遮挡" */
    SimSpan    link[4];   uint8_t nLink;    /* UHF 链路坏窗 */
    uint32_t   demagMs[32]; uint8_t nDemag; /* cmd17 消磁帧时刻 */
    uint8_t    amModeFail;                  /* 1=SET_MODE 全部失败 (A4) */
    uint8_t    uhfOpenFail;                 /* 1=UHF Open 失败 */
} SimScript;
extern SimScript g_script;

/* ---- 观测日志 (每场景 reset) ---- */
typedef struct { uint32_t ms; uint8_t len; uint8_t d[40]; } SimFrame;
extern SimFrame g_frames[256]; extern int g_nFrames;
typedef struct { uint32_t ms; uint16_t durMs; } SimBeep;
extern SimBeep  g_beeps[2048];  extern int g_nBeeps;
typedef struct { uint32_t ms; uint8_t ev; uint8_t val; } SimLed;  /* 1=Set 2=Flash 3=Clear */
extern SimLed   g_leds[16384];   extern int g_nLeds;
typedef struct { uint32_t t0, t1; uint8_t dir; int result; uint32_t steps; } SimLeg;
extern SimLeg   g_legs[64];    extern int g_nLegs;
typedef struct { uint32_t ms; uint16_t mode; int ok; } SimAmMode;
extern SimAmMode g_amModes[32]; extern int g_nAmModes;

extern uint32_t g_amEventCount;   /* 已"收到"的 cmd17 帧数 (时间驱动) */
extern uint16_t g_amMode;
extern uint32_t g_motorPos;      /* 电机绝对位置 (0=底, 8170=顶) */
extern uint32_t g_speedHz;

/* ---- 时间钩子 (剧本注入 CANCEL / 进度采样) ---- */
typedef void (*SimHookFn)(void);
void sim_at(uint32_t ms, SimHookFn fn);
void sim_reset_hooks(void);

/* ---- 进度采样: sim_progress_at(t) 预约, 事后 sim_get_sample(i) 取 ---- */
void sim_progress_at(uint32_t ms);
int  sim_n_samples(void);
uint32_t sim_sample_ms(int i);
/* 采样体经 sim_progress_at + 主循环读 — 用回调写全局数组 (Round_016 v3 布局): */
typedef struct { uint32_t ms; uint8_t status; uint8_t tag; uint8_t elapsed;
                 uint8_t riseCnt; uint8_t alarmCnt; uint8_t lastCycle; } SimSample;
extern SimSample g_samples[32]; extern int g_nSamples;

/* ---- 断言 ---- */
extern int g_checks, g_fails;
void CHECK(int cond, const char *msg);
void CHECK_EQ(long got, long want, const char *msg);

#endif
