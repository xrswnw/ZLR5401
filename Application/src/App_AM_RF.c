#include "App_AM.h"
#include "App_AM_Backend.h"
#include "App_AM_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * RF 解码器 后端 (帧 B5 A1) — Round_021 新写
 * 协议: RF解码器通信协议20220109 V1.01 (XLS 原表 + 梳理存客户目录 RF/)。
 *
 * 帧体与 AM 同构 (cmd/包数/包次/包长/数据/累加校验 Sum_L), 仅帧头
 * B5 A1 (天线变体 B5 A5 不用), 波特率同 115200 — 收发框架整体共用
 * AM_HL, 本后端只做命令语义:
 *   - 读: 0x01 总查询单帧取全 [版本2][ID5][报警次数4][N参数按命令升序]
 *     (RF 无逐参读命令, GetParam 每次全量查询后按位抽取);
 *   - 写: 回显式 B5 A1 cmd 01 01 01 <值> Sum_L, 等同命令回显;
 *   - 模式: 0x06 (0=检测解码/1=只检测/2=休眠) <-> AM 0x50 值域一一
 *     对应, 0x11 流程受理/出口/模式看门狗经 App_AM_SetMode 原生复用;
 *   - 计数: 0x3A 报警上报 (数据=设备ID 年月日+序列, 不带标签身份)
 *     <-> AM cmd17 — 0x11 逐帧入账口径不变 (2026-10-13 裁决: 基数
 *     计数, 不识别标签, 误识别现象与 AM 同款客户已知);
 *   - 心跳: 0x04 (ID5+噪声1, 15min) 与 AM 0x04 同为静默丢弃
 *     (链路判定只认总线事务, 与 AM 口径一致);
 *   - 0x1A 测试报警 / 0x19 恢复出厂: 写表动作帧 (值 0), 透传放行 —
 *     0x1A 可无标签触发报警, 台架验收利器。
 *
 * 计数语义待样机实测确认项 (架构已隔离为后端常量, 不阻塞编译):
 *   #1 0x3A 突发节奏与去活化静默判据 -> RF_BURST_GAP_MS 缺省沿用
 *      AM 3s, 实测后单独重标定;
 *   #2 0x01 返回的累计报警次数口径 (一帧一计 or 一报警一计) —
 *      暂不参与结账, 仅作对账参考。
 * ===================================================================== */

#define RF_CMD_TIMEOUT_MS  500u
#define RF_PROBE_TIMEOUT_MS 200u   /* 探链预算 (0x01 单帧, 较 AM 12 帧取首帧 150ms 略宽) */

/* ---- RF 命令字 (操作指令表 0x05~0x20 写表 + 主动上报 + 查询) ---- */
#define RF_CMD_QUERY        0x01u   /* 总查询: 回 [ver(2)][ID(5)][alarmCnt(4)][N 参数] */
#define RF_CMD_HEARTBEAT    0x04u   /* 心跳上报 15min: [ID(5)][噪声(1)] */
#define RF_CMD_SENSITIVITY  0x05u   /* 灵敏度 0宽/1中/2严 */
#define RF_CMD_MODE         0x06u   /* 工作状态 0检测解码/1只检测/2休眠 (AM 0x50 同值域) */
#define RF_CMD_ALARM_COLOR  0x07u   /* 报警灯颜色 0蓝..6白 (RF 自带 LED) */
#define RF_CMD_GAIN1        0x08u   /* 增益1 0-31 */
#define RF_CMD_GAIN2        0x09u   /* 增益2 0-31 */
#define RF_CMD_ENERGY1      0x0Au   /* 能量1 0-31 (去活化强度) */
#define RF_CMD_ENERGY2      0x0Bu   /* 能量2 0-31 */
#define RF_CMD_TAG_TYPE     0x0Cu   /* 软硬标签 0软/1硬 */
#define RF_CMD_BANDWIDTH    0x0Du   /* 带宽 0宽/1中/2窄 */
#define RF_CMD_BG_NOISE     0x0Eu   /* 背景噪声 0失能/1使能 (原报警使能位挪用) */
#define RF_CMD_THRESHOLD1   0x0Fu   /* 阈值1 8-31 */
#define RF_CMD_THRESHOLD2   0x10u   /* 阈值2 0-31 */
#define RF_CMD_HIT_COUNT    0x11u   /* 命中次数 6-15 (AM 3-8, 值域不同不翻译) */
#define RF_CMD_FEATURE1     0x12u   /* 特征1 0-15 */
#define RF_CMD_FEATURE2     0x13u   /* 特征2 0-15 */
#define RF_CMD_LED_TIME     0x14u   /* 灯时间 0短/1中/2长 */
#define RF_CMD_LED_MODE     0x15u   /* 模式组合 0-4 */
#define RF_CMD_LED_ALWAYS   0x16u   /* 常亮组合 0蓝..5淡蓝/6灭 (建议 6=灭, 避让主控 RGB) */
#define RF_CMD_RECV_DELAY1  0x17u   /* 接收延时1 0-31 */
#define RF_CMD_RECV_DELAY2  0x18u   /* 接收延时2 0-31 */
#define RF_CMD_FACTORY_RST  0x19u   /* 恢复出厂 (动作帧, 无需保存) */
#define RF_CMD_TEST_ALARM   0x1Au   /* 测试报警 (动作帧, 无标签可触发, 验收利器) */
#define RF_CMD_ANTI_NOISE   0x1Bu   /* 抗噪级别 0-15 */
/* 0x1C~0x20 预留 (写表在册, 透传放行); 0x02 波形不接入; 0x03 ID 写不接入 */
#define RF_CMD_ALARM_REPORT 0x3Au   /* 报警上报: [年月日序列H序列L]=设备ID (计数帧, 类比 cmd17) */

