#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * AM 解码器 控制/配置层 (AMS 消磁器)
 *  经 RS232/USART1 (App_AM_HL) 与消磁器通信。
 *
 * 通信事实 (live 实测):
 *  - 参数命令为"回显式"写, 无真读回; 发空数据 = 写 0(危险)。
 *    但存在真读回通道 = 总查询 0x63: 发 2A A2 63 01 01 02 00 00 回 12 帧,
 *    写→总查闭环已实锤 0x63 读到的是真实存储值。
 *  - 设备自身 ID 可读写: 写ID 0x5F(5字节: 年月日+序列), 读ID 0x5E; 实测可写可持久化。
 *  - 标签 ID 仅读 0x5E(默认 FF×5), 该消磁器不解码唯一标签 ID。
 *  - cmd17 为"消磁结果"主动上报: 连发 FF×5 = 消磁失败重试; 单帧 = 消磁成功。
 *  - 0x88 解码指令: 实测无作用(即使正确校验也无效), 已移除。
 * ===================================================================== */

#define AM_CMD_TIMEOUT_MS  500u
/* 无链路空闲衰减: AM 空闲时静默 (仅消磁事件主动发帧), 与 UHF 对齐,
 * s_link 仅反映最近一次总线事务结果 (查询/参数写/波形采集). */

/* cmd17 消磁结果判定 (live 实测 2026-08-29, 直连串口换标签验证):
 *  - 帧内容恒 FF FF FF FF FF 00 00 (datalen=7), 成功帧与失败帧内容完全相同,
 *    内容无信息, 唯一判据是突发模式。
 *  - 消磁成功: 恰好 1 帧后永久静默 (标签失活, 设备不再检测到)。
 *  - 消磁失败: 持续连发 (实测 ≈800ms 周期, 帧距 0.8~1.4s) 直到标签离开。
 *  突发判据: 相邻帧间隔 > AM_BURST_GAP_MS 视为不同消磁事件;
 *  事件结束 (静默超 GAP) 时结算: 帧数==1 -> 成功, >=2 -> 失败。
 *  结算后状态保持 (供 GET_STATUS 轮询), 下一事件首帧时回到 IDLE。 */
#define AM_BURST_GAP_MS    3000u

static AppAMConfig_t s_cfg;
static int           s_link;          /* 0=正常 */
static uint32_t      s_evtCount;      /* cmd17 上报帧累计 (兼容保留, 不参与状态判定) */
static uint32_t      s_lastEvtMs;     /* 最近一次 cmd17 相对上电 ms */

/* cmd17 消磁事件状态机 */
static uint16_t            s_burstFrames;   /* 当前消磁事件已收 cmd17 帧数, 0=无进行中事件 */
static uint32_t            s_lastCmd17Ms;  /* 最近一次 cmd17 时刻 */
static AppAMDeactState_t   s_deact;        /* 当前/最近结算结果 (0空闲/1成功/2失败) */
static uint32_t            s_deactCount;   /* 成功消磁累计 (单帧事件结算) */
static uint32_t            s_failCount;    /* 消磁失败累计 (>=2 帧事件结算) */

/* 波形缓存 (cmd 0x64 采集结果, 供上位机分页取回) */
static uint8_t       s_wave[AM_WAVE_POINTS];
static uint16_t      s_waveCount;     /* 已收集点数 (可能 <400, 实时背靠背丢包已知) */

/* 按命令字把写入值同步到本地缓存 */
static void am_cache_by_cmd(uint8_t cmd, uint16_t val)
{
    uint8_t b = (uint8_t)(val & 0xFF);
    switch (cmd) {
    case AM_CMD_THRESHOLD:   s_cfg.threshold   = val; break;
    case AM_CMD_HIT_COUNT:   s_cfg.hitCount    = val; break;
    case AM_CMD_FREQ_RANGE:  s_cfg.freqRange   = b;   break;
    case AM_CMD_RECV_DELAY:  s_cfg.recvDelay   = val; break;
    case AM_CMD_RECV_LENGTH: s_cfg.recvLength  = b;   break;
    case AM_CMD_PHASE_INVERT:s_cfg.phaseInvert = b;   break;
    case AM_CMD_PHASE_SYNC:  s_cfg.phaseSync   = val; break;
    case AM_CMD_DECODE_VOLT: s_cfg.decodeVolt  = b;   break;
    case AM_CMD_MODE:        s_cfg.mode        = b;   break;
    case AM_CMD_MAINS_FREQ:  s_cfg.mainsFreq   = b;   break;
    default: break;
    }
}

