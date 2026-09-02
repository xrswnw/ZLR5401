#ifndef __APP_LOCKER_H
#define __APP_LOCKER_H

#include <stdint.h>

/* =====================================================================
 * 开锁器业务编排层
 *  - 依据《约束/Link.txt》运行逻辑:
 *      上位机下发「硬标签 EPC 清单 + 软标数量 N」-> 比对硬标签 EPC:
 *      一致亮绿灯升起开锁 / 不一致红灯闪烁不升起, 比对结果回传上位机;
 *      硬标签预留 2min 开锁时间, 每多一个标签 +30s;
 *      读到不同 EPC 且已解锁数 n 达到硬标签数 m 时, 进入软标阶段;
 *      软标阶段每个软标消耗一次解码数量, 次数用尽则结账完成。
 *  - 编排已实现的模块: App_UHF(硬标签EPC读) + App_Stepper(升降开锁)
 *                      + App_MotorHoming(行程基准) + App_AM(软标解码器)
 *  - 磁块升降为 KEY_UP/KEY_DOWN 行程开关寻触 (非定步数), 判据与
 *    App_MotorHoming 一致 (2000Hz / 40%)。
 *  - 状态机由 App_Locker_Process 主循环非阻塞节拍驱动, 事件经
 *    LOCKER_SUB_GET_EVENT 拉取回上位机。
 * ===================================================================== */

/* ---- 硬标签清单容量 (v1: 单帧/多帧 ADD 构建) ----
 * 每硬标签 EPC 上限 12B; 清单上限 16. 上位机单帧受 HID 56B data 限制,
 * 一帧装不满时可连续 LOCKER_SUB_ADD 追加, START 前累积。 */
#define APP_LOCKER_MAX_HARD   16u
#define APP_LOCKER_MAX_EPC   12u

/* ---- 上报事件码 (LOCKER_SUB_GET_EVENT) ---- */
typedef enum {
    LOCKER_EVT_MATCH_OK   = 0,   /* 硬标签匹配, 亮绿灯升起开锁 */
    LOCKER_EVT_MISMATCH   = 1,   /* 非清单 EPC, 红灯闪烁不升起 */
    LOCKER_EVT_HARD_DONE  = 2,   /* 硬标签 m==n 全部解锁, 进入软标 */
    LOCKER_EVT_SOFT_USED  = 3,   /* 消耗一次软标解码 */
    LOCKER_EVT_TIMEOUT    = 4,   /* 开锁保持窗口超时 */
    LOCKER_EVT_DONE       = 5,   /* 结账完成 */
    LOCKER_EVT_FAULT      = 6    /* 故障 */
} AppLockerEvent_t;

/* ---- 状态机状态 (LOCKER_SUB_QUERY data[2] 回传) ---- */
typedef enum {
    LOCKER_IDLE        = 0,   /* 空闲: 无结账任务, 默认锁定 */
    LOCKER_CONFIGURED  = 1,   /* 已配置+START: 扫描等待硬标签比对 */
    LOCKER_UNLOCK_HOLD = 2,   /* 已有标签匹配, 磁块升起保持, 继续比对剩余 */
    LOCKER_SOFT_DECODE = 3,   /* 硬标签全部解锁, 软标解码阶段 */
    LOCKER_DONE        = 4,   /* 结账完成, 停留片刻后回降 */
    LOCKER_FAULT       = 5,   /* 故障 (电机/链路); CANCEL 可退出 */
    LOCKER_LOWERING    = 6    /* 回降中: 寻触 KEY_DOWN 完成后回 IDLE */
} AppLockerState_t;

/* ---- 单条硬标签清单项 ---- */
typedef struct {
    uint8_t epc[APP_LOCKER_MAX_EPC];
    uint8_t epcLen;
    uint8_t matched;          /* 是否已解锁 */
} AppLockerItem_t;

