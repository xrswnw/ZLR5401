#include "App_MotorHoming.h"
#include "App_NewPeriph_HL.h"
#include "App_Stepper.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"

/* =====================================================================
 * 上电行程自检 / 回零 (无条件完整自检: 先上后下).
 *
 * 背景: 上电时电机位置未知 (无绝对位置反馈), 行程开关是唯一绝对位置参考.
 *      开关若开路消失, 固件无法感知 (上拉输入, 未接与坏断都读高=从不触发),
 *      位置类软限位会因"起点未知+丢步"而失真. 故首次运动前, 无论停在何处:
 *        阶段1: 向上找 KEY_UP  -> 验证上行程开关在位/可达;
 *        阶段2: 向下回 KEY_DOWN -> 验证下行程开关并建立下行程绝对基准.
 *        任一阶段失败(堵转撞硬挡被 TRQ 停机 / 超步 / 超时) -> 置对应
 *        行程开关错误位, 由上层禁止 MOVE/TEST 并灯色报警 (保护设备).
 *
 * 探测驱动尽量保护设备: 低转速 + 低转矩; 朝目标方向驱动并设最大步数上限,
 *   超限/超时/堵转(TRQ 监测停机)即判对应开关失效, 立即断电停止.
 * 阻塞等待期间手动推进 App_Stepper_Process (主循环不跑) 并喂 IWDG.
 * ===================================================================== */

#define HOMING_SPEED_HZ     2000u      /* 回零转速 (较慢, 防冲击) */
#define HOMING_TORQUE_PCT   40u        /* 回零转矩 (较低, 硬顶冲击小) */
#define HOMING_DIR_DOWN     1u         /* dir=1 = 反转向下 (奔 KEY_DOWN) */
#define HOMING_DIR_UP       0u         /* dir=0 = 正转向上 (奔 KEY_UP) */
#define HOMING_DOWN_MAX     (4324u * 2u + 800u)   /* 下腿实测×2 + 裕量, 防无限冲底 */
#define HOMING_UP_MAX       (4085u * 2u + 800u)   /* 上腿实测×2 + 裕量 */
#define HOMING_TIMEOUT_MS   30000u
/* 步数停滞检测: 若连续 HOMING_STALL_MS 内步数未增加(丢步), 判该端不可达立即停.
 * 注: DRV8434S 堵转时控制器仍持续发 STEP 脉冲, 步数照涨, 故顶墙堵转不由本检测处理,
 *     而交由 App_Stepper_Process 的 TRQ 监测(持续低负载 -> 降速 -> 停机)兜底.
 *     本停滞检测仅针对"完全丢步/电机不转"的极端情况. */
#define HOMING_STALL_MS     400u

static volatile uint8_t s_homingReady = 0u;   /* 1=已回零/建立基准, 可安全 MOVE/TEST */

int App_MotorHoming_IsReady(void) { return (int)s_homingReady; }

/* 为"朝某方向驱动寻找开关"的最小封装: 返回 0=已触到(停在该端)  1=未触到(超限) */
static int seek_switch(uint8_t dir, uint32_t maxSteps)
{
    AppStepperMove_t mv;
    mv.dir = dir;
    mv.steps = 0u;                     /* 持续, 由触点/超限停 */
    if (App_Stepper_Move(&mv) != 0) return 1;
    uint32_t t0      = SysTickHl_GetMs();
    uint32_t lastSeq = App_Stepper_GetStepsDone();
    uint32_t stallMs = t0;
    while (1) {
        /* 回零是阻塞循环, 主循环不跑; 必须在此手动推进步进状态机,
         * 才能让 TRQ 堵转监测的"降速/停机"分支在回零期间真正生效.
         * 堵转保护完全交由 App_Stepper_Process 的既有逻辑(持续低TRQ -> 降速 -> 停机),
         * 其带启动缓冲(>600ms 才开始判定), 不会误停刚启动的正常电机. */
        App_Stepper_Process();
        IwdgHl_Feed();
        int hit = (dir == HOMING_DIR_DOWN) ? (App_NewPeriph_ReadKeyDown() == 0u)
                                           : (App_NewPeriph_ReadKeyUp() == 0u);
        if (hit) {                     /* 到达目标行程开关 */
            App_Stepper_DcBrakeStop();
            (void)App_Stepper_ClearFault();
            return 0;
        }
        uint32_t nowSt = SysTickHl_GetMs();
        uint32_t nowSteps = App_Stepper_GetStepsDone();
        /* 步数在前进 -> 更新停滞基准; 否则(丢步)累计停滞时间 */
        if (nowSteps != lastSeq) {
            lastSeq = nowSteps;
            stallMs = nowSt;
        }
        /* 停滞超过阈值且未触及开关 -> 判该端不可达, 立即断电停止 (丢步兜底) */
        if ((nowSt - stallMs) >= HOMING_STALL_MS) {
            App_Stepper_Stop();
            return 1;
        }
        /* 超限: 走了整程×2 仍未触发 -> 该端开关缺失/失效 */
        if (nowSteps >= maxSteps) {
            App_Stepper_Stop();
            return 1;
        }
        /* 电机陷入 FOIDLE (堵转保护/超限已由 Process 停机) -> 该端不可达 */
        if (App_Stepper_GetState() == APP_STEPPER_IDLE) {
            (void)App_Stepper_Stop();
            return 1;
        }
        if ((nowSt - t0) >= HOMING_TIMEOUT_MS) {
            App_Stepper_Stop();
            return 1;
        }
        SysTickHl_DelayMs(20u);
    }
}

