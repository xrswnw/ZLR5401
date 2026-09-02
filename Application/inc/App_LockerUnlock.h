#ifndef __APP_LOCKERUNLOCK_H
#define __APP_LOCKERUNLOCK_H

#include <stdint.h>

/* =====================================================================
 * 多标签解锁整合主路径 (LOCKER_SUB_UNLOCK_MULTI 0x0A) — 设计依据:
 * Agent/Round_012/Plan.html (v1)。
 *
 * 单帧下发 m(≤UNLK_MAX_TAGS) 张期望 EPC + 软标数量, 阻塞自治执行:
 *   ⑴ 前置检查 (Locker IDLE / 电机 IDLE / 已回零) -> 受理推 0x0F
 *   ⑵ 光电门控 (PC4, UNLK_IR_CONFIRM_MS 去抖; 窗=W, 窗满 -> err=NO_IR)
 *   ⑶ 消磁准备 (softCnt>0: AM 探链+强制检测模式 — 校验期不消磁; =0 跳过)
 *   ⑷ UHF 就绪 + 持续盘点校对:
 *       每标签独立记录 PENDING/TRACKING/CONFIRMED;
 *       确认判据双门限: 在场连续 UNLK_STABLE_CONFIRM_MS
 *       且累计读到 UNLK_CONFIRM_MIN_HITS 次 (单轮 ~1/4 漏读,
 *       禁"连续N轮"式判据); 离场超 UNLK_PRESENT_WINDOW_MS 回 PENDING。
 *       CONFIRMED 单向掩码 (后续读到静默, 不重报不计数)。
 *       首张确认即升起 KEY_UP (模型甲: 全程保持至硬标段结束)。
 *   ⑸ 每确认一张: 推 0x0B 确认帧 (EPC+序号+已确认/总数+用时)
 *       + 蜂鸣 UNLK_BEEP_TAG_MS + RGB 绿单闪。
 *       清单外 EPC 不升不计数; 连续 UNLK_MISMATCH_CONFIRM_ROUNDS 轮
 *       只读到外来标签且无任何清单标签在场 -> 推 0x0C 失配帧 + 红闪 3s,
 *       不终止 (客户可能换正确标签, 窗口兜底)。
 *   ⑹ 硬标段结束 (n==m ALL_OK / 窗 W 满 PARTIAL / CANCEL / UHF 失联):
 *       推 0x0D 硬标完成帧 -> 回降 KEY_DOWN (免疫打断)。
 *   ⑺ 软标解码 (softCnt>0): 入口切 AM 消磁模式 (EPC 校验已通过),
 *       消磁事件计数每+1 推 0x0E; 达标或 UNLK_SOFT_WINDOW_MS 窗满
 *       (以实况结账), 出口切回检测模式 (不再消磁)。
 *   ⑻ 结账终帧 (0x0A 回显): err=0 + endReason + 位图 + 计数 + 步数。
 *
 * 解锁窗口 W: holdMaxMs=0 -> 公式
 *   UNLK_HOLD_BASE_MS + (m-1)*UNLK_HOLD_EXTRA_PER_TAG_MS
 * ("预留2min, 每多一标签加30s" 直译); 非零取 min(host, UNLK_HOLD_MAX_MS)。
 * W 兼任光电等待窗与校对预算, 自 IR 触发时刻起算。
 *
 * 解锁态 (受理至终帧发完): 仅响应 CANCEL / GET_PROGRESS,
 * 其余命令 (含 0x08/CONFIGURE/START/MOTOR/UHF/AM 控制) 回 BUSY。
 * ===================================================================== */

/* ---- 容量 (64B 单帧: 数据区 ~57B, 固定开销 8B + 12B/张) ---- */
#define UNLK_MAX_TAGS               4u
#define UNLK_EPC_MAX                12u

/* ---- 流程阶段 (GET_PROGRESS / 受理帧) ---- */
#define UNLK_PH_NONE                0u
#define UNLK_PH_WAIT_TAG            1u   /* 光电门控: 等待放标 (PC4) */
#define UNLK_PH_VERIFY              2u   /* 持续校对 (盘点+逐张确认) */
#define UNLK_PH_RISE                3u   /* 首确认后升起 (寻触 KEY_UP) */
#define UNLK_PH_LOWER               4u   /* 硬标段结束回降 (寻触 KEY_DOWN) */
#define UNLK_PH_SOFT                5u   /* 软标解码计数 */
#define UNLK_PH_DONE                6u   /* 终帧组包 (瞬时) */

/* ---- 失败码 (编号对齐 ONE_ERR_*) ---- */
#define UNLK_ERR_OK                 0u
#define UNLK_ERR_BUSY               1u   /* Locker/UHF/电机 占用中 */
#define UNLK_ERR_PARAM              2u   /* 帧长/epcCnt/epcLen 非法 */
#define UNLK_ERR_UHF_OPEN            3u
#define UNLK_ERR_UHF_LINK            4u
#define UNLK_ERR_HOMING             7u   /* 未回零/行程开关错误 */
#define UNLK_ERR_MOTOR_FAULT        8u
#define UNLK_ERR_MOTOR_TIMEOUT      9u
#define UNLK_ERR_AM_LINK            10u  /* softCnt>0: AM 链路断/切模式失败 */
#define UNLK_ERR_NO_IR              11u  /* 光电等待窗内 PC4 未触发 */