/* ---- 结账任务上下文 (状态机工作集) ---- */
typedef struct {
    AppLockerState_t state;
    AppLockerItem_t  items[APP_LOCKER_MAX_HARD];
    uint16_t hardCount;       /* 硬标签总数 m */
    uint16_t hardMatched;     /* 已解锁硬标签数 n */
    uint16_t softCount;       /* 软标解码数量 N */
    uint16_t softUsed;        /* 已消耗软标解码次数 */
    uint32_t holdDeadlineMs;  /* 当前阶段截止时间戳 (0=无) */
    uint16_t holdTagIndex;    /* 最近匹配的硬标签索引 */
    uint8_t  lockRisen;       /* 磁块已升至 KEY_UP */
} AppLockerCtx_t;

/* ---- 配置 (可调参数) ---- */
#define APP_LOCKER_BASE_HOLD_MS      (2u * 60u * 1000u)  /* 2min 基础开锁时间 */
#define APP_LOCKER_EXTRA_PER_TAG_MS  (30u * 1000u)       /* 每多一标签 +30s */
#define APP_LOCKER_DONE_IDLE_MS      5000u               /* 结账完成后停留再回降 */
#define APP_LOCKER_SOFT_WINDOW_MS    (5u * 60u * 1000u)  /* 软标阶段兜底窗口 */

/* ---- 寻触升降工况 (与 App_MotorHoming 实测一致) ----
 * 定步数 4800 旧方案已废弃: 实测行程 4287/4280, 超程会硬顶挡块。 */
#define APP_LOCKER_SEEK_SPEED_HZ     2000u
#define APP_LOCKER_SEEK_TORQUE_PCT   40u
#define APP_LOCKER_SEEK_RISE_MAX     (4085u * 2u + 800u)  /* 上行超步兜底 */
#define APP_LOCKER_SEEK_LOWER_MAX    (4324u * 2u + 800u)  /* 下行超步兜底 */
#define APP_LOCKER_SEEK_STALL_MS     400u                 /* 步数停滞判丢步 */
#define APP_LOCKER_SEEK_TMO_MS       10000u               /* 单段寻触超时 */
/* 注: 寻触失败自动重试一次 (nFAULT 瞬态 + 启动全失步), 旧瞬态专用阈值已并入. */

/* ---- MISMATCH 红闪指示: 已迁移至 RGB 灯带 (App_RgbLed_Pattern
 *  RGBFLASH_MISMATCH_3S, 500ms 周期/250ms 亮窗, 与旧节奏一致) ---- */
#define APP_LOCKER_DONE_BEEP_MS      300u   /* 结账完成蜂鸣提示时长 */

/* ---- 初始化 / 周期处理 ---- */
void App_Locker_Init(void);
void App_Locker_Process(void);   /* 主循环非阻塞节拍: 推进状态机+寻触 */

/* ---- 控制 API (由协议层调用) ---- */
int  App_Locker_Configure(const AppLockerItem_t *items, uint16_t hardCount,
                          uint16_t softCount);   /* 全量重建结账任务 (仅 IDLE) */
int  App_Locker_AddTag(const AppLockerItem_t *item);  /* 追加硬标签 (仅 START 前) */
int  App_Locker_Start(void);       /* 激活: 纯软标直入 SOFT, 否则开扫 CONFIGURED */
int  App_Locker_Cancel(void);      /* 取消: 磁块升起中则先回降 (LOWERING) 再回 IDLE */
int  App_Locker_StopDecode(void);  /* 消耗一次软标解码 (v1 软标驱动) */

/* ---- 状态 / 信息 (供协议查询) ---- */
AppLockerState_t App_Locker_GetState(void);
uint8_t App_Locker_GetFaultReason(void);   /* 最近一次 FAULT 原因码 (诊断) */
void App_Locker_GetCtx(AppLockerCtx_t *ctx);
int  App_Locker_IsIdle(void);

/* ---- 上报事件拉取 (协议层取走回上位机) ---- */
int App_Locker_PopEvent(uint8_t *code, uint8_t *epc, uint8_t *epcLen,
                        uint16_t *hardMatched, uint16_t *softUsed,
                        uint16_t *softCount, uint16_t *hardCount);

#endif /* __APP_LOCKER_H */
