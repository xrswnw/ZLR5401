#ifndef __APP_LOCKERONESHOT_H
#define __APP_LOCKERONESHOT_H

#include <stdint.h>

/* =====================================================================
 * 单条同步开锁 (LOCKER_SUB_ONE_SHOT 0x08) — 用户视角一帧全流程。
 *
 * 请求帧带: UHF 盘点超时 + 最大保持窗 + 期望 EPC。固件同步阻塞执行:
 *   ⑴ 前置检查 (Locker IDLE / 电机 IDLE / 已回零)
 *   ⑴.5 光电门控: 等客户放置标签 (PC4 高=检测到, 200ms 去抖),
 *       等待窗 = maxHoldMs, 窗满未触发 -> NO_IR 失败
 *   ⑴.6 消磁准备 (demagCnt>0: AM 探链+强制检测模式 — 校验期不消磁;
 *       demagCnt=0 跳过, 不触碰 AM)
 *   ⑵ UHF 就绪 (上电+配置)
 *   ⑶ 持续校对: 反复盘点比对期望 EPC (单轮 ~1/4 漏读属正常, 未命中
 *       继续轮); 读到标签但连续 3 轮均无期望 -> MISMATCH (红闪不升起);
 *       校对预算 (maxHoldMs) 内无任何标签 -> NO_TAG
 *   ⑷ 命中 → (demagCnt>0 先切 AM 消磁模式) 电机上行至 KEY_UP 上行程
 *       触点, 停住保持; 流程结束 (含失败/打断) 切回 AM 检测模式
 *   ⑸ 保持期监控 (先到先回降):
 *       消磁事件数达标(demagCnt>0) / EPC 稳定确认(demagCnt=0: 连续
 *       在场 3s -> 结账成功, 不等取走/窗满) / 保持窗超时 /
 *       期望标签稳定移除 / 期望标签消失后读到其他 EPC / UHF 链路失联
 *   ⑹ 下行回退至 KEY_DOWN 下行程触点 (绝对基准), 停止
 *   ⑺ 回响应帧 (详细失败码 + 结束原因 + 诊断字段)
 *
 * 阻塞可行性: 分发回调在主循环上下文; UHF 收帧走 USART 中断,
 * 步进脉冲走 TIM4 中断, 阻塞泵循环手动推进 App_UHF_Process /
 * App_Stepper_Process 并喂狗 (同 App_MotorHoming 既有模式)。
 *
 * 升/降段失败必先尝试回退 KEY_DOWN 再返回, 结果记入 retreat。
 * ===================================================================== */

/* ---- 结束原因 (err=0 时回降触发者) ---- */
#define ONE_END_TAG_REMOVED   1u   /* 期望标签稳定移除 (正常业务完成) */
#define ONE_END_HOLD_TIMEOUT  2u   /* 保持窗超时 */
#define ONE_END_TAG_CHANGED   3u   /* 期望标签消失后读到其他 EPC */
#define ONE_END_UHF_LOST      4u   /* 保持期 UHF 链路失联 (无法证实标签在场) */
#define ONE_END_DEMAG_DONE    5u   /* 消磁标签数达标 (demagCnt>0 时先于其他原因) */
#define ONE_END_STABLE_OK     7u   /* EPC 稳定确认 (demagCnt=0: 保持期连续在场达标 -> 结账成功) */
#define ONE_END_ABORTED      6u   /* 上位机 CANCEL 打断 (安全回降后正常回帧) */

/* ---- 流程阶段 (GET_PROGRESS 查询用) ---- */
#define ONE_PH_NONE        0u   /* 无进行中流程 (空闲) */
#define ONE_PH_PRECHK      1u   /* 前置检查 */
#define ONE_PH_UHF_READY   2u   /* UHF 上电/配置 */
#define ONE_PH_INVENTORY   3u   /* 持续校对: 反复盘点 + EPC 比对 */
#define ONE_PH_RISE        4u   /* 升起 (寻触 KEY_UP) */
#define ONE_PH_HOLD        5u   /* 保持期 (监控/消磁等待) */
#define ONE_PH_LOWER       6u   /* 回降 (寻触 KEY_DOWN) */
#define ONE_PH_IR_WAIT      7u   /* 光电门控: 等待客户放置标签 (PC4) */

/* ---- 失败码 ---- */
#define ONE_ERR_OK             0u
#define ONE_ERR_BUSY           1u   /* Locker/UHF 扫描/电机 占用中 */
#define ONE_ERR_PARAM          2u   /* epcLen 或帧长非法 */
#define ONE_ERR_UHF_OPEN       3u   /* UHF 上电/配置失败 */
#define ONE_ERR_UHF_LINK       4u   /* UHF 通信失败 (初始盘点) */
#define ONE_ERR_NO_TAG         5u   /* 初始盘点未读到任何标签 */
#define ONE_ERR_MISMATCH       6u   /* 读到标签但 != 期望 EPC */
#define ONE_ERR_HOMING         7u   /* 未回零/行程开关错误, 禁止运动 */
#define ONE_ERR_MOTOR_FAULT    8u   /* 升降中电机故障 (含触点不可达) */
#define ONE_ERR_MOTOR_TIMEOUT  9u   /* 升降超时/步数停滞 */
#define ONE_ERR_AM_LINK        10u  /* 消磁流程要求 demagCnt>0: AM 链路断/切消磁模式失败 */
#define ONE_ERR_NO_IR          11u  /* 光电门控: 等待窗内 PC4 未触发 (标签未放置/传感器故障) */

