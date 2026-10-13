#include "App_AM.h"
#include "App_AM_Backend.h"
#include "App_AM_HL.h"
#include "App_CustomProtocol.h"   /* Memset8 */
#include "App_Param.h"

/* =====================================================================
 * 解码器分发层 (Round_021 参数双模) — App_AM 公共 API 的唯一属主
 *
 *  App_AM_Init/Process/... 全部按 s_decoderType 单选一个后端转发:
 *    0=AM 消磁解码器 (AMS, 帧 2A A2, g_amBkOps — 原 App_AM.c 平移)
 *    1=RF 解码器 (帧 B5 A1, g_rfBkOps — 新写)
 *  二者互斥不并存: 同一时刻只初始化/泵一个后端 (接错外设 = 探链
 *  失败如实上报, 不自适应 — 用户裁决 2026-10-13)。
 *
 *  物理层 (USART1 RS232 115200) 两族共用, 归本层持有 (AM_HL_Init);
 *  帧头差异由各后端 Init 时注入 AM_HL_SetHeader。
 *
 *  配置 (Get/SetConfig) 与波形为 AM 专属能力, RF 后端无此 ops,
 *  本层回 APP_AM_ERR_PARAM (协议层对相应子命令已按模式门禁)。
 * ===================================================================== */

static const DecoderBkOps_t *s_bk;     /* 当前后端 (按 decoderType 单选) */
static uint8_t s_decoderType;          /* APP_DECODER_AM / APP_DECODER_RF */

uint8_t App_AM_GetDecoderType(void) { return s_decoderType; }

/* 切换后端: 注入帧头 + 重建后端状态机 (计数清零 — 两族计数不可混账) */
static void bk_apply(uint8_t type)
{
    if (type == APP_DECODER_RF) s_bk = &g_rfBkOps;
    else                        s_bk = &g_amBkOps;
    s_decoderType = type;
    s_bk->Init();
}

void App_AM_Init(void)
{
    uint8_t type = APP_DECODER_AM;
    (void)DecParam_Load(&type);   /* 旧参数区无记录 -> AM, 现网零回归锚点 */
    AM_HL_Init();                 /* 物理层共用: USART1 RS232 115200 */
    bk_apply((type <= APP_DECODER_RF) ? type : APP_DECODER_AM);
}

/* ---- 类型切换 (空闲时调用, busy 门禁在协议层 Dispatch) ----
 * 幂等: 同类型直回。切换 = 重建物理层 (清半收帧) + 重建后端 + 持久化;
 * 切换后新链路状态由随后 Query 探测 (协议层 SET_DECODER 已联动探链)。 */
int App_AM_SetDecoderType(uint8_t type)
{
    if (type > APP_DECODER_RF) return APP_AM_ERR_PARAM;
    if (type == s_decoderType) return APP_AM_ERR_OK;

    AM_HL_DeInit();
    AM_HL_Init();
    bk_apply(type);
    (void)DecParam_Save(type);    /* 持久化 best-effort (与 UHF 配置同惯例) */
    return APP_AM_ERR_OK;
}

/* ---- 周期处理 / 逻辑控制 (转发当后端) ---- */
void App_AM_Process(void)     { s_bk->Process(); }

int App_AM_SetMode(uint8_t mode)          /* 逻辑模式 0-2 (后端转原生命令字) */
{
    return s_bk->SetMode(mode);
}

int App_AM_SetParam(uint8_t cmd, uint16_t value)      /* 原生命令字透传写 */
{
    return s_bk->SetParam(cmd, value);
}

int App_AM_GetParam(uint8_t cmd, uint16_t *value)     /* 原生命令字透传读 */
{
    return s_bk->GetParam(cmd, value);
}

int App_AM_Query(void)        { return s_bk->Query(); }
int App_AM_GetLinkStatus(void){ return s_bk->GetLinkStatus(); }

/* ---- 监控 / 计数 (0x11 流程逐帧入账源) ---- */
uint32_t App_AM_GetEventCount(void)   { return s_bk->GetEventCount(); }
uint32_t App_AM_GetLastEventMs(void)  { return s_bk->GetLastEventMs(); }

AppAMDeactState_t App_AM_GetDeactState(void) { return s_bk->GetDeactState(); }
uint32_t          App_AM_GetDeactCount(void) { return s_bk->GetDeactCount(); }
uint32_t          App_AM_GetFailCount(void)  { return s_bk->GetFailCount(); }
uint8_t           App_AM_BurstActive(void)   { return s_bk->BurstActive(); }

/* ---- 配置 / 波形 (AM 专属; RF 模式下无对应 ops -> PARAM) ---- */
int App_AM_GetConfig(AppAMConfig_t *cfg)
{
    if (s_bk->GetConfig == NULL) {
        if (cfg) Memset8(cfg, 0, sizeof(*cfg));   /* 供 0x0A 保留路径 amc.mode=0 判 */
        return APP_AM_ERR_PARAM;
    }
    return s_bk->GetConfig(cfg);
}

int App_AM_SetConfig(const AppAMConfig_t *cfg, int save)
{
    if (s_bk->SetConfig == NULL) return APP_AM_ERR_PARAM;
    return s_bk->SetConfig(cfg, save);
}

int App_AM_CaptureWave(void)
{
    if (s_bk->CaptureWave == NULL) return APP_AM_ERR_PARAM;
    return s_bk->CaptureWave();
}

int App_AM_GetWavePage(uint16_t page, uint8_t *out, uint16_t *outLen)
{
    if (s_bk->GetWavePage == NULL) return APP_AM_ERR_PARAM;
    return s_bk->GetWavePage(page, out, outLen);
}

uint16_t App_AM_GetWavePoints(void)
{
    return (s_bk->GetWavePoints != NULL) ? s_bk->GetWavePoints() : 0u;
}
