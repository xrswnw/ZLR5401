#ifndef __APP_AM_BACKEND_H
#define __APP_AM_BACKEND_H

#include <stdint.h>
#include <stddef.h>   /* NULL (ops 槽位判空) */
#include "App_AM.h"

/* =====================================================================
 * 解码器后端接口 (Round_021 参数双模)
 *  App_AM (分发层, App_AM.c) 按 decoderType 单选一个后端, 二者互斥:
 *    - AM 后端 = 原 App_AM.c 实现, 原样平移 (App_AM_Backend.c)
 *    - RF 后端 = 新写 (App_AM_RF.c, B5 A1 / 0x3A / 0x01 / 0x06)
 *  后端只依赖本接口 + AM_HL (共用 USART1 传输, 帧头由后端注入)。
 *
 *  接口分三组:
 *   - 逻辑控制: SetMode (值域 0-2 两族一致, 后端转原生命令字
 *     AM 0x50 / RF 0x06) — 0x11 流程与 AM_CTRL SET_MODE 共用,
 *     这使流程层零语义改动;
 *   - 透传参数: SetParam/GetParam — 命令字为当前后端原生命令表,
 *     分发层不做值域翻译 (AM 0x11~0x67 / RF 0x05~0x20 各查各表);
 *   - 计数/链路: EventCount (0x11 逐帧入账源) / BurstActive (模式
 *     看门狗静默门控) / LinkStatus / 结算计数 — 语义与 AM 现行一致。
 *
 *  配置 (Get/SetConfig) 与波形为 AM 专属能力, RF 后端置 NULL,
 *  分发层在 RF 模式下对相应子命令回 PARAM (协议文档双表并列)。
 * ===================================================================== */

/* RF 帧头 (AM 帧头沿用 App_AM_HL.h 的 AM_HL_FRAME_HDR1/2) */
#define RF_HL_FRAME_HDR1      0xB5u
#define RF_HL_FRAME_HDR2      0xA1u

typedef struct {
    void     (*Init)(void);                        /* 状态机复位 + 帧头注入 HL */
    void     (*Process)(void);                     /* 主循环泵: 被动帧/突发结算/探链 */
    int      (*SetMode)(uint8_t mode);             /* 逻辑模式 0-2 写 + 回显确认 */
    int      (*SetParam)(uint8_t cmd, uint16_t value);  /* 原生命令字透传写 */
    int      (*GetParam)(uint8_t cmd, uint16_t *value);  /* 原生命令字透传读 */
    int      (*Query)(void);                       /* 探链 + 真读回 */
    int      (*GetLinkStatus)(void);
    uint32_t (*GetEventCount)(void);               /* 解码上报帧累计 (cmd17 / 0x3A) */
    uint32_t (*GetLastEventMs)(void);
    AppAMDeactState_t (*GetDeactState)(void);
    uint32_t (*GetDeactCount)(void);
    uint32_t (*GetFailCount)(void);
    uint8_t  (*BurstActive)(void);                 /* 突发进行中 (看门狗门控) */
    /* AM 专属 (RF 后端置 NULL) */
    int      (*GetConfig)(AppAMConfig_t *cfg);
    int      (*SetConfig)(const AppAMConfig_t *cfg, int save);
    int      (*CaptureWave)(void);
    int      (*GetWavePage)(uint16_t page, uint8_t *out, uint16_t *outLen);
    uint16_t (*GetWavePoints)(void);
} DecoderBkOps_t;

extern const DecoderBkOps_t g_amBkOps;   /* AM 消磁解码器 (2A A2) */
extern const DecoderBkOps_t g_rfBkOps;   /* RF 解码器 (B5 A1) */

#endif /* __APP_AM_BACKEND_H */
