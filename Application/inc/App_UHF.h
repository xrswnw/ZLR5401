#ifndef __APP_UHF_H
#define __APP_UHF_H

#include <stdint.h>

/* =====================================================================
 * UHF 超高频模块 控制面 (状态机 + 配置) 与 数据面 (标签流)
 *  控制面: 命令/状态机/参数 (功率/天线/校验), 供协议层同步调用
 *  数据面: 盘点/读写的标签结果流, 独立缓冲, 供数据上报任务取走
 *  控制面与数据面解耦: 状态机只管命令执行与状态迁移,
 *  不直接参与标签数据搬运; 硬件走 App_UHF_HL (USART1 + 0xBB 帧)
 * ===================================================================== */

/* ---- 状态机状态 ---- */
typedef enum {
    APP_UHF_IDLE      = 0,   /* 空闲 */
    APP_UHF_READY     = 1,   /* 已初始化/配置已下发, 就绪 */
    APP_UHF_INVENTORY = 2,   /* 盘点中 */
    APP_UHF_READ      = 3,   /* 读操作中 */
    APP_UHF_WRITE     = 4,   /* 写操作中 */
    APP_UHF_ERROR     = 5,   /* 错误/掉线 */
    APP_UHF_ANT_CHECK = 6,   /* 回波检测中 (Open 内瞬态) */
    APP_UHF_GETBUF    = 7,   /* 多标签盘点后取缓冲中 (0x22 盘存 -> 0x29 取回) */
    APP_UHF_SCAN      = 8    /* 自动扫描运行中 (持续 0x22->0x29 连续盘点) */
} AppUHFState_t;
/* ---- 配置文件 (持久化到 userParam, 并经协议 get/set) ---- */
typedef struct {
    uint8_t  powerDbm;       /* 发射功率 5~30 dBm */
    uint8_t  antenna;        /* 天线 0=ANT1 / 1=ANT2 */
    uint8_t  checksumEn;     /* 1=启用电平校验/CRC 校验 (对模块) */
    uint8_t  session;        /* Gen2 session 0~3 */
    uint8_t  target;         /* Gen2 target 0=A / 1=B */
    uint8_t  q;              /* Gen2 Q 值 0~15, 0=模块默认(动态Q) */
    uint8_t  band;           /* 工作频段 (Region 码: 0x01=北美,0x06=中国1,0x08=CE_LOW,0xFF=全频段) */
    uint8_t  reserved;
} AppUHFConfig_t;

/* 默认配置 */
#define APP_UHF_CONFIG_DEFAULT { 20u, 0u, 1u, 0u, 0u, 0u, 0x01u, 0u }

/* ---- 单条标签记录 (数据面) ---- */
typedef struct {
    uint32_t rssi;             /* 信号强度 (0~255) */
    uint8_t  epc[16];          /* EPC (16B) */
    uint8_t  epcLen;           /* EPC 有效长度 */
} AppUHFTag_t;

/* 真实 EPC 最小字节数. 低于此长度的 0x21/0x28 响应视为边框/损坏伪标签,
 * 不入缓冲 (模块有时对边际读错误返回 status=0 但 EPC 字段被截断). */
#define UHF_EPC_MIN_LEN  6u

/* ---- 标签缓冲容量 (数据面环形缓冲) ---- */
#define APP_UHF_TAG_BUF_SIZE  16u

/* ---- 状态/错误监控 (数据面, 供 GET_STATUS) ---- */
typedef struct {
    int      lastErr;         /* 最近一次操作错误 (APP_UHF_ERR_*) */
    uint16_t antRl;           /* 上次回波 RL 反射损耗 (0.1dB), 0=未检测 */
    uint16_t antVswr;         /* 上次回波 VSWR 电压驻波比 (×100), 0=未检测 */
    uint8_t  antennaOk;       /* 1=天线连接正常 (RL>=0.5dB 且 VSWR<=7.00) */
    uint8_t  powered;         /* 1=已上电 */
} AppUHFStatus_t;

/* 帧格式版本 (userParam 内配置结构识别) */
#define APP_UHF_CFG_VERSION   1u

/* ---- 初始化 / 周期处理 ---- */
void     App_UHF_Init(void);              /* HL 初始化 + 状态机置 IDLE */
void     App_UHF_Process(void);           /* 主循环节拍: 推进状态机 + 标签采集 */

