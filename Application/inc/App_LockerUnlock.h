#ifndef __APP_LOCKERUNLOCK_H
#define __APP_LOCKERUNLOCK_H

#include <stdint.h>

/* =====================================================================
 * 多标签解锁整合主路径 (LOCKER_SUB_UNLOCK_MULTI 0x0A) — 设计依据:
 * Agent/Round_012/Plan.html (v1)。
 *
 * 单帧下发 m(≤UNLK_MAX_TAGS) 张期望 EPC + 软标数量, 阻塞自治执行:
 *   ⑴ 前置检查 (Locker IDLE / 电机 IDLE / 已回零) -> 受理推 0x0F
 *       epcCnt=0 且 softCnt>0: 纯软标结账 — 跳过 ⑵⑷⑸ (不门控光电、
 *       不校验 EPC), 受理帧 phase=SOFT, 直接进入 升起+消磁+软解码;
 *       流程计时基准=受理时刻, 窗口即软标窗。
 *   ⑵ 光电门控 (PC11, UNLK_IR_CONFIRM_MS 去抖; 窗=W, 窗满 -> err=NO_IR)
 *   ⑶ 消磁准备 (softCnt>0: AM 探链+强制检测模式 — 校验期不消磁; =0 跳过)
 *   ⑷ UHF 就绪 + 持续盘点校对:
 *       每标签独立记录 PENDING/TRACKING/CONFIRMED;
 *       确认判据双门限: 在场连续 UNLK_STABLE_CONFIRM_MS
 *       且累计读到 UNLK_CONFIRM_MIN_HITS 次 (单轮 ~1/4 漏读,
 *       禁"连续N轮"式判据); 离场超 UNLK_PRESENT_WINDOW_MS 回 PENDING。
 *       CONFIRMED 单向掩码 (后续读到静默, 不重报不计数)。
 *   ⑸ 每确认一张: 推 0x0B 确认帧 (EPC+序号+已确认/总数+用时)
 *       + 蜂鸣 UNLK_BEEP_TAG_MS + RGB 绿单闪。
 *       清单外 EPC 独立缓存 (≤UNLK_MAX_FGN_TAGS 张, 裁决3): 稳定
 *       判据同期望标签 (双门限), 稳定一张推一帧 0x0C 失配帧
 *       + 红闪 3s (单向掩码不重报), 不终止 (窗 W 兜底)。
 *   ⑹ 硬标段结束 (n==m ALL_OK / 窗 W 满 PARTIAL / CANCEL / UHF 失联):
 *       n==m 才升起 KEY_UP (全确认门控: 任一 EPC 未校验通过磁块
 *       全程不动), softCnt>0 同时切 AM 消磁 (裁决4: 电机至上行程
 *       开关与消磁同时开启, 保持至整个流程结束)。
 *       推 0x0D 硬标完成帧 (PARTIAL/UHF_LOST: 无升降无消磁直接结账)。
 *   ⑺ 软标解码 (仅 ALL_OK 进入): 升起时已开消磁 (失败则入口再试
 *       一次, 再败按 AM_LINK 结账), 消磁事件计数每+1 推 0x0E;
 *       达标即收, UNLK_SOFT_WINDOW_MS 窗满未达标按超时失败结账
 *       (endReason=SOFT_TIMEOUT, 裁决5)。
 *   ⑻ 整个流程结束统一回降 KEY_DOWN (免疫打断) -> 结账终帧
 *       (0x0A 回显): err=0 + endReason + 位图 + 计数 + 步数。
 *       AM 消磁模式由 Run 出口统一切回仅检测 (裁决1), 不恢复
 *       流程前预设。
 *
 * 解锁窗口 W: holdMaxMs=0 -> 公式
 *   UNLK_HOLD_BASE_MS + (m-1)*UNLK_HOLD_EXTRA_PER_TAG_MS
 * ("预留2min, 每多一标签加30s" 直译); 非零取 min(host, UNLK_HOLD_MAX_MS)。
 * W 兼任光电等待窗与校对预算, 自 IR 触发时刻起算。
 *
 * 解锁态 (受理至终帧发完): 仅响应 CANCEL / GET_PROGRESS,
 * 其余命令 (含 0x08/CONFIGURE/START/MOTOR/UHF/AM 控制) 回 BUSY。
 *
 * [Round_013] 0x0A 多标签流程分发层注释关闭 (码位显式回 PARAM, 实现
 * 保留于 App_Dispatch.c #if 0 备恢复), 解锁业务改走 0x10 UNLOCK_EPC
 * (App_LockerUnlock_EpcRun, 见下); 灯语槽位 RGBSRC_UNLOCK 与
 * UNLK_ERR_* 错误码移交新流程接管。
 * ===================================================================== */

