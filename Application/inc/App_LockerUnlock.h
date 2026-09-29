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
#define UNLK_MAX_FGN_TAGS           10u       /* 外来标签缓存容量 (超出只计数不上报; Round_015 4→10 与 EPC_MAX_TAGS 对齐) */
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
 * Round_014 (2026-09-29) 按客户联调诉求重设计:
 *   帧下发 tmo 秒流程窗 + epcCnt 张 EPC ((len,EPC)×n, 1~10 张, 重复拒绝):
 *   UHF 0x22 多标签连续盘点, 名单内任意非空子集 (A/B/C/AB/AC/BC/ABC 均有效)
 *   稳定在场 (首读起 EPC_STABLE_MS 无 EPC_ABSENT_MS 级断读) + 红外遮挡
 *   且无外来标签介入 -> 升起; 顶部不设固定保持时长, 在场即保持。
 *   期间外来标签介入 (连续 EPC_FGN_MIN_HITS 次读到非名单 EPC) 或 名单
 *   全部离场 -> 异常报警 (红灯 50ms 亮/50ms 灭闪 + 蜂鸣 50ms/500ms,
 *   Round_014 裁决) + 电机快速下行; 报警解除 (外来稳定消失 >=EPC_ABSENT_MS
 *   且有效子集恢复稳定) 后经红外门重新升起, 循环直至 tmo 超窗。
 *   超窗时在顶 -> 回降后 ALL_OK + 蜂鸣 1s; 未升/非顶 -> TIMEOUT。
 *   CANCEL: 全状态复原 (在顶则安全回降后复原), 回 ABORTED。
 *   状态经 GET_PROGRESS 0x09 实时拉取 (v2 布局: motorPh + flags 位图
 *   见 LockerUnlockProgress_t)。
 *   无软标/无消磁。帧布局/终帧见 App_CustomProtocol.h LOCKER_SUB_UNLOCK_EPC。
 * ===================================================================== */

/* ---- EPC 解锁容量/判据可调参数 (Round_014) ---- */
#define EPC_MAX_TAGS                10u    /* 名单容量 (客户裁决: 缓冲 10 张) */
#define EPC_STABLE_MS               1500u  /* 稳定在场判据: 首读起持续在场时长
                                             * (期间断读超 EPC_ABSENT_MS 即重新累计) */
#define EPC_ABSENT_MS               1500u  /* 离场判据: 连续未读到此时长 -> 离场 */
#define EPC_FGN_MIN_HITS            2u     /* 外来介入判据: 累计读到次数 (要快,
                                             * 不等 1.5s 稳定窗) */
#define EPC_ONE_TMO_MS              100u   /* 0x22 每轮盘点超时 (连续盘点接力) */
#define EPC_CONFIRM_READS           10u    /* [0x0A 保留实现专用] 回退监守连读次数 */
#define EPC_ALARM_BEEP_PERIOD_MS    500u   /* 异常报警蜂鸣节拍 */
#define EPC_ALARM_BEEP_MS           50u    /* 异常报警蜂鸣时长 (与红闪 50/50 同步节奏,
                                             * Round_014 裁决) */
#define EPC_TMO_MAX_S               600u   /* tmo 秒上限 (10min, 超出钳位) */
#define EPC_BEEP_FLOW_DONE_MS       1000u  /* 流程结束 (超窗在顶): 1s 持续鸣 */

/* 电机子状态 (GET_PROGRESS v2 motorPh 字节) */
#define UNLK_MPH_NONE               0u   /* 静止 */
#define UNLK_MPH_RISING             1u   /* 上行中 */
#define UNLK_MPH_UP                 2u   /* 顶部保持 */
#define UNLK_MPH_LOWERING           3u   /* 下行中 (正常回降/异常快降) */

/* GET_PROGRESS v2 flags 位图 (bit=1 有效) */
#define UNLK_FLG_VALID_STABLE       0x01u /* 有名单标签稳定在场 (有效子集非空) */
#define UNLK_FLG_FGN_ACTIVE         0x02u /* 外来标签介入中 */
#define UNLK_FLG_ALARM              0x04u /* 异常报警中 */
#define UNLK_FLG_IR_BLOCKED         0x08u /* 红外遮挡 (去抖后) */
#define UNLK_FLG_DEMAG_ON           0x10u /* 0x11: AM 消磁模式运行中 */

/* ---- EPC 解锁结束原因 (err=0 时终帧 endReason 字段) ---- */
#define EPC_END_ALL_OK              1u   /* 超窗时有效子集仍在场 (在顶回降后结账,
                                          * 蜂鸣 1s; 中途曾升起即视为流程完成) */
#define EPC_END_EPC_LOST            2u   /* [Round_014 不再单独触发] 名单全离场回降
                                          * 并不再恢复; 由 TIMEOUT 兜底, 码位保留 */
