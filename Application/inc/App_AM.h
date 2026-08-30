#ifndef __APP_AM_H
#define __APP_AM_H

#include <stdint.h>

/* =====================================================================
 * AM 解码器 控制/配置层
 *  控制面: 配置 (工作模式/阀值/频率范围等) get/set, 逐命令写读, 供协议层同步调用
 *  硬件层: App_AM_HL (USART2 + 2A A2 帧收发)
 *  说明: 所有命令读写同一命令字; "写" 携带参数并等待回显,
 *        "读" 发空数据等待解码器回帧返回当前值。
 * ===================================================================== */

/* ---- 配置 (对应协议参数命令表) ---- */
typedef struct {
    uint16_t threshold;    /* 接收阀值 0-30 */
    uint16_t hitCount;     /* 命中次数 3-8 */
    uint8_t  freqRange;    /* 频率范围 0=宽 1=中 2=窄 */
    uint16_t recvDelay;    /* 接收延迟 0-50 (掩藏参数) */
    uint8_t  recvLength;   /* 接收长短 0=长 1=短 */
    uint8_t  phaseInvert;  /* 零火翻转 0=否 1=是 */
    uint16_t phaseSync;    /* 相位同步 0-2000 */
    uint8_t  decodeVolt;   /* 解码电压 0=低 1=中 2=高 */
    uint8_t  mode;         /* 工作模式 0=检测消磁 1=仅检测 2=待机 */
    uint8_t  mainsFreq;    /* 市电 0=50Hz 1=60Hz */
} AppAMConfig_t;

#define APP_AM_CONFIG_DEFAULT { 5u, 6u, 1u, 0u, 0u, 0u, 0u, 1u, 0u, 0u }
#define APP_AM_CFG_VERSION   1u

/* 工作模式 (cmd 0x50) */
#define AM_MODE_DEACTIVATE   0u   /* 检测/消磁 */
#define AM_MODE_DETECT_ONLY  1u   /* 仅检测 */
#define AM_MODE_STANDBY      2u   /* 待机 */

/* 波形 (cmd 0x64): 400 点, 上位机分页 (48 点/页, 9 页, 末页 16 点) */
#define AM_WAVE_POINTS       400u
#define AM_WAVE_PAGE         48u

/* 消磁结果分类状态 (live 实测 2026-08-29: cmd17 帧内容恒 FF×5 00 00 无信息,
 * 唯一判据是突发模式 —— 成功=恰好 1 帧后静默(标签失活不再被检测),
 * 失败=持续连发 ≈800ms 直到标签离开。事件静默 >3s 时结算,
 * 帧数==1 -> 成功, >=2 -> 失败; 结算后保持至下一事件首帧) */
typedef enum {
    AM_DEACT_IDLE    = 0,   /* 空闲: 无消磁事件, 或新事件首帧已到未结算 */
    AM_DEACT_SUCCESS = 1,   /* 消磁成功: 本事件恰好 1 帧后静默 */
    AM_DEACT_FAILURE = 2    /* 消磁失败: 本事件 >=2 帧连发 (不可消磁标签在场重试) */
} AppAMDeactState_t;

/* ---- 初始化 / 周期处理 ---- */
void App_AM_Init(void);
void App_AM_Process(void);

/* ---- 参数读写 (同步, 单命令) ---- */
int  App_AM_SetParam(uint8_t cmd, uint16_t value);      /* 0=OK */
int  App_AM_GetParam(uint8_t cmd, uint16_t *value);      /* 0=OK */

/* ---- 配置 get / set ---- */
int  App_AM_GetConfig(AppAMConfig_t *cfg);              /* 0=OK */
int  App_AM_SetConfig(const AppAMConfig_t *cfg, int save); /* 下发全部 + 可选持久化 */

/* ---- 状态 / 链路 ---- */
int  App_AM_Query(void);        /* 发 0x63 总查询, 真读回全部参数 + 探链, 更新缓存 */
int  App_AM_GetLinkStatus(void);/* 0=正常, 非0=掉线 */

/* ---- 监控 (标签检测事件) ---- */
uint32_t App_AM_GetEventCount(void);   /* cmd17 上报帧累计 (兼容保留, 不参与状态判定) */
uint32_t App_AM_GetLastEventMs(void);  /* 最近一次 cmd17 相对上电 ms, 0=尚无 */

/* ---- 消磁结果 (cmd17 突发分类) ---- */
AppAMDeactState_t App_AM_GetDeactState(void); /* 最近结算结果 (0空闲/1成功/2失败) */
uint32_t          App_AM_GetDeactCount(void); /* 成功消磁事件累计 (单帧事件) */
uint32_t          App_AM_GetFailCount(void);  /* 消磁失败事件累计 (>=2 帧事件) */

/* ---- 波形 (cmd 0x64): 同步采集一次完整波形 + 分页取回 ---- */
int      App_AM_CaptureWave(void);      /* 发 0x64, 阻塞收 4 包到缓存, 0=OK, 负=链路失败 */
int      App_AM_GetWavePage(uint16_t page, uint8_t *out, uint16_t *outLen); /* 0=OK; outLen=该页点数 */
uint16_t App_AM_GetWavePoints(void);    /* 当前缓存有效点数, 0=未采集/无 */

/* 错误码 */
#define APP_AM_ERR_OK          0
#define APP_AM_ERR_PARAM       (-2)
#define APP_AM_ERR_LINK        (-4)
#define APP_AM_ERR_TIMEOUT     (-6)

#endif /* __APP_AM_H */