void App_AM_Init(void)
{
    AM_HL_Init();
    s_cfg = (AppAMConfig_t)APP_AM_CONFIG_DEFAULT;
    s_link = 0;
    s_evtCount = 0;
    s_lastEvtMs = 0;

    s_burstFrames  = 0;
    s_lastCmd17Ms  = 0;
    s_deact        = AM_DEACT_IDLE;
    s_deactCount   = 0;
    s_failCount    = 0;
}

void App_AM_Process(void)
{
    /* 轮询被动上报帧 (cmd17 等): 取到任何合法帧都刷新链路活动时间戳 */
    uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    uint32_t now;
    while (AM_HL_TryRecvFrame(&rcmd, rdata, &rlen) == 0) {
        now = SysTickHl_GetMs();
        if (rcmd == AM_CMD_ATAG_REPORT) {
            s_evtCount++;
            s_lastEvtMs = now;

            /* 帧距 > GAP: 上一事件已结算 (静默分支), 本帧开新事件 */
            if (s_burstFrames == 0u || (now - s_lastCmd17Ms) > AM_BURST_GAP_MS) {
                s_burstFrames = 1u;
                s_deact      = AM_DEACT_IDLE;
            } else {
                s_burstFrames++;
            }
            s_lastCmd17Ms = now;
        }
    }

    /* 消磁事件结束结算: 静默超 GAP 时按本事件帧数定成功/失败 */
    if (s_burstFrames != 0u && (SysTickHl_GetMs() - s_lastCmd17Ms) > AM_BURST_GAP_MS) {
        if (s_burstFrames == 1u) { s_deact = AM_DEACT_SUCCESS; s_deactCount++; }
        else                     { s_deact = AM_DEACT_FAILURE; s_failCount++;  }
        s_burstFrames = 0u;
    }
}

/* ---------- 参数写 (携带参数, 等待解码器回显) ---------- */
int App_AM_SetParam(uint8_t cmd, uint16_t value)
{
    uint8_t data[2];
    data[0] = (uint8_t)((value >> 8) & 0xFF);   /* Data0 = 高位 */
    data[1] = (uint8_t)(value & 0xFF);          /* Data1 = 低位 */

    /* 清掉可能积压的被动帧, 避免误当作回显 */
    AM_HL_RxFlush();
    AM_HL_SendFrame(cmd, data, 2u);

    uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    int r = AM_HL_RecvFrame(&rcmd, rdata, &rlen, AM_CMD_TIMEOUT_MS);
    if (r != 0) { s_link = AM_HL_LINK_TIMEOUT; return APP_AM_ERR_LINK; }
    s_link = 0;
    if (rcmd != cmd) return APP_AM_ERR_LINK;    /* 回显命令不一致视为链路异常 */

    /* 回显校验成功 -> 同步本地缓存 (缓存即真相, 供读) */
    am_cache_by_cmd(cmd, value);
    return APP_AM_ERR_OK;
}

