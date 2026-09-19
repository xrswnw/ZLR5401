#include "App_LockerSeek.h"
#include "App_RgbLed_Pattern.h"
#include "App_UHF.h"
#include "App_Stepper.h"
#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_CustomProtocol.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"
#include "App_NewPeriph_HL.h"
#include <stddef.h>

/* 从原单标签流程提取 (Round_012): 泵循环与寻触驱动供
 * Unlock 0x0A 使用, 判据与工况参数不变。 */

static volatile uint8_t *s_abortFlag;    /* 调用方 CANCEL 请求 (NULL=不参与) */
static volatile uint8_t *s_immuneFlag;   /* 调用方回退免疫标志 (NULL=不参与) */
static int (*s_monitor)(void);            /* 泵拍监守回调 (NULL=无): 返回非0 → 主动停机 */

void LockerSeek_BindAbort(volatile uint8_t *abortFlag, volatile uint8_t *immuneFlag)
{
    s_abortFlag  = abortFlag;
    s_immuneFlag = immuneFlag;
}

/* 设置寻触段泵拍监守回调 (每拍调用一次, 回调内可做阻塞时长 ≤ 单发盘点
 * 超时的采样)。返回非0 → 主动停机, Run 返回 3 (不重试)。
 * Round_013: EPC 解锁 0x10 升起期间 0x21 连续盘点监守 (标签移除/更换
 * 即收起)。传 NULL 撤销。 */
void LockerSeek_SetMonitor(int (*fn)(void))
{
    s_monitor = fn;
}

/* 泵循环: 阻塞在主循环上下文时手动推进各状态机 + 喂狗 + 节拍。
 * TIM4(步进)/SysTick(ms)/USART(UHF+AM收帧)/USB 均为中断驱动, 不受阻塞影响。
 * 内嵌 Proto_Poll: 阻塞期间协议层继续收发, 分发层按互斥白名单放行
 * 进度查询/CANCEL/只读查询, 其余回 BUSY (防嵌套重入)。 */
void LockerSeek_Pump(void)
{
    App_RgbLedPat_Tick();   /* 阻塞期间推进 RGB 灯语 (否则图样冻结) */
    App_UHF_Process();
    App_Stepper_Process();
    App_AM_Process();
    Proto_Poll();
    IwdgHl_Feed();
    (void)App_NewPeriph_BeepPulseActive();  /* 蜂鸣脉冲到期静音 (主循环外必调, 否则长鸣) */
    SysTickHl_DelayMs(20u);
}

/* 朝 dir 方向持续驱动寻触点 (KEY_UP/KEY_DOWN)。触到即 DC 磁制动停位并清障。 */
int LockerSeek_Run(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut)
{
    AppStepperMove_t mv;
    mv.dir = dir;
    mv.steps = 0u;                       /* 持续运行, 由触点/兜底停 */
    if (App_Stepper_Move(&mv) != 0) return 1;

    uint32_t t0      = SysTickHl_GetMs();
    uint32_t lastSeq = App_Stepper_GetStepsDone();
    uint32_t stallMs = t0;
    uint8_t  aborted = 0u;
    uint8_t  monitored = 0u;
    while (1) {
        LockerSeek_Pump();
        /* CANCEL 打断: 立即停机 (免疫段忽略) */
        if (s_abortFlag && *s_abortFlag &&
            !(s_immuneFlag && *s_immuneFlag)) {
            (void)App_Stepper_Stop(); aborted = 1u; break;
        }
        /* 监守回调请求停机 (标签移除/更换等, 不重试) */
        if (s_monitor && s_monitor() != 0) {
            (void)App_Stepper_Stop(); monitored = 1u; break;
        }
        int hit = (dir == LSEEK_DIR_DOWN) ? (App_NewPeriph_ReadKeyDown() == 0u)
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
        if ((now - stallMs) >= LSEEK_STALL_MS)  { (void)App_Stepper_Stop(); break; }
        if (st >= maxSteps)                     { (void)App_Stepper_Stop(); break; }
        if (App_Stepper_GetState() == APP_STEPPER_IDLE) { (void)App_Stepper_Stop(); break; }
        if ((now - t0) >= LSEEK_TIMEOUT_MS)     { (void)App_Stepper_Stop(); break; }
    }
    if (stepsOut) *stepsOut = App_Stepper_GetStepsDone();
    return aborted ? 2 : (monitored ? 3 : 1);
}

/* 寻触 + 失败重试一次 (覆盖: 上电首驱 nFAULT 瞬态误停 [判据同回零] 与
 * 启动全失步 [实测脉冲走满整腿而机构未动, 紧随其后的同参数驱动即正常])。 */
int LockerSeek_RunRetry(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut)
{
    int r = LockerSeek_Run(dir, maxSteps, stepsOut);
    if (r != 1) return r;
    (void)App_Stepper_ClearFault();
    r = LockerSeek_Run(dir, maxSteps, stepsOut);
    return (r == 1) ? 1 : r;
}