/* 0x01 响应数据布局: [版本(2)][ID(5)][报警次数(4)][N 参数按命令字升序] */
#define RF_QUERY_HDR_LEN    11u
#define RF_PARAM_FIRST_CMD  0x05u
#define RF_PARAM_LAST_CMD   0x20u

/* 0x3A 报警突发结算窗 (ms): 相邻帧间隔超此值视为不同事件; 静默超窗
 * 结算一次成功解码 (供 GET_STATUS, 0x11 流程本身逐帧入账不依赖结算)。
 * AM 实测节奏 0.8~1.4s 帧距 / 3s 判静默; RF 节奏未知, 缺省沿用 3s,
 * 样机实测后单独重标定 (待确认项 #1, 后端常量已隔离)。 */
#define RF_BURST_GAP_MS     3000u

static int           s_link;        /* 0=正常 */
static uint32_t      s_evtCount;    /* 0x3A 帧累计 (0x11 逐帧入账源) */
static uint32_t      s_lastEvtMs;   /* 最近一次 0x3A 相对上电 ms */
static uint16_t      s_burstFrames; /* 当前报警事件已收 0x3A 帧数, 0=无进行中事件 */
static uint32_t      s_lastRptMs;   /* 最近一次 0x3A 时刻 */
static AppAMDeactState_t s_deact;   /* 最近结算结果 (0空闲/1成功) */
static uint32_t      s_deactCount;  /* 结算事件累计 */
static uint32_t      s_failCount;  /* 恒 0 保留 (与 AM 同, 判据不可达) */

/* 链路瞬断自动探测 (AM 同款退避结构) */
static uint32_t      s_probeNextMs;
static uint8_t       s_probeBackoff;

static void RfBk_Init(void)
{
    AM_HL_SetHeader(RF_HL_FRAME_HDR1, RF_HL_FRAME_HDR2);   /* 帧头注入 */
    s_link = 0;
    s_evtCount = 0;
    s_lastEvtMs = 0;
    s_burstFrames = 0;
    s_lastRptMs = 0;
    s_deact = AM_DEACT_IDLE;
    s_deactCount = 0;
    s_failCount = 0;
    s_probeNextMs = 0;
    s_probeBackoff = 0;
}

/* ---- 0x01 总查询 (读通道): 单帧请求-响应 ----
 * 请求 B5 A1 01 01 01 01 00 Sum_L (live 前按协议表, datalen=1/data=00);
 * 响应数据 [ver(2)][ID(5)][alarmCnt(4)][N 参数]。 */