#define EPC_END_TIMEOUT             3u   /* 超窗时不在顶 (未升起 / 异常回降后未恢复) */
#define EPC_END_UHF_LOST            4u   /* UHF 链路失联 (任意阶段) */
#define EPC_END_ABORTED             6u   /* CANCEL 打断 (安全回降后回帧) */

/* =====================================================================
 * AM 标签解锁流程 (LOCKER_SUB_UNLOCK_AM 0x11) — Round_013 新增独立通道;
 * Round_014 (2026-09-29) 恢复 AM 控制逻辑 (客户裁决 "AM 开启消磁,
 * 标签达标后 AM 挂起关闭消磁, 以收到几个标签完成的消磁帧为主"):
 *   受理 -> SET_MODE(mode 0 消磁, 回显确认; 失败重试 AM_MODE_RETRY 次,
 *   全败按 AM_LINK 结账) -> 计数窗 tmo 秒: 每收到一条 cmd17 消磁帧
 *   立即入账一计 (Round_014 裁决 "收到一个消磁帧立马更新状态, 无需
 *   等待无需稳定, 它只有一次" — 改按帧计数, 不再等 3s 突发静默结算;
 *   每计推 0x0E + 绿闪) -> 达标 (>=amCnt) 或超窗 -> SET_MODE(mode 1
 *   仅检测, 回显确认, 重试同上; 确认失败不阻塞结账, 终帧 amFail=1
 *   如实上报 — 解码器自行恢复消磁的风险客户已知情, 无法根除)。
 *   全程不动电机 (2026-09-19 裁决维持): 蜂鸣 1s + 绿三闪,
 *   rise/lower 恒 0; 窗满未达标按超时结账 (部分完成数如实上报)。
 *   amCnt=0 支路: 无计数门受理即结账 ALL_OK (不耗窗不碰 AM)。
 *   CANCEL: 立即尽力挂起 + ABORTED。无 IR 门控; 全程不碰 UHF。
 * 帧布局/终帧见 App_CustomProtocol.h LOCKER_SUB_UNLOCK_AM。
 * ===================================================================== */

/* ---- AM 解锁可调参数 ---- */
#define AM_TMO_MAX_S                16000u  /* 超时秒钳位: 受理帧 winMs 为
                                             * 3 字节 (上限 16777215ms),
                                             * 16000s*1000 仍在界内 */
#define AM_MODE_RETRY               3u      /* SET_MODE 回显确认重试次数 */

/* ---- AM 解锁结束原因 (err=0 时终帧 endReason 字段) ---- */
#define AM_END_ALL_OK               1u   /* 计数达标 (蜂鸣+绿闪结账, 电机不动) */
#define AM_END_TIMEOUT              3u   /* 窗满未达标 */
#define AM_END_ABORTED              6u   /* CANCEL 打断 (尽力挂起后回帧) */

/* 终帧出参: 由分发层组 0x0A (关闭, 备恢复) / 0x10 EPC / 0x11 AM 回显帧,
 * 字段随 err 取用 */
typedef struct {
    uint8_t  err;             /* UNLK_ERR_* */
    uint8_t  endReason;       /* UNLK_END_* (err=0 有效) */
    uint8_t  total;           /* m (0x10: 名单张数; 0x11: amCnt) */
    uint8_t  confirmed;       /* n (0x10: 最终稳定在场的名单标签数;
                              * 0x11: 实际解锁数) */
    uint8_t  confirmedBitmap;  /* bit i = 清单第 i 张确认 (0x10 最终快照) */
    uint16_t riseSteps;
    uint16_t lowerSteps;
    uint8_t  softCnt;
    uint8_t  softDone;
    uint8_t  amFail;             /* 0x11: 挂起确认失败标志 (Round_014:
                                  * 1=SET_MODE(mode1) 重试全败, 解码器
                                  * 可能仍在消磁; 其余恒 0) */
    uint32_t elapsedMs;       /* 受理 -> 终帧 ms (组帧统一 u16 秒, 封顶) */
    uint8_t  tagsFound;       /* 读到的外来标签累计 (诊断) */
    uint8_t  fgnEpcLen;       /* 最后外来 EPC (失配帧/终帧诊断) */
    uint8_t  fgnEpc[UNLK_EPC_MAX];
    uint8_t  riseCnt;         /* 0x10 (Round_014): 累计升至顶部次数 */
    uint8_t  alarmCnt;        /* 0x10 (Round_014): 异常报警触发次数 */
    uint8_t  fgnSeen;         /* 0x10 (Round_014): 全程是否出现过外来介入 */
    uint8_t  fault, diag1, diag2;   /* MOTOR_*: DRV8434S 原样 */
    uint32_t steps;                 /* MOTOR_*: 出错段已走微步数 */
    uint8_t  motorPhase;            /* MOTOR_*: 1=升段 2=降段 */
    uint8_t  retreat;               /* MOTOR_*: UNLK_RETREAT_* */
    uint8_t  lockerState, uhfState, stepperState;   /* BUSY: 三模块状态 */
    uint8_t  switchErr;             /* HOMING: 行程开关错误位 */
    int      uhfRawErr;             /* UHF_*: App_UHF 原始错误码 */
} LockerUnlockResult_t;