/* ---- 控制面 API (由协议层调用) ---- */
int      App_UHF_Open(void);              /* 上电 + 等待就绪 (同步, 含回波检测) */
int      App_UHF_Close(void);             /* 停止 + 下电 */
int      App_UHF_Inventory(void);         /* 发起一次盘点 (默认 1s 超时, 0x22 多标签) */
int      App_UHF_InventoryTimeout(uint16_t timeoutMs); /* 发起一次盘点, 指定超时 (0x22 多标签) */
int      App_UHF_InventorySync(uint16_t timeoutMs);    /* 同步阻塞盘点: 完成 0x22->0x29 并返标签数 */

/* ---- 自动扫描 (连续盘点, 标签入缓冲不主动上报; 主机用 GET_TAGS 拉取) ---- */
int      App_UHF_ScanStart(uint16_t cycleMs);  /* 启动连续扫描: 每轮 0x22->0x29 入缓冲, 回 READY 立起下一轮 */
int      App_UHF_ScanStop(void);               /* 停止自动扫描并停止当前盘点 */
int      App_UHF_IsScanning(void);             /* 1=扫描中; 供协议层拒绝冲突命令 */
int      App_UHF_ReadTag(const uint8_t *epc, uint8_t epcLen, uint8_t bank,
                         uint8_t addr, uint8_t cnt);
int      App_UHF_WriteTag(const uint8_t *epc, uint8_t epcLen, uint8_t bank,
                          uint8_t addr, const uint8_t *data, uint8_t len);
int      App_UHF_Stop(void);              /* 停止当前操作 */
int      App_UHF_Query(void);             /* 查询模块/链路状态, 更新状态机 */

/* ---- 状态 / 错误监控 ---- */
int      App_UHF_GetStatus(AppUHFStatus_t *st);   /* 0=OK, 拷贝当前状态 */
int      App_UHF_CheckAntenna(void);              /* 主动同步触发 0xAA4A 回波检测 */

/* ---- 配置 get / set (功率/天线/校验等) ---- */
int      App_UHF_GetConfig(AppUHFConfig_t *cfg);   /* 0=OK */
int      App_UHF_SetConfig(const AppUHFConfig_t *cfg, int save); /* 下发 + 可选持久化 */

/* ---- 数据面 (标签/结果流读取) ---- */
uint16_t App_UHF_TagCount(void);          /* 缓冲内标签数 */
int      App_UHF_TagTake(AppUHFTag_t *t); /* 取出一条, 0=OK, 负=空 */
void     App_UHF_ClearTags(void);         /* 清空标签缓冲 (新任务边界防陈旧标签) */

/* ---- 状态 / 信息 ---- */
AppUHFState_t App_UHF_GetState(void);
int      App_UHF_GetLinkStatus(void);     /* 0=正常, 非0=掉线 */
uint32_t App_UHF_GetTotalTags(void);      /* 累计盘点标签计数 */
int      App_UHF_IsBusy(void);            /* 是否有活动操作 */

/* ---- 真机盘点诊断 (Round_050/051 临时: 定位 0x22 TagsFound=0) ----
 * 记录最近一次 0x22/0x29 请求与响应的原始字节, 供上位机取证. */
typedef struct {
    uint8_t  cnt;            /* 已记录请求次数 */
    uint8_t  reqCmd;         /* 上次请求指令 (0x22/0x29) */
    uint8_t  reqData[5];     /* 上次请求 Data (含 Timeout) */
    uint8_t  reqLen;
    uint16_t rawDataLen;     /* 模块原始帧的 Data Length 字段 (tmp[1]) */
    uint8_t  rspCmd;         /* 模块响应指令 */
    uint16_t rspStatus;      /* 上次响应 status */
    uint8_t  rspData[40];    /* 上次响应 Data 原始字节 */
    uint16_t rspLen;
} AppUHFDump_t;
void App_UHF_GetDump(AppUHFDump_t *d);    /* 拷贝诊断记录 */

/* 错误码 */
#define APP_UHF_ERR_OK          0
#define APP_UHF_ERR_BUSY        (-1)
#define APP_UHF_ERR_PARAM       (-2)
#define APP_UHF_ERR_NOT_READY   (-3)
#define APP_UHF_ERR_LINK        (-4)
#define APP_UHF_ERR_NO_TAG      (-5)
#define APP_UHF_ERR_TIMEOUT     (-6)

#endif /* __APP_UHF_H */
