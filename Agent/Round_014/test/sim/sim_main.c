/* sim_main.c — Round_014 0x10/0x11 状态机宿主机模拟测试。
 * 剧本驱动虚拟外设 (sim_stubs.c), 对 App_LockerUnlock.c 真实逻辑
 * 做黑盒断言: 终帧字段 / 推送帧序列 / 电机腿轨迹 / 灯语蜂鸣 / 进度采样。
 * 约定: 先 reset_scenario() 再布剧本 (tag/demag/hook), 最后 EpcRun/AmRun。 */
#include "sim_env.h"
#include "App_LockerUnlock.h"
#include "App_LockerSeek.h"
#include "App_RgbLed_Pattern.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* 名单 EPC: A=E2A1, B=E2B2, C=E2C3; 外来 D=E2D4 */
static void add_tag(uint32_t t0, uint32_t t1, uint8_t v)
{ SimTagSpan *s = &g_script.tags[g_script.nTags++];
  s->t0 = t0; s->t1 = t1; s->epc[0] = 0xE2; s->epc[1] = v; s->len = 2; }
static void add_demag(uint32_t t) { g_script.demagMs[g_script.nDemag++] = t; }

static void reset_scenario(void)
{
    memset(&g_script, 0, sizeof(g_script));
    g_nFrames = 0; g_nBeeps = 0; g_nLeds = 0; g_nLegs = 0;
    g_nAmModes = 0; g_nSamples = 0;
    g_amEventCount = 0; g_amMode = 1u;       /* 常态仅检测 */
    g_motorPos = 0; g_speedHz = 2000u;
    g_ms = 1000u;                            /* 非零起点 (受理时刻基准) */
    sim_reset_hooks();
    App_LockerUnlock_Finish();
}

static int pack1(uint8_t *pk, int n, ...)
{
    int pos = 0;
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) {
        pk[pos++] = 2; pk[pos++] = 0xE2; pk[pos++] = (uint8_t)va_arg(ap, int);
    }
    va_end(ap);
    return pos;
}

static int count_frames(uint8_t sub)
{ int c = 0; for (int i = 0; i < g_nFrames; i++) if (g_frames[i].d[0] == sub) c++; return c; }
static int count_legs(int dir, int result)
{ int c = 0; for (int i = 0; i < g_nLegs; i++)
    if (g_legs[i].dir == dir && g_legs[i].result == result) c++; return c; }
static int led_count(uint8_t ev, uint8_t val)
{ int c = 0; for (int i = 0; i < g_nLeds; i++)
    if (g_leds[i].ev == ev && g_leds[i].val == val) c++; return c; }
static int beep_count(uint16_t dur)
{ int c = 0; for (int i = 0; i < g_nBeeps; i++) if (g_beeps[i].durMs == dur) c++; return c; }
static int find_sample(uint32_t aroundMs)
{ int best = 0; long bd = 1L << 30;
  for (int i = 0; i < g_nSamples; i++) {
      long d = (long)g_samples[i].ms - (long)aroundMs;
      if (d < 0) d = -d;
      if (d < bd) { bd = d; best = i; } }
  return best; }
static void cancel_hook(void) { App_LockerUnlock_Abort(); }

/* ================= E1: 单标稳定 -> 升 -> 在顶至超窗 -> ALL_OK ================= */
static void e1(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== E1 单标全程 ==\n");
    add_tag(0, 999999, 0xA1);
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 10, 2u, &res);
    App_LockerUnlock_Finish();

    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, EPC_END_ALL_OK, "endReason ALL_OK");
    CHECK_EQ(res.riseCnt, 1, "riseCnt=1");
    CHECK_EQ(res.alarmCnt, 0, "alarmCnt=0");
    CHECK_EQ(res.fgnSeen, 0, "fgnSeen=0");
    CHECK(res.riseSteps > 8000, "riseSteps 至顶部");
    CHECK(res.lowerSteps > 8000, "lowerSteps 回底");
    CHECK_EQ(count_frames(0x0F), 1, "0x0F 受理帧 1");
    CHECK_EQ(count_frames(0x0B), 1, "0x0B 确认帧 1 (A)");
    CHECK_EQ(count_legs(LSEEK_DIR_UP, 0), 1, "升至触点 1 次");
    CHECK_EQ(count_legs(LSEEK_DIR_DOWN, 0), 1, "回降至触点 1 次");
    CHECK_EQ(led_count(2, RGBFLASH_DONE_3GREEN), 1, "结束绿三闪");
    CHECK_EQ(beep_count(1000), 1, "结束蜂鸣 1s");
    /* M1 时序: 升腿 8170 步 @2000Hz ≈ 4.1s < 10s 超时 */
    CHECK(g_legs[0].t1 - g_legs[0].t0 < 4600, "升腿 2000Hz ~4.1s");
    printf("   rise leg %lums, elapsed %lds\n",
           (long)(g_legs[0].t1 - g_legs[0].t0), (long)(res.elapsedMs / 1000));
}