/* ---- 容量 (64B 单帧: 数据区 ~57B, 固定开销 8B + 12B/张) ---- */
#define UNLK_MAX_TAGS               4u
#define UNLK_EPC_MAX                12u

/* ---- 流程阶段 (GET_PROGRESS / 受理帧) ---- */
#define UNLK_PH_NONE                0u
#define UNLK_PH_WAIT_TAG            1u   /* 光电门控: 等待放标 (PC11) */
#define UNLK_PH_VERIFY              2u   /* 持续校对 (盘点+逐张确认) */
#define UNLK_PH_RISE                3u   /* 全部确认后升起 (寻触 KEY_UP) */
#define UNLK_PH_LOWER               4u   /* 硬标段结束回降 (寻触 KEY_DOWN) */
#define UNLK_PH_SOFT                5u   /* 软标解码计数 */
#define UNLK_PH_DONE                6u   /* 终帧组包 (瞬时) */
#define UNLK_PH_HOLD                7u   /* EPC 解锁 (0x10): 顶部保持 EPC 在场监守 */

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
#define UNLK_ERR_NO_IR              11u  /* 光电等待窗内 PC11 未触发 */

/* ---- 结束原因 (err=0 时整程触发者; 终帧 endReason 字段) ---- */
#define UNLK_END_ALL_OK             1u   /* n==m 全部确认 */
#define UNLK_END_PARTIAL_TIMEOUT    2u   /* 硬标窗 W 满, 部分确认 (位图明示) */
#define UNLK_END_UHF_LOST           4u   /* UHF 链路失联确认窗仍坏 */
#define UNLK_END_ABORTED            6u   /* CANCEL 打断 (安全回降后回帧) */
#define UNLK_END_SOFT_TIMEOUT       7u   /* 软标窗 5min 满未校验完成 (裁决5: 按超时失败结账) */

/* ---- 失败时安全回退状态 (err=8/9) ---- */
#define UNLK_RETREAT_OK             0u
#define UNLK_RETREAT_FAIL           1u
#define UNLK_RETREAT_NONE           2u   /* 失败发生在运动前 */

/* ---- 可调参数 (宏集中, 台架/现场按需调整) ---- */
#define UNLK_IR_CONFIRM_MS          200u     /* IR 触发去抖 */
#define UNLK_STABLE_CONFIRM_MS      1500u     /* 确认门限A: 在场连续时长 (裁决2: 1.5s) */
#define UNLK_PRESENT_WINDOW_MS      3000u     /* 在场判定窗 (容忍数轮漏读) */
#define UNLK_CONFIRM_MIN_HITS       2u        /* 确认门限B: 累计最少命中 */
#define UNLK_MAX_FGN_TAGS           4u        /* 外来标签缓存容量 (超出只计数不上报, 裁决3) */
#define UNLK_UHF_LOST_CONFIRM_MS    5000u     /* 链路异常确认窗 */
#define UNLK_INVENTORY_TMO_MS       500u      /* 每轮盘点时限 (tmoMs=0 缺省, 需求: 默认盘点500Ms) */
#define UNLK_HOLD_BASE_MS           120000u   /* W 基数: 首标签 2min */
#define UNLK_HOLD_EXTRA_PER_TAG_MS  30000u    /* W 增量: 每多一标签 30s */
#define UNLK_HOLD_MAX_MS            240000u   /* W 绝对上限 */
#define UNLK_BEEP_TAG_MS            200u      /* 每张确认提示音 */
#define UNLK_BEEP_DONE_MS           300u      /* 结账完成提示音 */
#define UNLK_SOFT_WINDOW_MS         300000u   /* 软标窗 (5min, 满窗以实况结账) */