/* ---- 结束原因 (err=0 时硬标段/整程触发者) ---- */
#define UNLK_END_ALL_OK             1u   /* n==m 全部确认 */
#define UNLK_END_PARTIAL_TIMEOUT    2u   /* 窗 W 满, 部分确认 (位图明示) */
#define UNLK_END_UHF_LOST           4u   /* UHF 链路失联确认窗仍坏 */
#define UNLK_END_ABORTED            6u   /* CANCEL 打断 (安全回降后回帧) */

/* ---- 失败时安全回退状态 (err=8/9) ---- */
#define UNLK_RETREAT_OK             0u
#define UNLK_RETREAT_FAIL           1u
#define UNLK_RETREAT_NONE           2u   /* 失败发生在运动前 */

/* ---- 可调参数 (宏集中, 台架/现场按需调整) ---- */
#define UNLK_IR_CONFIRM_MS          200u     /* IR 触发去抖 */
#define UNLK_STABLE_CONFIRM_MS      3000u     /* 确认门限A: 在场连续时长 */
#define UNLK_PRESENT_WINDOW_MS      3000u     /* 在场判定窗 (容忍 2~3 轮漏读) */
#define UNLK_CONFIRM_MIN_HITS       2u        /* 确认门限B: 累计最少命中 */
#define UNLK_MISMATCH_CONFIRM_ROUNDS 3u       /* 外来独占连续轮数 -> 失配帧 */
#define UNLK_UHF_LOST_CONFIRM_MS    5000u     /* 链路异常确认窗 */
#define UNLK_INVENTORY_TMO_MS       1000u     /* 每轮盘点时限 (tmoMs=0 缺省) */
#define UNLK_HOLD_BASE_MS           120000u   /* W 基数: 首标签 2min */
#define UNLK_HOLD_EXTRA_PER_TAG_MS  30000u    /* W 增量: 每多一标签 30s */
#define UNLK_HOLD_MAX_MS            240000u   /* W 绝对上限 */
#define UNLK_BEEP_TAG_MS            200u      /* 每张确认提示音 */
#define UNLK_BEEP_DONE_MS           300u      /* 结账完成提示音 */
#define UNLK_SOFT_WINDOW_MS         300000u   /* 软标窗 (5min, 满窗以实况结账) */

/* 终帧出参: 由分发层组 0x0A 回显帧, 字段随 err 取用 */
typedef struct {
    uint8_t  err;             /* UNLK_ERR_* */
    uint8_t  endReason;       /* UNLK_END_* (err=0 有效) */
    uint8_t  total;           /* m */
    uint8_t  confirmed;       /* n */
    uint8_t  confirmedBitmap;  /* bit i = 清单第 i 张已确认 */
    uint16_t riseSteps;
    uint16_t lowerSteps;
    uint8_t  softCnt;
    uint8_t  softDone;
    uint32_t elapsedMs;       /* IR 触发 -> 终帧 (cap 0xFFFF 组帧) */
    uint8_t  tagsFound;       /* 校对期读到的外来标签累计 (诊断) */
    uint8_t  fgnEpcLen;       /* 最后外来 EPC (失配帧/终帧诊断) */
    uint8_t  fgnEpc[UNLK_EPC_MAX];
    uint8_t  fault, diag1, diag2;   /* MOTOR_*: DRV8434S 原样 */
    uint32_t steps;                 /* MOTOR_*: 出错段已走微步数 */
    uint8_t  motorPhase;            /* MOTOR_*: 1=升段 2=降段 */
    uint8_t  retreat;               /* MOTOR_*: UNLK_RETREAT_* */
    uint8_t  lockerState, uhfState, stepperState;   /* BUSY: 三模块状态 */
    uint8_t  switchErr;             /* HOMING: 行程开关错误位 */
    int      uhfRawErr;             /* UHF_*: App_UHF 原始错误码 */
} LockerUnlockResult_t;

/* 流程进行中进度快照 (GET_PROGRESS 0x09, 0x0A 流程时为多标签版布局) */
typedef struct {
    uint8_t  phase;           /* UNLK_PH_* */
    uint8_t  total, confirmed, confirmedBitmap;
    uint8_t  softCnt, softDone;
    uint32_t holdMs;          /* 自 IR 触发已过 ms (未触发=0); Round_011: W 超 16bit, 3 字节回填 */
    uint8_t  tagPresent;      /* 有待确认期望标签在场 */
} LockerUnlockProgress_t;

/* 阻塞执行整个多标签解锁流程 (FC_LOCKER_CTRL 分发上下文调用)。
 * epc 为 epcCnt 张连续 EPC (每张 epcLen 字节); channel 为命令来向
 * 通道 (推送帧原路回)。返回时结果已填好, 调用方组终帧。 */
void App_LockerUnlock_Run(const uint8_t *epc, uint8_t epcLen, uint8_t epcCnt,
                          uint16_t tmoMs, uint16_t holdMaxMs, uint8_t softCnt,
                          uint8_t channel, LockerUnlockResult_t *out);

/* ---- 流程中交互 (阻塞期间经泵循环内嵌 Proto_Poll 服务) ---- */
uint8_t App_LockerUnlock_IsBusy(void);
void    App_LockerUnlock_Abort(void);
void    App_LockerUnlock_GetProgress(LockerUnlockProgress_t *p);
void    App_LockerUnlock_Finish(void);   /* Run 返回后分发层调用, 清态/解绑 */

#endif /* __APP_LOCKERUNLOCK_H */