/* ============ E2: AB 组合有效升; D 介入报警快降; D 失 A/B 在 -> 再升 ============ */
static void e2(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 2, 0xB2, 0xA1);
    reset_scenario();
    printf("== E2 外来介入回降再升 ==\n");
    add_tag(0, 999999, 0xB2);
    add_tag(0, 999999, 0xA1);
    add_tag(8000, 10000, 0xD4);                   /* 外来 D 8~10s (在顶期) */
    App_LockerUnlock_EpcRun(pk, 2, (uint16_t)plen, 20, 2u, &res);
    App_LockerUnlock_Finish();

    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, EPC_END_ALL_OK, "endReason ALL_OK (超窗在顶)");
    CHECK_EQ(res.riseCnt, 2, "riseCnt=2 (再升)");
    CHECK_EQ(res.alarmCnt, 1, "alarmCnt=1");
    CHECK_EQ(res.fgnSeen, 1, "fgnSeen=1");
    CHECK_EQ(count_frames(0x0C), 1, "0x0C 外来介入帧 1");
    CHECK_EQ(count_frames(0x0B), 2, "0x0B A/B 各一帧");
    CHECK_EQ(count_legs(LSEEK_DIR_UP, 0), 2, "升至触点 2 次");
    CHECK(count_legs(LSEEK_DIR_DOWN, 0) >= 1, "快降至少 1 次");
    CHECK(led_count(1, RGBPAT_ALARM_BLINK) >= 1, "报警红闪图样出现过");
    CHECK(beep_count(50) >= 1, "报警蜂鸣 50ms");
}


/* ============ E2b: 升起途中 D 介入 -> 监守即停 + 快降; 再升 ============ */
static void e2b(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== E2b 升起中介入即停 ==\n");
    add_tag(0, 999999, 0xA1);
    add_tag(6000, 8000, 0xD4);                    /* D 在升起途中进入 */
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 20, 2u, &res);
    App_LockerUnlock_Finish();

    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, EPC_END_ALL_OK, "endReason ALL_OK");
    CHECK_EQ(count_legs(LSEEK_DIR_UP, 3), 1, "升起途中被监守中止 1 次");
    CHECK_EQ(res.alarmCnt, 1, "alarmCnt=1");
    CHECK_EQ(res.fgnSeen, 1, "fgnSeen=1");
    CHECK(res.riseCnt >= 1, "中止后再升到顶");
    CHECK(count_legs(LSEEK_DIR_DOWN, 0) >= 1, "快降");
}

/* ============ E3: 在顶名单全失 -> 报警快降; A 回场稳定 -> 再升 ============ */
static void e3(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== E3 全失回降再升 ==\n");
    add_tag(0, 8000, 0xA1);
    add_tag(12000, 999999, 0xA1);                  /* 回场 */
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 25, 2u, &res);
    App_LockerUnlock_Finish();

    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, EPC_END_ALL_OK, "endReason ALL_OK");
    CHECK_EQ(res.riseCnt, 2, "riseCnt=2");
    CHECK_EQ(res.alarmCnt, 1, "alarmCnt=1 (全失报警)");
    CHECK_EQ(res.fgnSeen, 0, "fgnSeen=0 (无外来)");
    CHECK_EQ(count_frames(0x0C), 0, "无 0x0C");
}

/* ============ E4: WAIT 期外来介入 (不升只报警); D 走后升起 ============ */
static void e4(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== E4 等待期介入不升 ==\n");
    add_tag(0, 999999, 0xA1);
    add_tag(0, 4000, 0xD4);                        /* D 先在 */
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 15, 2u, &res);
    App_LockerUnlock_Finish();

    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.riseCnt, 1, "riseCnt=1 (D 走后才升)");
    CHECK_EQ(res.alarmCnt, 1, "alarmCnt=1");
    CHECK(g_legs[0].t0 > 5000, "首升在 D 稳定消失后 (~4s+1.5s)");
}