/* =====================================================================
 * EPC 解锁流程 (LOCKER_SUB_UNLOCK_EPC 0x10) — Round_013 新增独立通道,
 * 旧 0x0A 多标签流程同步关闭 (灯语槽位 RGBSRC_UNLOCK 与 UNLK_ERR_*
 * 错误码移交本流程):
 *   下发单张 EPC -> IR 门控 (单轮=窗 W 窗满 err=NO_IR) -> EPC 连续
 *   EPC_CONFIRM_READS 次读到且一致即稳定在场 (0x21 逐发 ~27ms/发,
 *   纯计数判据, Round_013 裁决: 流畅迅速; 确认即蜂鸣+绿单闪+0x0B) ->
 *   升起寻触 KEY_UP (期间盘点不停, 移除/更换即停机收起) -> 顶部保持
 *   holdTopMs (连续 EPC_LOST_READS 次未读到即稳定移除) -> 周期末回降
 *   KEY_DOWN (免疫 CANCEL)。
 *   循环模式 (timeoutSec>0, Round_013 裁决③): 受理起 timeoutSec 秒内
 *   周期往复; 每轮回退后须先 标签移走 (连续空射) 且红外回落, 再经
 *   红外触发 + EPC 稳定才开下一轮; 流程窗在周期边界判定。周期失败
 *   (移除/更换) 不推帧, 状态由 GET_PROGRESS 拉取 (softCnt=周期数,
 *   softDone=成功数, lastCycle=上轮结果)。timeoutSec=0 单轮模式:
 *   保持原语义 (周期失败即流程结束)。
 *   无软标/无消磁/无外来标签失配上报; 外来标签仅诊断记账 (终帧)。
 * 帧布局/终帧见 App_CustomProtocol.h LOCKER_SUB_UNLOCK_EPC。
 * ===================================================================== */

/* ---- EPC 解锁可调参数 ---- */
#define EPC_ONE_TMO_MS              100u    /* 0x21 单发盘点超时缺省 (tmo=0):
                                             * 命中实测 ~27ms 即返, 空射至多此时长 */
#define EPC_HOLD_TOP_MS             3000u    /* 顶部保持时长缺省 (holdTop=0) */
#define EPC_HOLD_TOP_MAX_MS          60000u  /* 顶部保持时长上限 */
#define EPC_CONFIRM_READS           10u     /* 连续读到且 EPC 一致次数 -> 稳定在场
                                             * (2026-09-19 用户裁决: 3→10, 抗读区
                                             * 边缘抖动; ~10×27ms ≈ 270ms 判据) */
#define EPC_LOST_READS              10u     /* 连续未读到次数 -> 稳定移除 (保持期监守;
                                             * 2026-09-19 用户裁决: 3→10, 判定延迟
                                             * ~10×tmo, 1000 微步/s 回退段余量足) */
#define EPC_BEEP_STABLE_EVERY       3u      /* [已废除 2026-09-19] 解锁期间 EPC
                                             * 盘点心跳蜂鸣移除: 远距边缘标签
                                             * 偶发凑齐判据致电机乱跑 */
#define EPC_BEEP_STABLE_MS          50u     /* [已废除, 见上] */
#define EPC_BEEP_CYCLE_END_MS       800u    /* 周期成功 (3s 保持满在场) 电机
                                             * 开始下回才响 800ms — 不等回到底;
                                             * 失败路径 (移走/更换下回) 不响
                                             * (2026-09-19 裁决: 蜂鸣只留
                                             * 保持满回降 + 流程结束两处) */