static int rf_query_frame(uint8_t *rdata, uint8_t *rlen, uint32_t timeoutMs)
{
    AM_HL_RxFlush();
    static const uint8_t kQuery[1] = {0u};
    AM_HL_SendFrame(RF_CMD_QUERY, kQuery, 1u);

    uint8_t rcmd;
    int r = AM_HL_RecvFrame(&rcmd, rdata, rlen, timeoutMs);
    if (r != 0) { s_link = AM_HL_LINK_TIMEOUT; return APP_AM_ERR_LINK; }
    s_link = 0;
    if (rcmd != RF_CMD_QUERY) return APP_AM_ERR_LINK;   /* 回帧命令不一致 */
    return APP_AM_ERR_OK;
}

static int RfBk_Query(void)
{
    uint8_t rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    int e = rf_query_frame(rdata, &rlen, RF_CMD_TIMEOUT_MS);
    if (e != APP_AM_ERR_OK) return e;
    /* 形态校验: 帧头段 (版本+ID+报警次数) 须齐, 缺 -> 视为链路异常
     * (接错外设/异构设备时不自适应, 探链失败如实上报) */
    if (rlen < RF_QUERY_HDR_LEN) { s_link = AM_HL_LINK_TIMEOUT; return APP_AM_ERR_LINK; }
    return APP_AM_ERR_OK;
}

/* ---- 写 (回显式): B5 A1 cmd 01 01 01 <值> Sum_L, 等同命令回显 ----
 * 与 AM 不同处: 等回显期间忽略 0x3A/0x04 等主动上报帧 (只认命令
 * 匹配帧), 避免报警突发打劫回显判定 (AM 历史瑕疵不带入新后端)。 */
static int RfBk_SetParam(uint8_t cmd, uint16_t value)
{
    if (cmd < RF_PARAM_FIRST_CMD || cmd > RF_PARAM_LAST_CMD)
        return APP_AM_ERR_PARAM;          /* 0x01/0x04/0x3A 等非写表命令拒绝 */

    uint8_t data[1] = { (uint8_t)(value & 0xFF) };   /* RF 参数均单字节值 */
    AM_HL_RxFlush();
    AM_HL_SendFrame(cmd, data, 1u);

    uint32_t t0 = SysTickHl_GetMs();
    while ((SysTickHl_GetMs() - t0) < RF_CMD_TIMEOUT_MS) {
        uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
        uint8_t rlen;
        if (AM_HL_RecvFrame(&rcmd, rdata, &rlen, RF_CMD_TIMEOUT_MS) == 0) {
            if (rcmd == cmd) { s_link = 0; return APP_AM_ERR_OK; }
            /* 主动上报帧 (0x3A/0x04): 丢弃继续等回显 */
        }
    }
    s_link = AM_HL_LINK_TIMEOUT;
    return APP_AM_ERR_LINK;
}

/* ---- 逻辑模式写: RF 0x06 原生命令, 值域 0-2 与 AM 一致 ----
 * 0x11 流程受理 (切检测解码)/出口 (切只检测)/模式看门狗 (幂等重申)
 * 全部经此, 语义与 AM 后端逐点对齐。 */
static int RfBk_SetMode(uint8_t mode)
{
    if (mode > 2u) return APP_AM_ERR_PARAM;
    return RfBk_SetParam(RF_CMD_MODE, (uint16_t)mode);
}

/* ---- 读: 无逐参读命令, 每次全量 0x01 查询后按命令字位序抽取 ----
 * 参数区位于响应数据偏移 11 (版本2+ID5+报警次数4 之后), 按操作
 * 指令表命令升序 (0x05..0x20) 排布 (协议: "数据排布参考后面的命令排序")。 */
static int RfBk_GetParam(uint8_t cmd, uint16_t *value)
{
    if (cmd < RF_PARAM_FIRST_CMD || cmd > RF_PARAM_LAST_CMD)
        return APP_AM_ERR_PARAM;
    uint8_t rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    if (rf_query_frame(rdata, &rlen, RF_CMD_TIMEOUT_MS) != APP_AM_ERR_OK)
        return APP_AM_ERR_LINK;
    uint16_t idx = (uint16_t)(RF_QUERY_HDR_LEN + (uint16_t)(cmd - RF_PARAM_FIRST_CMD));
    if (idx >= rlen) return APP_AM_ERR_LINK;   /* 设备未提供该参数位 (N 不足) */
    if (value) *value = rdata[idx];
    return APP_AM_ERR_OK;
}