/* ============ E5: 升起中 CANCEL -> ABORTED 回降 ============ */
static void e5(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== E5 升起中 CANCEL ==\n");
    add_tag(0, 999999, 0xA1);
    sim_at(3000, cancel_hook);                     /* 升起始于 ~2.6s */
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 15, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, EPC_END_ABORTED, "endReason ABORTED");
    CHECK(count_legs(LSEEK_DIR_DOWN, 0) >= 1, "CANCEL 后安全回降");
    CHECK_EQ(g_motorPos, 0, "电机复位到 0");
}

/* ============ E6: UHF 失联 5s -> UHF_LOST ============ */
static void e6(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== E6 UHF 失联 ==\n");
    add_tag(0, 999999, 0xA1);
    g_script.link[0] = (SimSpan){ 2000, 999999 }; g_script.nLink = 1;
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 20, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, EPC_END_UHF_LOST, "endReason UHF_LOST");
    CHECK_EQ(res.riseCnt, 0, "未升起");
    CHECK(led_count(2, RGBFLASH_FAIL_DOUBLE) >= 1, "红双闪");
}

/* ============ E7: 参数校验 (纯函数, 无外设动作) ============ */
static void e7(void)
{
    LockerUnlockResult_t res;
    uint8_t dup[6] = { 2, 0xE2, 0xA1, 2, 0xE2, 0xA1 };
    uint8_t badLen[3] = { 0, 0xE2, 0xA1 };
    uint8_t shortData[2] = { 2, 0xE2 };
    printf("== E7 参数校验 ==\n");
    reset_scenario();
    App_LockerUnlock_EpcRun(dup, 2, 6, 10, 2u, &res);
    CHECK_EQ(res.err, UNLK_ERR_PARAM, "重复 EPC -> PARAM");
    reset_scenario();
    App_LockerUnlock_EpcRun(badLen, 1, 3, 10, 2u, &res);
    CHECK_EQ(res.err, UNLK_ERR_PARAM, "len=0 -> PARAM");
    reset_scenario();
    App_LockerUnlock_EpcRun(shortData, 1, 2, 10, 2u, &res);
    CHECK_EQ(res.err, UNLK_ERR_PARAM, "数据不足 -> PARAM");
    reset_scenario();
    App_LockerUnlock_EpcRun(dup, 2, 6, 0, 2u, &res);
    CHECK_EQ(res.err, UNLK_ERR_PARAM, "tmo=0 -> PARAM");
    reset_scenario();
    App_LockerUnlock_EpcRun(dup, 0, 6, 10, 2u, &res);
    CHECK_EQ(res.err, UNLK_ERR_PARAM, "epcCnt=0 -> PARAM");
    CHECK_EQ(g_nLegs, 0, "全程无电机动作");
}

/* ============ A1: AM 3 帧逐条入账 -> 达标挂起 ALL_OK ============ */
static void a1(void)
{
    LockerUnlockResult_t res;
    reset_scenario();
    printf("== A1 AM 逐帧入账 ==\n");
    add_demag(1000); add_demag(2500); add_demag(5000);
    App_LockerUnlock_AmRun(3, 60, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, AM_END_ALL_OK, "ALL_OK");
    CHECK_EQ(res.softDone, 3, "softDone=3");
    CHECK_EQ(count_frames(0x0E), 3, "0x0E 每帧一推");
    CHECK_EQ(count_frames(0x0F), 1, "0x0F 受理帧");
    CHECK(g_amModes[0].mode == 0 && g_amModes[1].mode == 1, "消磁开->挂起");
    CHECK_EQ(res.amFail, 0, "amFail=0");
    CHECK_EQ(res.riseSteps, 0, "riseSteps=0 (不动电机)");
    CHECK_EQ(beep_count(1000), 1, "结束蜂鸣 1s");
    CHECK_EQ(g_nLegs, 0, "全程无电机动作");
}

/* ============ A2: AM 超窗 TIMEOUT (部分完成) ============ */
static void a2(void)
{
    LockerUnlockResult_t res;
    reset_scenario();
    printf("== A2 AM 超窗 ==\n");
    add_demag(1000); add_demag(2000);
    App_LockerUnlock_AmRun(3, 5, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, AM_END_TIMEOUT, "TIMEOUT");
    CHECK_EQ(res.softDone, 2, "softDone=2 (部分完成)");
    CHECK(g_amModes[0].mode == 0 && g_amModes[1].mode == 1, "超窗也挂起");
}