#define EPC_BEEP_FLOW_DONE_MS       1000u   /* 总流程结束 (时间到): 1s 持续鸣 */

/* ---- EPC 解锁结束原因 (err=0 时终帧 endReason 字段) ---- */
#define EPC_END_ALL_OK              1u   /* 保持期满 EPC 仍在场: 正常回降结账
                                          * (循环模式: 流程窗满且有成功周期) */
#define EPC_END_EPC_LOST            2u   /* 单轮: 保持期/升起期 EPC 消失, 回降后结束
                                          * (循环模式周期失败不以此结束流程) */
#define EPC_END_TIMEOUT             3u   /* 单轮: W 窗满 EPC 未确认; 循环: 流程窗满
                                          * 且无成功周期 (磁块未动) */
#define EPC_END_UHF_LOST            4u   /* UHF 链路失联 (校对期/保持期) */
#define EPC_END_ABORTED             6u   /* CANCEL 打断 (安全回降后回帧) */

/* =====================================================================
 * AM 标签解锁流程 (LOCKER_SUB_UNLOCK_AM 0x11) — Round_013 新增独立通道:
 *   受理起 tmo 秒窗内被动计数 AM 消磁成功事件 (cmd17 突发结算,
 *   deactCount 增量), 达标 (>=amCnt) 即结账 — 全程不动电机
 *   (2026-09-19 裁决 "AM 解锁完成后不要动作电机, 直接蜂鸣器动作
 *   即可"): 蜂鸣 1s + 绿三闪, rise/lower 恒 0; 窗满未达标按超时结账。
 *   amCnt=0 支路: 无计数门受理即达标, 不耗窗直接结账 ALL_OK。
 *   AM 解码器零控制 (2026-09-19 裁决 "AM 设备从上电到结束不要控制,
 *   按照默认参数即可, 仅在解锁期间监控解锁帧"): 不探链/不切模式/
 *   不下发参数 (main.c 上电零下发), 流程仅被动收 cmd17; 解码器静默
 *   (含挂起/自锁) 按窗满结账, 不报 AM_LINK。
 *   CANCEL: 窗内即回 ABORTED (无电机, 无安全回落问题)。
 *   无 IR 门控/无循环/无升起段监守 (已消磁标签必静默); 全程不碰 UHF。
 *   窗末 cmd17 突发未结算时宽限至多 AM_BURST_GAP_MS 等其入账 (cmd17
 *   成功事件须静默 3s 才结算, 窗末消磁的标签不得漏计)。
 * 帧布局/终帧见 App_CustomProtocol.h LOCKER_SUB_UNLOCK_AM。
 * ===================================================================== */

/* ---- AM 解锁可调参数 ---- */
#define AM_TMO_MAX_S                16000u  /* 超时秒钳位: 受理帧 winMs 为
                                             * 3 字节 (上限 16777215ms),
                                             * 16000s*1000 仍在界内 */

/* ---- AM 解锁结束原因 (err=0 时终帧 endReason 字段) ---- */
#define AM_END_ALL_OK               1u   /* 计数达标 (蜂鸣+绿闪结账, 电机不动) */
#define AM_END_TIMEOUT              3u   /* 窗满未达标 */
#define AM_END_ABORTED              6u   /* CANCEL 打断 (窗内即回, 电机未动) */

/* 终帧出参: 由分发层组 0x0A (关闭, 备恢复) / 0x10 EPC 回显帧, 字段随 err 取用 */
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
    uint8_t  amFail;             /* 0x11: 消磁失败事件数 (判据证伪后恒 0, 保留) */
    uint32_t elapsedMs;       /* IR 触发 -> 终帧 ms (组帧统一 u16 秒, 封顶) */
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