static void RfBk_Process(void)
{
    /* 轮询被动上报: 0x3A 计数帧 (0x11 入账源) + 0x04 心跳 (静默丢弃) */
    uint8_t rcmd, rdata[AM_HL_PAYLOAD_MAX];
    uint8_t rlen;
    uint32_t now;
    while (AM_HL_TryRecvFrame(&rcmd, rdata, &rlen) == 0) {
        now = SysTickHl_GetMs();
        if (rcmd == RF_CMD_ALARM_REPORT) {
            s_evtCount++;
            s_lastEvtMs = now;

            /* 帧距 > GAP: 上一事件已结算, 本帧开新事件 (与 AM cmd17 同构) */
            if (s_burstFrames == 0u || (now - s_lastRptMs) > RF_BURST_GAP_MS) {
                s_burstFrames = 1u;
                s_deact = AM_DEACT_IDLE;
            } else {
                s_burstFrames++;
            }
            s_lastRptMs = now;
        }
    }

    /* 事件静默结算: 供 GET_STATUS 轮询; 0x11 流程逐帧入账不依赖此 */
    if (s_burstFrames != 0u && (SysTickHl_GetMs() - s_lastRptMs) > RF_BURST_GAP_MS) {
        s_deact = AM_DEACT_SUCCESS;
        s_deactCount++;
        s_burstFrames = 0u;
    }

    /* 链路瞬断探测 (AM 同款): 断链且无事件结算在进行时轻量探链,
     * 0x01 单帧 (200ms 预算), 失败 2s/4s/8s 退避。 */
    if (s_link != 0 && s_burstFrames == 0u) {
        uint32_t now2 = SysTickHl_GetMs();
        if (s_probeNextMs == 0u || (int32_t)(now2 - s_probeNextMs) >= 0) {
            uint32_t period = 2000u << ((s_probeBackoff > 2u) ? 2u : s_probeBackoff);
            s_probeNextMs = now2 + period;
            uint8_t pdata[AM_HL_PAYLOAD_MAX];
            uint8_t plen;
            if (rf_query_frame(pdata, &plen, RF_PROBE_TIMEOUT_MS) == 0) {
                s_link = 0;
                s_probeBackoff = 0u;
            } else if (s_probeBackoff < 3u) {
                s_probeBackoff++;
            }
        }
    } else {
        s_probeNextMs = 0u;               /* 链路正常/事件结算中: 不探测 */
    }
}

static int           RfBk_GetLinkStatus(void) { return s_link; }
static uint32_t      RfBk_GetEventCount(void)  { return s_evtCount; }
static uint32_t      RfBk_GetLastEventMs(void) { return s_lastEvtMs; }
static AppAMDeactState_t RfBk_GetDeactState(void) { return s_deact; }
static uint32_t      RfBk_GetDeactCount(void) { return s_deactCount; }
static uint32_t      RfBk_GetFailCount(void)  { return s_failCount; }
static uint8_t       RfBk_BurstActive(void)
{
    return (s_burstFrames != 0u) ? 1u : 0u;   /* 模式看门狗静默门控用 */
}

/* ---------- 后端接口表 (配置/波形为 AM 专属, RF 置 NULL ---------- */
const DecoderBkOps_t g_rfBkOps = {
    RfBk_Init,
    RfBk_Process,
    RfBk_SetMode,
    RfBk_SetParam,
    RfBk_GetParam,
    RfBk_Query,
    RfBk_GetLinkStatus,
    RfBk_GetEventCount,
    RfBk_GetLastEventMs,
    RfBk_GetDeactState,
    RfBk_GetDeactCount,
    RfBk_GetFailCount,
    RfBk_BurstActive,
    NULL,   /* GetConfig: AM 专属 (RF 参数走 SET/GET_PARAM 透传) */
    NULL,   /* SetConfig */
    NULL,   /* CaptureWave: RF 0x02 波形不接入 */
    NULL,   /* GetWavePage */
    NULL    /* GetWavePoints */
};