/* 起步瞬态重试: 实测上电后首次驱动电机会被 DRV8434S 以 nFAULT+SPI_ERROR 误停
 * (间歇性, 与电机位置/方向/负载无关; 稍后同样动作完全正常, 疑上电枚举/电源瞬态).
 * 判据: 走了 <50 步且 DRV 故障寄存器非零 —— 真实堵转(撞硬挡)会持续走步直到
 * TRQ/腿长监控停机, 超步/超时也必然步数或时间先耗尽, 均不会长这样. 满足判据则
 * 清故障重试一次; 重试仍失败按真实失败处理. */
#define HOMING_TRANSIENT_STEPS 50u

static int seek_phase(uint8_t dir, uint32_t maxSteps)
{
    if (seek_switch(dir, maxSteps) == 0) return 0;
    if (App_Stepper_GetStepsDone() < HOMING_TRANSIENT_STEPS &&
        App_Stepper_GetFault() != 0u) {
        (void)App_Stepper_ClearFault();       /* 同时清被瞬态污染的 switchErr */
        if (seek_switch(dir, maxSteps) == 0) return 0;
    }
    return 1;
}

MotorHomingResult_t App_MotorHoming_Run(void)
{
    s_homingReady = 0u;   /* 先假定未就绪 */
    if (App_Stepper_GetState() == APP_STEPPER_FAULT) {
        App_Stepper_SetSwitchErr(0x03u);
        return MOTOR_HOMING_FAULT;
    }

    (void)App_Stepper_SetSpeedHz(HOMING_SPEED_HZ);
    (void)App_Stepper_SetTorquePercent(HOMING_TORQUE_PCT);

    /* ===== 无条件完整自检 (先上后下), 无论电机当前停在哪个位置 =====
     * 阶段1 向上找 KEY_UP: 验证上行程开关在位/可达.
     *   静态读已触发 -> 上开关确认在位, 无需驱动;
     *   否则驱动向上寻找; 堵转(撞硬挡被 TRQ 停机)/超步/超时 -> 上开关缺失, 报警.
     * 阶段2 向下回 KEY_DOWN: 建立下行程绝对基准 (自检通过的标志位).
     *   失败(堵转/超步/超时) -> 下开关缺失, 报警.
     * 任一阶段失败: 置对应错误位 (switchErr + 灯色), MOVE/TEST 被禁. */
    if (App_NewPeriph_ReadKeyUp() != 0u) {     /* 上开关未触发 -> 驱动向上验证 */
        if (seek_phase(HOMING_DIR_UP, HOMING_UP_MAX) != 0) {
            App_Stepper_SetSwitchErr(0x01u);   /* bit0 = 上行程缺失 */
            return MOTOR_HOMING_TIMEOUT;
        }
    }

    /* 阶段2: 向下回归 KEY_DOWN 建立基准 (上阶段结束时机构在上端) */
    if (seek_phase(HOMING_DIR_DOWN, HOMING_DOWN_MAX) != 0) {
        App_Stepper_SetSwitchErr(0x02u);       /* bit1 = 下行程缺失 */
        return MOTOR_HOMING_TIMEOUT;
    }

    s_homingReady = 1u;                        /* 上下均验证: 已回零到下行程 */
    return MOTOR_HOMING_OK;
}