/* 流程进行中进度快照 (GET_PROGRESS 0x09 v2, 0x10/0x11 统一; Round_014):
 *   0x10: total=名单张数, confirmed/bitmap=当前 CONFIRMED 位图,
 *         softCnt=累计升至顶部次数, softDone=异常报警次数,
 *         lastCycle=最近一次电机事件 (0=无 1=升至顶 2=名单全失回降
 *         3=外来介入回降), motorPh=UNLK_MPH_*, flags 见 UNLK_FLG_*。
 *   0x11: total/softCnt=amCnt, confirmed/softDone=已解锁数 (逐帧即时),
 *         motorPh 恒 0, flags 仅 DEMAG_ON。 */
typedef struct {
    uint8_t  phase;           /* UNLK_PH_* */
    uint8_t  total, confirmed, confirmedBitmap;
    uint8_t  softCnt, softDone;
    uint32_t holdMs;          /* 自受理已过 ms (3 字节回填) */
    uint8_t  tagPresent;      /* 有名单标签在场 (TRACKING/CONFIRMED) */
    uint8_t  lastCycle;       /* 0x10: 最近电机事件 0/1升至顶/2全失回降/3外来回降 */
    uint8_t  motorPhase;      /* UNLK_MPH_* (Round_014 v2 新增) */
    uint8_t  flags;           /* UNLK_FLG_* 位图 (Round_014 v2 新增) */
} LockerUnlockProgress_t;

/* 阻塞执行整个多标签解锁流程 (FC_LOCKER_CTRL 分发上下文调用)。
 * epc 为 epcCnt 张连续 EPC (每张 epcLen 字节); channel 为命令来向
 * 通道 (推送帧原路回)。返回时结果已填好, 调用方组终帧。 */
void App_LockerUnlock_Run(const uint8_t *epc, uint8_t epcLen, uint8_t epcCnt,
                          uint16_t tmoMs, uint16_t holdMaxMs, uint8_t softCnt,
                          uint8_t channel, LockerUnlockResult_t *out);

/* EPC 解锁 (LOCKER_SUB_UNLOCK_EPC 0x10, Round_014 重设计): 阻塞自治执行
 * 名单子集稳定确认 (EPC_STABLE_MS) + IR 门控 -> 升起 (监守) -> 在场即保持
 * -> 异常 (外来介入/名单全失) 报警快降 -> 恢复再升 ... 直至 tmoSec 超窗。
 * packed 为 epcCnt 个变长条目 (len, EPC...), 由 EpcRun 内部校验
 * (len 1~12 / 不重复 / 数据长度吻合), 非法回 PARAM。
 * 结果字段 err / EPC_END_* / riseSteps / lowerSteps / riseCnt /
 * alarmCnt / fgnSeen 已填好, 调用方组 0x10 终帧 (布局见 App_CustomProtocol.h)。 */
void App_LockerUnlock_EpcRun(const uint8_t *packed, uint8_t epcCnt,
                             uint16_t packedLen, uint16_t tmoSec,
                             uint8_t channel, LockerUnlockResult_t *out);

/* AM 标签解锁 (LOCKER_SUB_UNLOCK_AM 0x11, Round_014 恢复控制): 受理 ->
 * AM 切消磁 (确认) -> 逐帧计数 (每条 cmd17 立即入账一计) -> 达标/超窗 ->
 * AM 挂起切仅检测 (确认, 失败 amFail=1 如实上报) -> 蜂鸣+绿闪结账
 * (不动电机, rise/lower 恒 0)。amCnt=0: 不耗窗不碰 AM 直接 ALL_OK。
 * 结果字段 err / AM_END_* / softCnt(amCnt) / softDone(实际解锁数) /
 * amFail (1=挂起确认失败) 已填好, 调用方组 0x11 终帧。 */
void App_LockerUnlock_AmRun(uint8_t amCnt, uint16_t tmoSec, uint8_t channel,
                            LockerUnlockResult_t *out);

/* ---- 流程中交互 (阻塞期间经泵循环内嵌 Proto_Poll 服务) ---- */
uint8_t App_LockerUnlock_IsBusy(void);
void    App_LockerUnlock_Abort(void);
void    App_LockerUnlock_GetProgress(LockerUnlockProgress_t *p);
void    App_LockerUnlock_Finish(void);   /* Run 返回后分发层调用, 清态/解绑 */
void    App_LockerUnlock_GetEpcDbg(uint8_t *v);  /* Round_013 调试: [total,tagLen,phase,vMiss,epc[0..7]] (IO_DIAG 带出) */

#endif /* __APP_LOCKERUNLOCK_H */
