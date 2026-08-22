#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * AM 解码器 控制/配置层
 *  经 RS485/USART1 (App_AM_HL) 与解码器通信。
 *  本层维护一份运行配置, 供协议层 get/set; set 逐条将参数下发并以
 *  解码器回显验证, 同步返回。不下发任何定时/心跳逻辑 (心跳由解码器
 *  30 分钟自理, 或后续按需扩展)。
 * ===================================================================== */

#define AM_CMD_TIMEOUT_MS  500u

static AppAMConfig_t s_cfg;
static int           s_link;   /* 0=正常 */

void App_AM_Init(void)
{
    AM_HL_Init();
    s_cfg = (AppAMConfig_t)APP_AM_CONFIG_DEFAULT;
    s_link = 0;
}

void App_AM_Process(void)
{
    /* 预留: 周期心跳/链路监测所需扩展点 */
}

/* ---------- 参数写 (携带参数, 等待解码器回显) ---------- */
int App_AM_SetParam(uint8_t cmd, uint16_t value)
{
    uint8_t data[2];
    data[0] = (uint8_t)((value >> 8) & 0xFF);   /* Data0 = 高位 */
    data[1] = (uint8_t)(value & 0xFF);          /* Data1 = 低位 */

    AM_HL_RxFlush();
    AM_HL_SendFrame(cmd, data, 2u);

    uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    int r = AM_HL_RecvFrame(&rcmd, rdata, &rlen, AM_CMD_TIMEOUT_MS);
    if (r != 0) { s_link = AM_HL_LINK_TIMEOUT; return APP_AM_ERR_LINK; }
    s_link = 0;
    if (rcmd != cmd) return APP_AM_ERR_LINK;   /* 回显命令不一致视为链路异常 */
    return APP_AM_ERR_OK;
}

/* ---------- 参数读 (发空数据, 等解码器返回当前值) ---------- */
int App_AM_GetParam(uint8_t cmd, uint16_t *value)
{
    AM_HL_RxFlush();
    AM_HL_SendFrame(cmd, (const uint8_t *)0, 0u);

    uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    int r = AM_HL_RecvFrame(&rcmd, rdata, &rlen, AM_CMD_TIMEOUT_MS);
    if (r != 0) { s_link = AM_HL_LINK_TIMEOUT; return APP_AM_ERR_LINK; }
    s_link = 0;
    if (rcmd != cmd) return APP_AM_ERR_LINK;

    if (value) {
        if (rlen >= 2u) *value = (uint16_t)(((uint16_t)rdata[0] << 8) | rdata[1]);
        else if (rlen >= 1u) *value = rdata[0];
        else *value = 0u;
    }
    return APP_AM_ERR_OK;
}

int App_AM_GetConfig(AppAMConfig_t *cfg)
{
    if (!cfg) return APP_AM_ERR_PARAM;
    *cfg = s_cfg;
    return APP_AM_ERR_OK;
}

int App_AM_SetConfig(const AppAMConfig_t *cfg, int save)
{
    (void)save;   /* 持久化由上层 (协议层) 决定 */
    if (!cfg) return APP_AM_ERR_PARAM;

    /* 逐条下发并验证 */
    if (App_AM_SetParam(AM_CMD_THRESHOLD,  cfg->threshold)  != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_HIT_COUNT,  cfg->hitCount)   != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_FREQ_RANGE, cfg->freqRange)  != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_RECV_DELAY, cfg->recvDelay)  != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_RECV_LENGTH,cfg->recvLength) != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_PHASE_INVERT,cfg->phaseInvert)!= APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_PHASE_SYNC, cfg->phaseSync)  != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_DECODE_VOLT,cfg->decodeVolt) != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_MODE,       cfg->mode)       != APP_AM_ERR_OK) goto fail;

    s_cfg = *cfg;
    return APP_AM_ERR_OK;

fail:
    return APP_AM_ERR_LINK;
}

int App_AM_Query(void)
{
    uint16_t v;
    int r = App_AM_GetParam(AM_CMD_QUERY_ALL, &v);
    return (r == APP_AM_ERR_OK) ? APP_AM_ERR_OK : r;
}

int App_AM_GetLinkStatus(void) { return s_link; }