/* ---------- 0x63 总查询: 真读回全部参数到 s_cfg + 判链路 ---------- */
static int am_query_all(void)
{
    uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    int bad = 0;
    uint32_t t0 = SysTickHl_GetMs();

    AM_HL_RxFlush();
    /* 解码器只认 datalen=2+data 00 00 的完整帧 (live 实测格式), len=0 会被忽略 */
    static const uint8_t kQueryAllData[2] = {0u, 0u};
    AM_HL_SendFrame(AM_CMD_QUERY_ALL, kQueryAllData, 2u);

    /* 12 帧背靠背, 需足够时间/轮询取全; 每收到一帧即写入对应缓存 */
    uint16_t seen = 0u;
    while ((SysTickHl_GetMs() - t0) < 1200u) {
        int r = AM_HL_RecvFrame(&rcmd, rdata, &rlen, 200u);
        if (r != 0) break;
        switch (rcmd) {
        case AM_CMD_THRESHOLD:   s_cfg.threshold   = (uint16_t)(((uint16_t)rdata[0] << 8) | rdata[1]); break;
        case AM_CMD_HIT_COUNT:   s_cfg.hitCount    = (uint16_t)(((uint16_t)rdata[0] << 8) | rdata[1]); break;
        case AM_CMD_FREQ_RANGE:  s_cfg.freqRange   = rdata[1]; break;
        case AM_CMD_RECV_DELAY:  s_cfg.recvDelay   = (uint16_t)(((uint16_t)rdata[0] << 8) | rdata[1]); break;
        case AM_CMD_RECV_LENGTH: s_cfg.recvLength  = rdata[1]; break;
        case AM_CMD_PHASE_INVERT:s_cfg.phaseInvert = rdata[1]; break;
        case AM_CMD_PHASE_SYNC:  s_cfg.phaseSync   = (uint16_t)(((uint16_t)rdata[0] << 8) | rdata[1]); break;
        case AM_CMD_DECODE_VOLT: s_cfg.decodeVolt  = rdata[1]; break;
        case AM_CMD_MODE:        s_cfg.mode        = rdata[1]; break;
        case AM_CMD_MAINS_FREQ:  s_cfg.mainsFreq   = rdata[1]; break;
        default: break;
        }
        seen++;
    }
    if (seen < 3u) { s_link = AM_HL_LINK_TIMEOUT; bad = -1; }   /* 帧太少判链路断 */
    else s_link = 0;
    return bad;
}

/* ---------- 参数读: 经 0x63 真读回 (非本地缓存/非逐参回显) ---------- */
int App_AM_GetParam(uint8_t cmd, uint16_t *value)
{
    if (cmd == AM_CMD_QUERY_ALL) return APP_AM_ERR_PARAM;   /* 0x63 用 Query/GetConfig */
    int e = am_query_all();
    if (e != 0) return APP_AM_ERR_LINK;

    uint16_t v = 0u;
    switch (cmd) {
    case AM_CMD_THRESHOLD:   v = s_cfg.threshold;   break;
    case AM_CMD_HIT_COUNT:   v = s_cfg.hitCount;    break;
    case AM_CMD_FREQ_RANGE:  v = s_cfg.freqRange;   break;
    case AM_CMD_RECV_DELAY:  v = s_cfg.recvDelay;   break;
    case AM_CMD_RECV_LENGTH: v = s_cfg.recvLength;  break;
    case AM_CMD_PHASE_INVERT:v = s_cfg.phaseInvert; break;
    case AM_CMD_PHASE_SYNC:  v = s_cfg.phaseSync;   break;
    case AM_CMD_DECODE_VOLT: v = s_cfg.decodeVolt;  break;
    case AM_CMD_MODE:        v = s_cfg.mode;        break;
    case AM_CMD_MAINS_FREQ:  v = s_cfg.mainsFreq;   break;
    default: return APP_AM_ERR_PARAM;
    }
    if (value) *value = v;
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

    /* 逐条下发并验证回显 (含市电) */
    if (App_AM_SetParam(AM_CMD_THRESHOLD,   cfg->threshold)   != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_HIT_COUNT,   cfg->hitCount)    != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_FREQ_RANGE,  cfg->freqRange)   != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_RECV_DELAY,  cfg->recvDelay)   != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_RECV_LENGTH, cfg->recvLength)  != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_PHASE_INVERT,cfg->phaseInvert) != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_PHASE_SYNC,  cfg->phaseSync)   != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_DECODE_VOLT, cfg->decodeVolt)  != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_MAINS_FREQ,  cfg->mainsFreq)   != APP_AM_ERR_OK) goto fail;
    if (App_AM_SetParam(AM_CMD_MODE,        cfg->mode)        != APP_AM_ERR_OK) goto fail;

    /* 写后用 0x63 复核: 若设备真读回与所设不符 -> 算链路异常 */
    if (am_query_all() == 0) {
        if (s_cfg.threshold  != cfg->threshold ||
            s_cfg.hitCount   != cfg->hitCount ||
            s_cfg.freqRange  != cfg->freqRange ||
            s_cfg.recvDelay  != cfg->recvDelay ||
            s_cfg.recvLength != cfg->recvLength ||
            s_cfg.phaseInvert!= cfg->phaseInvert ||
            s_cfg.phaseSync  != cfg->phaseSync ||
            s_cfg.decodeVolt != cfg->decodeVolt ||
            s_cfg.mode       != cfg->mode ||
            s_cfg.mainsFreq  != cfg->mainsFreq) return APP_AM_ERR_LINK;
    }

    s_cfg = *cfg;
    return APP_AM_ERR_OK;

