#ifndef __APP_LOCKER_H
#define __APP_LOCKER_H

#include <stdint.h>

/* =====================================================================
 * 开锁器业务编排层 (初版功能)
 *  - 依据《约束/Link.txt》运行逻辑:
 *      上位机下发「硬标签 EPC 清单 + 软标数量 N」-> 比对硬标签 EPC:
 *      一致亮绿灯升起开锁 / 不一致红灯闪烁不升起, 比对结果回传上位机;
 *      硬标签预留 2min 开锁时间, 每多一个标签 +30s;
 *      读到不同 EPC 且已解锁数 n 达到硬标签数 m 时, 进入软标阶段;
 *      软标阶段每个软标消耗一次解码数量, 次数用尽则结账完成。
 *  - 编排已实现的模块: App_UHF(硬标签EPC读) + App_Stepper(升降开锁)
 *                      + App_AM(软标解码器) + LED (App_Led_HL 单绿灯)
 *  - 状态机由 App_Locker_Process 主循环节拍驱动, 事件经 FC_LOCKER_CTRL
 *    注入/查询, 上报走 App_Locker_Report() (经协议层回上位机)。
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

/* ---- 状态机状态 ---- */
typedef enum {
    LOCKER_IDLE        = 0,   /* 空闲: 无结账任务, 默认锁定 */
    LOCKER_CONFIGURED  = 1,   /* 已配置清单, 等待客户放置硬标签 */
    LOCKER_UNLOCK_HOLD = 2,   /* 硬标签匹配, 磁块升起保持(2min+30s*n) */
    LOCKER_SOFT_DECODE = 3,   /* 硬标签全部解锁, 进入软标解码阶段 */
    LOCKER_DONE        = 4,   /* 结账完成 */
    LOCKER_FAULT       = 5    /* 锁定故障 (电机/链路), 需清障 */
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
    uint32_t holdDeadlineMs;  /* 当前开锁保持截止时间戳 */
    uint16_t holdTagIndex;    /* 当前处于保持的硬标签索引 */
    uint8_t  lockRisen;       /* 磁块是否处于升起 */
} AppLockerCtx_t;

/* ---- 配置 (可调参数) ---- */
#define APP_LOCKER_BASE_HOLD_MS      (2u * 60u * 1000u)  /* 2min 基础开锁时间 */
#define APP_LOCKER_EXTRA_PER_TAG_MS  (30u * 1000u)       /* 每多一标签 +30s */
#define APP_LOCKER_DONE_IDLE_MS      5000u               /* 结账完成后停留再回 IDLE */
#define APP_LOCKER_STEP_RISE         4800u               /* 升起微步数(可调) */
#define APP_LOCKER_STEP_LOWER        4800u               /* 下降微步数(可调) */

/* ---- 初始化 / 周期处理 ---- */
void App_Locker_Init(void);
void App_Locker_Process(void);   /* 主循环节拍: 推进状态机 */

/* ---- 控制 API (由协议层调用) ---- */
int  App_Locker_Configure(const AppLockerItem_t *items, uint16_t hardCount,
                          uint16_t softCount);   /* 全量重建结账任务 */
int  App_Locker_AddTag(const AppLockerItem_t *item);          /* 追加硬标签 */
int  App_Locker_Start(void);       /* 激活任务: 上电UHF+广播, 进入 CONFIGURED */
int  App_Locker_Cancel(void);      /* 取消当前任务, 磁块回降, 回 IDLE */
int  App_Locker_StopDecode(void);  /* 手动消耗一次软标解码 (v1 软标驱动) */

/* ---- 事件接入 (供唤醒/外部触发, 初版可空) ---- */
void App_Locker_NotifyTag(void);   /* UHF 读到标签放置 */

/* ---- 状态 / 信息 (供协议查询) ---- */
AppLockerState_t App_Locker_GetState(void);
void App_Locker_GetCtx(AppLockerCtx_t *ctx);
int  App_Locker_IsIdle(void);

/* ---- 上报事件拉取 (协议层取走回上位机) ---- */
int App_Locker_PopEvent(uint8_t *code, uint8_t *epc, uint8_t *epcLen,
                        uint16_t *hardMatched, uint16_t *softUsed,
                        uint16_t *softCount, uint16_t *hardCount);

#endif /* __APP_LOCKER_H */
