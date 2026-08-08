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
    uint8_t  mode;         /* 工作模式 0=检测解码 1=检测 2=待机 */
} AppAMConfig_t;

#define APP_AM_CONFIG_DEFAULT { 5u, 6u, 1u, 0u, 0u, 0u, 0u, 1u, 0u }
#define APP_AM_CFG_VERSION   1u

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
int  App_AM_Query(void);        /* 发送总查询, 探测链路, 更新状态 */
int  App_AM_GetLinkStatus(void);/* 0=正常, 非0=掉线 */

/* 错误码 */
#define APP_AM_ERR_OK          0
#define APP_AM_ERR_PARAM       (-2)
#define APP_AM_ERR_LINK        (-4)
#define APP_AM_ERR_TIMEOUT     (-6)

#endif /* __APP_AM_H */