fail:
    return APP_AM_ERR_LINK;
}

int App_AM_Query(void)
{
    /* 0x63 主动总查询: 真读回 + 探链 */
    return (am_query_all() == 0) ? APP_AM_ERR_OK : APP_AM_ERR_LINK;
}

int App_AM_GetLinkStatus(void) { return s_link; }

uint32_t App_AM_GetEventCount(void)   { return s_evtCount; }
uint32_t App_AM_GetLastEventMs(void)  { return s_lastEvtMs; }

/* ---- 消磁结果分类 (cmd17) ---- */
AppAMDeactState_t App_AM_GetDeactState(void) { return s_deact; }
uint32_t          App_AM_GetDeactCount(void) { return s_deactCount; }
uint32_t          App_AM_GetFailCount(void)  { return s_failCount; }

/* ---------- 波形 cmd 0x64: 同步采集 4 包 (背靠背, 镜像 am_query_all 时序) ---------- */
int App_AM_CaptureWave(void)
{
    uint8_t data[2] = { 0u, 0u };
    uint8_t pkt[AM_HL_WAVE_PAYLOAD];
    uint8_t idx;
    uint32_t t0 = SysTickHl_GetMs();
    int got = 0;

    AM_HL_RxFlush();
    AM_HL_SendFrame(AM_CMD_WAVE, data, 2u);   /* 2A A2 64 01 01 02 00 00 68 */

    while ((SysTickHl_GetMs() - t0) < 1500u) {
        int r = AM_HL_RecvWavePkt(pkt, &idx, 250u);
        if (r != 0) break;
        if (idx >= 4u) continue;               /* 非法包次丢弃 */
        for (uint16_t i = 0; i < AM_HL_WAVE_PAYLOAD; i++)
            s_wave[(uint16_t)idx * AM_HL_WAVE_PAYLOAD + i] = pkt[i];
        got++;
        if (got >= 4u) break;                  /* 四包集齐 */
    }

    s_waveCount = (uint16_t)(got * AM_HL_WAVE_PAYLOAD);
    if (got < 1u) { s_link = AM_HL_LINK_TIMEOUT; return APP_AM_ERR_LINK; }
    s_link = 0;
    return APP_AM_ERR_OK;
}

uint16_t App_AM_GetWavePoints(void) { return s_waveCount; }

int App_AM_GetWavePage(uint16_t page, uint8_t *out, uint16_t *outLen)
{
    if (!out || !outLen) return APP_AM_ERR_PARAM;
    if (s_waveCount == 0u) return APP_AM_ERR_PARAM;      /* 未采集 */

    uint16_t base = (uint16_t)(page * AM_WAVE_PAGE);
    if (base >= s_waveCount) return APP_AM_ERR_PARAM;    /* 越界页 */

    uint16_t n = s_waveCount - base;
    if (n > AM_WAVE_PAGE) n = AM_WAVE_PAGE;
    for (uint16_t i = 0; i < n; i++) out[i] = s_wave[base + i];
    *outLen = n;
    return APP_AM_ERR_OK;
}