/* ============ A3: AM CANCEL -> 尽力挂起 + ABORTED ============ */
static void a3(void)
{
    LockerUnlockResult_t res;
    reset_scenario();
    printf("== A3 AM CANCEL ==\n");
    add_demag(1000);
    sim_at(3000, cancel_hook);
    App_LockerUnlock_AmRun(3, 60, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, AM_END_ABORTED, "ABORTED");
    CHECK(g_amModes[0].mode == 0 && g_amModes[1].mode == 1, "CANCEL 即挂起");
}

/* ============ A4: SET_MODE 确认失败 -> AM_LINK ============ */
static void a4(void)
{
    LockerUnlockResult_t res;
    reset_scenario();
    printf("== A4 AM 链路失败 ==\n");
    g_script.amModeFail = 1;
    App_LockerUnlock_AmRun(3, 60, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, UNLK_ERR_AM_LINK, "AM_LINK");
    CHECK_EQ(count_frames(0x0F), 0, "未受理 (控制失败先回)");
    CHECK_EQ(g_nLegs, 0, "无电机动作");
}

/* ============ A5: amCnt=0 直通 ============ */
static void a5(void)
{
    LockerUnlockResult_t res;
    reset_scenario();
    printf("== A5 amCnt=0 直通 ==\n");
    App_LockerUnlock_AmRun(0, 10, 2u, &res);
    App_LockerUnlock_Finish();
    CHECK_EQ(res.err, 0, "err");
    CHECK_EQ(res.endReason, AM_END_ALL_OK, "ALL_OK 直通");
    CHECK_EQ(g_nAmModes, 0, "不碰 AM");
}

/* ============ P1: 0x10 进度采样 (flags/mph 实时性) ============ */
static void p1(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 2, 0xA1, 0xB2);
    reset_scenario();
    printf("== P1 进度采样 ==\n");
    add_tag(0, 999999, 0xA1);
    add_tag(0, 999999, 0xB2);
    add_tag(9000, 11000, 0xD4);
    sim_progress_at(1500);    /* WAIT: 未稳定 */
    sim_progress_at(3000);    /* 已稳定或刚升 */
    sim_progress_at(7500);    /* 在顶 */
    sim_progress_at(9600);    /* D 介入报警中 */
    sim_progress_at(13500);   /* D 走后 (回升/在顶) */
    App_LockerUnlock_EpcRun(pk, 2, (uint16_t)plen, 20, 2u, &res);
    App_LockerUnlock_Finish();

    int i;
    i = find_sample(1500);
    CHECK(g_samples[i].phase == UNLK_PH_WAIT_TAG, "t=1.5s WAIT");
    CHECK(!g_samples[i].valid, "t=1.5s 未稳定");
    i = find_sample(3000);
    CHECK(g_samples[i].valid, "t=3s 有效子集稳定");
    i = find_sample(7500);
    CHECK(g_samples[i].mph == UNLK_MPH_UP, "t=7.5s 在顶保持");
    i = find_sample(9600);
    CHECK(g_samples[i].fgn && g_samples[i].alarm, "t=9.6s 外来介入+报警");
    CHECK(g_samples[i].mph != UNLK_MPH_UP, "t=9.6s 已离顶");
    i = find_sample(13500);
    CHECK(g_samples[i].valid && !g_samples[i].fgn, "t=13.5s D 已稳定消失");
}

/* ============ M1: 2000Hz 时序预算 (全腿逐条) ============ */
static void m1(void)
{
    LockerUnlockResult_t res; uint8_t pk[64];
    int plen = pack1(pk, 1, 0xA1);
    reset_scenario();
    printf("== M1 时序预算 ==\n");
    add_tag(0, 999999, 0xA1);
    App_LockerUnlock_EpcRun(pk, 1, (uint16_t)plen, 6, 2u, &res);
    App_LockerUnlock_Finish();
    for (int i = 0; i < g_nLegs; i++)
        CHECK(g_legs[i].t1 - g_legs[i].t0 < 10000, "每腿 <10s (LSEEK_TIMEOUT)");
    CHECK(g_legs[0].t1 - g_legs[0].t0 < 4600, "升腿 ~4.1s (8170步@2000Hz)");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    e1(); e2(); e2b(); e3(); e4(); e5(); e6(); e7();
    a1(); a2(); a3(); a4(); a5();
    p1(); m1();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