/* 流程进行中进度快照 (GET_PROGRESS 0x09, 统一布局; 0x10 EPC 流程时
 * total=1, softCnt=周期数, softDone=成功周期数, lastCycle=上轮结果,
 * phase 含 UNLK_PH_HOLD=7 顶部保持) */
typedef struct {
    uint8_t  phase;           /* UNLK_PH_* */
    uint8_t  total, confirmed, confirmedBitmap;
    uint8_t  softCnt, softDone;
    uint32_t holdMs;          /* 自 IR 触发 (循环模式=受理) 已过 ms (3 字节回填) */
    uint8_t  tagPresent;      /* 有待确认期望标签在场 */
    uint8_t  lastCycle;       /* 0x10: 上轮结果 0=无/进行中 1=成功 2=移除 3=更换 */
} LockerUnlockProgress_t;

/* 阻塞执行整个多标签解锁流程 (FC_LOCKER_CTRL 分发上下文调用)。
 * epc 为 epcCnt 张连续 EPC (每张 epcLen 字节); channel 为命令来向
 * 通道 (推送帧原路回)。返回时结果已填好, 调用方组终帧。 */
void App_LockerUnlock_Run(const uint8_t *epc, uint8_t epcLen, uint8_t epcCnt,
                          uint16_t tmoMs, uint16_t holdMaxMs, uint8_t softCnt,
                          uint8_t channel, LockerUnlockResult_t *out);

/* EPC 解锁 (LOCKER_SUB_UNLOCK_EPC 0x10, Round_013 新): 阻塞自治执行
 * IR 门控 + 单张 EPC 计数稳定确认 -> 升起寻触 KEY_UP (期间盘点监守) ->
 * 顶部保持 -> 周期末回降; timeoutSec=0 单轮 / >0 循环模式 (受理起
 * timeoutSec 秒内周期往复, 周期失败不推帧, 状态由 GET_PROGRESS 拉取)。
 * 结果字段 err / EPC_END_* / riseSteps / lowerSteps / softCnt(周期数) /
 * softDone(成功数) 已填好, 调用方组 0x10 终帧 (布局见 App_CustomProtocol.h)。 */
void App_LockerUnlock_EpcRun(const uint8_t *epc, uint8_t epcLen, uint16_t tmoMs,
                             uint16_t winMaxMs, uint16_t holdTopMs, uint8_t timeoutSec,
                             uint8_t channel, LockerUnlockResult_t *out);

/* AM 标签解锁 (LOCKER_SUB_UNLOCK_AM 0x11, Round_013 新): 阻塞自治执行
 * 计数窗 (受理起 tmoSec 秒内被动数消磁成功事件) -> 达标 (>=amCnt) 即
 * 蜂鸣+绿闪结账 (不动电机, rise/lower 恒 0); 窗满未达标按超时结账。
 * amCnt=0: 无计数门受理即达标, 不耗窗直接结账 ALL_OK。
 * AM 零控制 (2026-09-19 裁决): 不探链/不切模式/不下发参数, 仅被动收
 * cmd17。结果字段 err / AM_END_* / softCnt(amCnt) / softDone(实际解锁数) /
 * amFail (判据证伪后恒 0, 保留) 已填好, 调用方组 0x11 终帧。 */
void App_LockerUnlock_AmRun(uint8_t amCnt, uint16_t tmoSec, uint8_t channel,
                            LockerUnlockResult_t *out);

/* ---- 流程中交互 (阻塞期间经泵循环内嵌 Proto_Poll 服务) ---- */
uint8_t App_LockerUnlock_IsBusy(void);
void    App_LockerUnlock_Abort(void);
void    App_LockerUnlock_GetProgress(LockerUnlockProgress_t *p);
void    App_LockerUnlock_Finish(void);   /* Run 返回后分发层调用, 清态/解绑 */
void    App_LockerUnlock_GetEpcDbg(uint8_t *v);  /* Round_013 调试: [total,tagLen,phase,vMiss,epc[0..7]] (IO_DIAG 带出) */

#endif /* __APP_LOCKERUNLOCK_H */