/* ---- 失败时安全回退状态 (err=8/9 时有效) ---- */
#define ONE_RETREAT_OK      0u   /* 已回退至 KEY_DOWN */
#define ONE_RETREAT_FAIL    1u   /* 回退失败, 机构停在半程 */
#define ONE_RETREAT_NONE    2u   /* 失败发生在运动前, 未尝试回退 */

/* ---- 防抖/时限参数 (可按现场漏读率调整) ---- */
#define ONE_INV_PERIOD_MS        1000u  /* 保持阶段盘点周期 */
#define ONE_REMOVED_CONFIRM_MS   3000u  /* 期望标签连续未见达到此时长 -> 稳定移除 */
#define ONE_STABLE_CONFIRM_MS    3000u  /* EPC 稳定确认: 连续在场此时长 -> 结账成功 (demagCnt=0) */
#define ONE_MISMATCH_CONFIRM_ROUNDS 3u  /* 持续校对: 读到标签但连续此轮数无期望 -> MISMATCH */
#define ONE_IR_CONFIRM_MS        200u   /* 光电门控去抖: PC4 连续高此时长 -> 放标触发 */
#define ONE_UHF_LOST_CONFIRM_MS  5000u  /* 链路异常持续此时长 -> 失联回降 */
#define ONE_HOLD_MAX_MS          60000u /* 保持窗硬上限 (maxHoldMs 参数钳位) */
#define ONE_HOLD_DEFAULT_MS      30000u /* maxHoldMs=0 时的默认保持窗 */

/* 结果出参: 由协议层组帧回上位机, 字段随 err 取用 */
typedef struct {
    uint8_t  err;            /* ONE_ERR_* */
    uint8_t  endReason;      /* ONE_END_* (err=0 有效) */
    uint8_t  epcLen;         /* OK:命中EPC长 / MISMATCH:读到第一张非期望EPC长 */
    uint8_t  epc[12];        /* 同上, 原样回传 */
    uint16_t tagsFound;      /* MISMATCH: 初始盘点读到标签总数 */
    uint16_t riseSteps;      /* OK: KEY_DOWN->KEY_UP 实际步数 */
    uint16_t lowerSteps;     /* OK: KEY_UP->KEY_DOWN 实际步数 */
    uint8_t  demagCnt;       /* 请求的消磁标签数 (0=跳过消磁流程) */
    uint8_t  demagDone;      /* OK: 本流程内成功消磁事件数 (demagCnt=0 恒 0) */
    uint8_t  fault, diag1, diag2;   /* MOTOR_*: DRV8434S 寄存器原样 */
    uint32_t steps;          /* MOTOR_*: 出错段已走微步数 */
    uint8_t  phase;          /* MOTOR_*: 1=升段 2=降段 */
    uint8_t  retreat;        /* MOTOR_*: ONE_RETREAT_* */
    uint8_t  lockerState, uhfState, stepperState;  /* BUSY: 三模块状态 */
    uint8_t  switchErr;      /* HOMING: 行程开关错误位 */
    int      uhfRawErr;      /* UHF_*: App_UHF 原始错误码 */
} LockerOneShotResult_t;

/* 流程进行中进度快照 (LOCKER_SUB_GET_PROGRESS 0x09 查询)。
 * 阻塞期间 one_pump 内嵌 Proto_Poll, 上位机可在流程中随时拉取。 */
typedef struct {
    uint8_t  phase;       /* ONE_PH_* */
    uint16_t holdMs;      /* 保持期: 已保持 ms */
    uint8_t  tagPresent;  /* 保持期: 期望标签在场 0/1 */
    uint8_t  demagDone;   /* 本流程成功消磁事件数 */
    uint16_t steps;       /* 升降段: 当前累计微步 (瞬时) */
    uint8_t  epcLen;      /* 期望 EPC (原样回传) */
    uint8_t  epc[12];
} LockerOneShotProgress_t;

/* 同步阻塞执行整个单标签开锁流程 (在 FC_LOCKER_CTRL 分发上下文调用)。
 * 返回时结果已填好; 调用方负责组响应帧。epcLen: 1~12。
 * demagCnt: 请求消磁的 AM 标签数; 0=跳过消磁流程 (不触碰 AM),
 * >0 时期望 EPC 命中后才切 AM 消磁模式, 保持期等待成功消磁事件数
 * 达标即回降, 流程结束切回检测模式 (不再消磁)。 */
void App_LockerOneShot_Run(const uint8_t *epc, uint8_t epcLen,
                           uint16_t tmoMs, uint16_t maxHoldMs, uint8_t demagCnt,
                           LockerOneShotResult_t *out);

/* ---- 流程中交互 (阻塞期间经泵循环内嵌 Proto_Poll 服务) ---- */
uint8_t App_LockerOneShot_IsBusy(void);             /* 0=空闲, 1=流程进行中 */
void    App_LockerOneShot_Abort(void);              /* CANCEL 打断: 安全回降后以 ABORTED 结束 */
void    App_LockerOneShot_GetProgress(LockerOneShotProgress_t *p); /* 进度快照 (非流程时 phase=0) */
void    App_LockerOneShot_Finish(void);             /* Run 返回后由分发层调用, 清 busy/abort */

#endif /* __APP_LOCKERONESHOT_H */
