#include "App_UHF.h"
#include "App_UHF_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * UHF 控制面状态机 (SIM7500 / EX10 平台)
 *
 * 控制面 / 数据面解耦:
 *   控制面  = App_UHF:  状态机 (IDLE/READY/INVENTORY/READ/WRITE/ERROR)
 *                       + 配置 (功率/天线/校验) get/set, 上电/下电/停止
 *   数据面  = 标签环形缓冲: App_UHF_TagTake 取走盘点/读取结果
 *   硬件层  = App_UHF_HL: UART4 传输 + EX10 0xFF 帧收发 + CRC16 (由应用驱动)
 *
 * 状态机迁移:
 *   IDLE --Open--> READY --Inventory/Read/Write--> 忙态 --完成或Stop--> READY
 *   忙态 --链路失败--> ERROR --Query/Clear--> READY
 * ===================================================================== */

/* EX10 通用指令码 (见《EX10系列模块通信协议》) */
#define UHF_EX_QUERY_LAYER        0x0Cu   /* 获取当前层: Bootloader=0x11 / APP=0x12 */
#define UHF_EX_START_APP          0x04u   /* 启动 APP Firmware 层 */
#define UHF_EX_SET_ANT_PWR        0x91u   /* 设天线/功率 */
#define UHF_EX_SET_BAND           0x97u   /* 设工作频段 (Region Code) */
#define UHF_EX_GET_BAND           0x67u   /* 获取当前频段 */
#define UHF_EX_SET_GEN2CFG        0x9Bu   /* 设标签协议配置 (Session/Target/Q) */
#define UHF_EX_INVENTORY_SINGLE   0x21u   /* 单标签盘存: 读到 1 个标签立即返回 EPC */
#define UHF_EX_READ_GEN2          0x28u   /* 读存储区 */
#define UHF_EX_WRITE_GEN2         0x2Du   /* 块写存储区 */
#define UHF_EX_GET_VERSION        0x05u   /* 获取版本 (链路探测) */
#define UHF_EX_VSWR               0xAA4Au /* 扩展指令: 回波检测 (驻波比) */

/* 模块状态码 (Data 中 status 2字节, 0=成功) */
#define UHF_STATUS_OK         0x0000u

/* 层值 (0x0C 响应) */
#define UHF_LAYER_BOOTLOADER  0x11u
#define UHF_LAYER_APPFW       0x12u

/* 协议级同步超时 (等待元命令响应) */
#define UHF_CMD_TIMEOUT_MS    700u
/* 操作超时: 盘点/读写没有固定时长, 由 Stop 或空闲检测结束 */
#define UHF_IDLE_DETECT_MS    1500u   /* 长时间无标签/响应视为本次操作结束 */

static AppUHFState_t  s_state;
static AppUHFConfig_t s_cfg;
static AppUHFConfig_t s_cfgSaved;        /* 上次成功下发的配置备份 */
static AppUHFStatus_t s_status;          /* 状态/错误监控缓存 */
static int            s_link;            /* 0=正常 */
static uint8_t        s_powered;
static uint8_t        s_appLayer;        /* 模块当前所在层 (0=未知) */
static uint32_t       s_totalTags;
static uint32_t       s_actStartMs;      /* 当前操作开始时间戳 */
static uint32_t       s_lastActivityMs;  /* 最近一次数据活动时间戳 */
static uint32_t       s_opExpire;        /* 本操作是否已超时空闲 */

static AppUHFTag_t    s_tagBuf[APP_UHF_TAG_BUF_SIZE];
static uint16_t       s_tagHead, s_tagTail;

static void uhf_state(AppUHFState_t st)
{
    s_state = st;
}

void App_UHF_Init(void)
{
    UHF_HL_Init();
    s_cfg = (AppUHFConfig_t)APP_UHF_CONFIG_DEFAULT;
    s_cfgSaved = s_cfg;
    s_state = APP_UHF_IDLE;
    s_link = 0;
    s_powered = 0;
    s_appLayer = 0;
    s_totalTags = 0;
    s_tagHead = 0; s_tagTail = 0;
    s_actStartMs = 0; s_lastActivityMs = 0; s_opExpire = 0;
    s_status.lastErr = APP_UHF_ERR_OK;
    s_status.antRl = 0; s_status.antVswr = 0;
    s_status.antennaOk = 0; s_status.powered = 0;
}

/* ---------- 数据面: 标签入队 ---------- */
static void uhf_tag_push(const AppUHFTag_t *t)
{
    uint16_t next = (uint16_t)((s_tagHead + 1) % APP_UHF_TAG_BUF_SIZE);
    if (next == s_tagTail) s_tagTail = (uint16_t)((s_tagTail + 1) % APP_UHF_TAG_BUF_SIZE);
    s_tagBuf[s_tagHead] = *t;
    s_tagHead = next;
    s_totalTags++;
    s_lastActivityMs = SysTickHl_GetMs();
}

/* ---------- 探测: 获取模块当前层 ---------- */
static int uhf_probe_layer(void)
{
    uint16_t st, len;
    uint8_t  rx[UHF_HL_FRAME_MAX];
    int r = UHF_HL_Transact(UHF_EX_QUERY_LAYER, (const uint8_t *)0, 0u,
                            &st, rx, &len, UHF_CMD_TIMEOUT_MS);
    if (r != 0) return r;
    if (st != UHF_STATUS_OK) return UHF_HL_LINK_BAD_CRC;
    if (len < 1u) return UHF_HL_LINK_BAD_CRC;
    s_appLayer = rx[0];
    return UHF_HL_LINK_OK;
}

/* ---------- 确保模块处于 APP 层 (Bootloader -> 0x04 启动 APP) ---------- */
static int uhf_ensure_app(void)
{
    if (uhf_probe_layer() != UHF_HL_LINK_OK) return UHF_HL_LINK_TIMEOUT;
    if (s_appLayer == UHF_LAYER_APPFW) return UHF_HL_LINK_OK;

    /* 处于 Bootloader, 发 0x04 启动 APP 并等待模块切换 */
    {
        uint16_t st, len2;
        uint8_t  rx[UHF_HL_FRAME_MAX];
        if (UHF_HL_Transact(UHF_EX_START_APP, (const uint8_t *)0, 0u,
                            &st, rx, &len2, UHF_CMD_TIMEOUT_MS) != 0)
            return UHF_HL_LINK_TIMEOUT;
        if (st != UHF_STATUS_OK) return UHF_HL_LINK_BAD_CRC;
    }
    SysTickHl_DelayMs(250u);   /* 切换后稳定 (文档 4.6: 收到成功响应后等 250ms) */

    /* 再次确认进入 APP 层 */
    return uhf_probe_layer();
}

/* ---------- 下发电机/天线参数到模块 (0x91) ----------
 * Option=0x03: 设置天线收发功率. Data: [Option, TX天线号, 读功率(2), 写功率(2)]
 * 功率单位 0.01dBm, 实际精度 1dBm, 故 dBm*1.0*100 */
static int uhf_apply_config(void)
{
    uint8_t p[6];
    uint16_t pow = (uint16_t)((uint32_t)s_cfg.powerDbm * 100u);

    p[0] = 0x03u;                 /* Option: 设置收发功率 */
    p[1] = 0x01u;                 /* TX 逻辑天线号 1 (模块天线口, 硬件无天线切换) */
    p[2] = (uint8_t)(pow >> 8);   /* 读功率 高在前 */
    p[3] = (uint8_t)(pow & 0xFF);
    p[4] = (uint8_t)(pow >> 8);   /* 写功率 */
    p[5] = (uint8_t)(pow & 0xFF);

    {
        uint16_t st, len;
        uint8_t  rx[UHF_HL_FRAME_MAX];
        int r = UHF_HL_Transact(UHF_EX_SET_ANT_PWR, p, 6u,
                                &st, rx, &len, UHF_CMD_TIMEOUT_MS);
        if (r != 0) return APP_UHF_ERR_LINK;
        if (st != UHF_STATUS_OK) return APP_UHF_ERR_LINK;
    }

    /* 使能天线 1: Option=0x00, Data=[0x00, TX=1, RX=1] */
    {
        uint8_t a[3];
        a[0] = 0x00u;
        a[1] = 0x01u;
        a[2] = 0x01u;
        uint16_t st2, len2;
        uint8_t  rx2[UHF_HL_FRAME_MAX];
        int r2 = UHF_HL_Transact(UHF_EX_SET_ANT_PWR, a, 3u,
                                 &st2, rx2, &len2, UHF_CMD_TIMEOUT_MS);
        if (r2 != 0) return APP_UHF_ERR_LINK;
        if (st2 != UHF_STATUS_OK) return APP_UHF_ERR_LINK;
    }

    /* 设工作频段 (0x97): Data=[Region Code] */
    {
        uint8_t b[1] = { s_cfg.band };
        uint16_t st3, len3;
        uint8_t  rx3[UHF_HL_FRAME_MAX];
        int r3 = UHF_HL_Transact(UHF_EX_SET_BAND, b, 1u,
                                 &st3, rx3, &len3, UHF_CMD_TIMEOUT_MS);
        if (r3 != 0) return APP_UHF_ERR_LINK;
        if (st3 != UHF_STATUS_OK) return APP_UHF_ERR_LINK;
    }

    /* 设 Gen2 协议配置 (0x9B): Protocol=0x05.
     * Session (Param=0x00, Value=session);
     * Target  (Param=0x01, Option=0x01 静态, Value=target);
     * Q       (Param=0x12; q=0→动态, 否则 Option=0x01 静态 Value=q). */
    {
        uint8_t g[4];
        uint16_t stx, lenx;
        uint8_t  rxx[UHF_HL_FRAME_MAX];
        int rx;

        /* Session */
        g[0] = 0x05u; g[1] = 0x00u; g[2] = s_cfg.session;
        rx = UHF_HL_Transact(UHF_EX_SET_GEN2CFG, g, 3u, &stx, rxx, &lenx, UHF_CMD_TIMEOUT_MS);
        if (rx != 0 || stx != UHF_STATUS_OK) return APP_UHF_ERR_LINK;

        /* Target: 静态 A/B */
        g[0] = 0x05u; g[1] = 0x01u; g[2] = 0x01u; g[3] = s_cfg.target;
        rx = UHF_HL_Transact(UHF_EX_SET_GEN2CFG, g, 4u, &stx, rxx, &lenx, UHF_CMD_TIMEOUT_MS);
        if (rx != 0 || stx != UHF_STATUS_OK) return APP_UHF_ERR_LINK;

        /* Q: 0=动态(无起始), 否则静态指定 */
        if (s_cfg.q == 0u) {
            g[0] = 0x05u; g[1] = 0x12u; g[2] = 0x00u;   /* 动态 Q */
            rx = UHF_HL_Transact(UHF_EX_SET_GEN2CFG, g, 3u, &stx, rxx, &lenx, UHF_CMD_TIMEOUT_MS);
        } else {
            g[0] = 0x05u; g[1] = 0x12u; g[2] = 0x01u; g[3] = s_cfg.q;   /* 静态 Q */
            rx = UHF_HL_Transact(UHF_EX_SET_GEN2CFG, g, 4u, &stx, rxx, &lenx, UHF_CMD_TIMEOUT_MS);
        }
        if (rx != 0 || stx != UHF_STATUS_OK) return APP_UHF_ERR_LINK;
    }
    return APP_UHF_ERR_OK;
}

/* ---------- 回波检测 (0xAA4A 扩展指令) ----------
 * 发送 Data: [功率(2)=0BB8 无效, 逻辑天线=1, 频段码, 频率数 N=0(测频段全部频率)]
 * 响应 subData: 首 5 字节同发送, 其后每频率 4 字节 [freq(3), RL(1)].
 * RL 反射损耗 = 发射功率-反射功率 (0.1dB). 仅取首个频率结果.
 * VSWR 由 RL 查表 (协议 10.1 表, VSWR=(10^(RL/20)+1)/(10^(RL/20)-1)):
 *   索引=RL/10 (整数 dB), 值= VSWR*100. 例如 RL=3dB→585, 12dB→167. */
static const uint16_t s_vswrTable[26] = {
    9900, 1739, 872, 585, 442, 357, 301, 261, 232, 210, 192, 178, 167,
    158, 150, 143, 138, 133, 129, 125, 122, 120, 117, 115, 113, 112
};
static void uhf_check_vswr(void)
{
    uint8_t  tx[5];
    uint16_t st, dlen;
    uint8_t  rx[UHF_HL_FRAME_MAX];

    s_status.antRl = 0; s_status.antVswr = 0; s_status.antennaOk = 0;

    tx[0] = 0x0Bu; tx[1] = 0xB8u;          /* 功率 3000 (模块内实际用 20dBm) */
    tx[2] = 0x01u;                          /* 逻辑天线 1 */
    tx[3] = s_cfg.band;                     /* 频段 */
    tx[4] = 0x00u;                          /* N=0: 频段内全部频率 */

    if (UHF_HL_TransactExt(UHF_EX_VSWR, tx, 5u,
                           &st, rx, &dlen, 5000u) != 0 || st != UHF_STATUS_OK) {
        s_status.lastErr = APP_UHF_ERR_LINK;
        return;
    }
    /* subData 前 5 字节回显, 之后为每频率 [freq(3), RL(1)] */
    if (dlen < 5u + 4u) { s_status.lastErr = APP_UHF_ERR_LINK; return; }
    uint8_t rl = rx[5 + 3u];               /* 首个频率的 RL (0.1dB) */

    s_status.antRl = (uint16_t)rl;         /* 0.1dB */
    uint16_t idx = (uint16_t)(rl / 10u);
    if (idx > 25u) idx = 25u;
    s_status.antVswr = s_vswrTable[idx];   /* VSWR×100 */
    /* 判定: RL>=0.5dB 且 VSWR<=7.00 视为天线正常 */
    s_status.antennaOk = (rl >= 5u && s_status.antVswr <= 700u) ? 1u : 0u;
    s_status.lastErr = APP_UHF_ERR_OK;
}

int App_UHF_CheckAntenna(void)
{
    if (!s_powered) return APP_UHF_ERR_NOT_READY;
    uhf_check_vswr();
    return APP_UHF_ERR_OK;
}

int App_UHF_Open(void)
{
    if (s_powered) return APP_UHF_ERR_OK;
    UHF_HL_SetPowerEn(1u);
    SysTickHl_DelayMs(100u);               /* 模块上电稳定 */
    UHF_HL_RxFlush();
    s_powered = 1;
    s_appLayer = 0;

    /* 确保模块进入 APP 层 (Bootloader 上电默认) */
    if (uhf_ensure_app() != UHF_HL_LINK_OK) {
        uhf_state(APP_UHF_ERROR);
        return APP_UHF_ERR_LINK;
    }
    /* 下发功率/天线/频段/Gen2 配置 */
    int r = uhf_apply_config();
    if (r != APP_UHF_ERR_OK) {
        uhf_state(APP_UHF_ERROR);
        return r;
    }
    s_cfgSaved = s_cfg;

    /* 上电回波检测 (同步阻塞, 判断天线连接; 告警仍可用不阻断) */
    s_status.powered = 1;
    uhf_state(APP_UHF_ANT_CHECK);
    uhf_check_vswr();
    s_status.powered = 1;
    uhf_state(APP_UHF_READY);
    return APP_UHF_ERR_OK;
}

int App_UHF_Close(void)
{
    (void)App_UHF_Stop();
    UHF_HL_SetPowerEn(0u);
    s_powered = 0;
    s_appLayer = 0;
    s_status.powered = 0;
    s_status.antennaOk = 0;
    uhf_state(APP_UHF_IDLE);
    return APP_UHF_ERR_OK;
}

int App_UHF_Stop(void)
{
    if (s_state == APP_UHF_INVENTORY || s_state == APP_UHF_READ ||
        s_state == APP_UHF_WRITE)
        uhf_state(APP_UHF_READY);
    s_opExpire = 0;
    return APP_UHF_ERR_OK;
}

static int uhf_enter_busy(AppUHFState_t st, uint8_t applyCfg)
{
    if (s_state == APP_UHF_READY || s_state == APP_UHF_IDLE) {
        if (applyCfg) {
            int r = uhf_apply_config();
            if (r != APP_UHF_ERR_OK) { uhf_state(APP_UHF_ERROR); return r; }
        }
        uhf_state(st);
        s_actStartMs = SysTickHl_GetMs();
        s_lastActivityMs = s_actStartMs;
        s_opExpire = 0;
        return APP_UHF_ERR_OK;
    }
    return APP_UHF_ERR_BUSY;
}

int App_UHF_Inventory(void)
{
    int r = uhf_enter_busy(APP_UHF_INVENTORY, 1u);
    if (r != APP_UHF_ERR_OK) return r;

    /* 单标签盘存 0x21: Data=[Timeout(2), Option(1)] (Option BIT4=0, 无元数据).
     * 在 Timeout 内读到 1 个标签立即返回, 响应 Data 直接含 EPC, 无需 0x29 取缓冲.
     * Timeout=1000ms, Option=0x00 (无过滤). 收到响应即结束. */
    uint8_t d[3];
    d[0] = (uint8_t)(1000u >> 8); d[1] = (uint8_t)(1000u & 0xFF);
    d[2] = 0x00u;
    UHF_HL_SendFrame(UHF_EX_INVENTORY_SINGLE, d, 3u);
    return APP_UHF_ERR_OK;
}

int App_UHF_ReadTag(const uint8_t *epc, uint8_t epcLen, uint8_t bank,
                    uint8_t addr, uint8_t cnt)
{
    (void)epc; (void)epcLen;
    int r = uhf_enter_busy(APP_UHF_READ, 1u);
    if (r != APP_UHF_ERR_OK) return r;

    /* 0x28 读存储区: [Timeout(2)=03E8, Option=0x00, MemBank, ReadAddr(4), WordCnt]
     * 本次只读 EPC 区 (bank=1), 过滤由上层 epc 参数略 (单天线/无过滤首标签) */
    uint8_t d[9];
    d[0] = 0x03u; d[1] = 0xE8u;        /* Timeout 1000ms */
    d[2] = 0x00u;                       /* Option: 无过滤 */
    d[3] = bank;                        /* MemBank */
    d[4] = 0x00u; d[5] = 0x00u;         /* ReadAddr (addr 参作低字节, 高位 0) */
    d[6] = 0x00u; d[7] = addr;
    d[8] = cnt;                         /* WordCnt */
    UHF_HL_SendFrame(UHF_EX_READ_GEN2, d, 9u);
    return APP_UHF_ERR_OK;
}

int App_UHF_WriteTag(const uint8_t *epc, uint8_t epcLen, uint8_t bank,
                     uint8_t addr, const uint8_t *data, uint8_t len)
{
    (void)epc; (void)epcLen;
    int r = uhf_enter_busy(APP_UHF_WRITE, 1u);
    if (r != APP_UHF_ERR_OK) return r;

    /* 0x2D 块写: [Timeout(2), MemBank, WriteAddr(4), ...Data] */
    uint8_t d[2 + 1 + 4 + 128];
    d[0] = 0x03u; d[1] = 0xE8u;
    d[2] = bank;
    d[3] = 0x00u; d[4] = 0x00u; d[5] = 0x00u; d[6] = addr;
    for (uint8_t i = 0; i < len && i < 128u; i++) d[7 + i] = data[i];
    UHF_HL_SendFrame(UHF_EX_WRITE_GEN2, d, (uint16_t)(7u + len));
    return APP_UHF_ERR_OK;
}

int App_UHF_Query(void)
{
    uint16_t st, len;
    uint8_t  rx[UHF_HL_FRAME_MAX];
    int r = UHF_HL_Transact(UHF_EX_GET_VERSION, (const uint8_t *)0, 0u,
                            &st, rx, &len, UHF_CMD_TIMEOUT_MS);
    if (r != 0) {
        s_link = UHF_HL_LINK_TIMEOUT;
        uhf_state(APP_UHF_ERROR);
        return APP_UHF_ERR_LINK;
    }
    s_link = 0;
    if (s_state == APP_UHF_ERROR) uhf_state(APP_UHF_READY);
    return APP_UHF_ERR_OK;
}

int App_UHF_GetConfig(AppUHFConfig_t *cfg)
{
    if (!cfg) return APP_UHF_ERR_PARAM;
    *cfg = s_cfg;
    return APP_UHF_ERR_OK;
}

int App_UHF_SetConfig(const AppUHFConfig_t *cfg, int save)
{
    (void)save;
    if (!cfg) return APP_UHF_ERR_PARAM;
    if (cfg->powerDbm < 5u || cfg->powerDbm > 30u) return APP_UHF_ERR_PARAM;
    if (cfg->antenna > 1u) return APP_UHF_ERR_PARAM;

    s_cfg = *cfg;
    if (s_powered) {
        int r = uhf_apply_config();
        if (r != APP_UHF_ERR_OK) {
            s_status.lastErr = r;
            uhf_state(APP_UHF_ERROR);
            return r;
        }
        s_cfgSaved = s_cfg;
        s_status.lastErr = APP_UHF_ERR_OK;
    }
    return APP_UHF_ERR_OK;
}

int App_UHF_GetStatus(AppUHFStatus_t *st)
{
    if (!st) return APP_UHF_ERR_PARAM;
    st->lastErr   = s_status.lastErr;
    st->antRl     = s_status.antRl;
    st->antVswr  = s_status.antVswr;
    st->antennaOk = s_status.antennaOk;
    st->powered   = s_status.powered;
    return APP_UHF_ERR_OK;
}

uint16_t App_UHF_TagCount(void)
{
    if (s_tagHead >= s_tagTail) return (uint16_t)(s_tagHead - s_tagTail);
    return (uint16_t)(APP_UHF_TAG_BUF_SIZE - s_tagTail + s_tagHead);
}

int App_UHF_TagTake(AppUHFTag_t *t)
{
    if (s_tagHead == s_tagTail) return -1;
    if (t) *t = s_tagBuf[s_tagTail];
    s_tagTail = (uint16_t)((s_tagTail + 1) % APP_UHF_TAG_BUF_SIZE);
    return 0;
}

static void uhf_decode_incoming(void)
{
    uint8_t cmd, data[UHF_HL_FRAME_MAX];
    uint16_t st, len;
    while (UHF_HL_RecvFrame(&cmd, &st, data, &len, 0u) == 0) {
        switch (cmd) {
        case UHF_EX_INVENTORY_SINGLE: {
            /* 单标签盘存 0x21 响应 Data: [Option(1), EPCID(N), TagCRC(2)].
             * EPC 直接从响应返回, 无需 0x29 取缓冲. len = 1 + N + 2.
             * 健壮性: 只接受真实 EPC 长度的帧. 边际 RF 读错误时模块可能
             * 返回 status=0 但 EPC 字段被截断/损坏 (仅 1~2 字节), 这种
             * 伪标签必须丢弃, 否则会被当作一个"短标签"入缓冲. */
            if (st == UHF_STATUS_OK && len >= 3u) {
                uint16_t n = len - 3u;              /* EPCID 字节数 */
                if (n >= UHF_EPC_MIN_LEN && n <= 16u) {
                    AppUHFTag_t t;
                    t.rssi = 0;
                    for (uint16_t k = 0; k < n; k++) t.epc[k] = data[1 + k];
                    t.epcLen = (uint8_t)n;
                    uhf_tag_push(&t);
                }
                /* 单标签操作完成即结束盘点状态 */
                if (s_state == APP_UHF_INVENTORY) {
                    uhf_state(APP_UHF_READY);
                    s_opExpire = 0;
                }
            }
            break;
        }
        case UHF_EX_READ_GEN2:
            /* 读存储区返回: [Option(1), Data...] 简化为一条标签流 (带数据).
             * 同样丢弃过短/损坏数据 (见 0x21 注释). */
            if (st == UHF_STATUS_OK && len > 1u) {
                uint16_t m = len - 1u;
                if (m >= UHF_EPC_MIN_LEN && m <= 16u) {
                    AppUHFTag_t t;
                    t.rssi = 0;
                    for (uint16_t k = 0; k < m; k++) t.epc[k] = data[1 + k];
                    t.epcLen = (uint8_t)m;
                    uhf_tag_push(&t);
                }
            }
            break;
        default:
            break;
        }
    }
}

void App_UHF_Process(void)
{
    /* 数据面: 取硬件字节流解析入缓冲 */
    uhf_decode_incoming();

    /* 忙态: 若长时间无响应/标签视为本次操作结束 (超时保护).
     * 0x21 正常收到 EPC 响应时已在解码里即刻回到 READY. */
    if (s_state == APP_UHF_INVENTORY || s_state == APP_UHF_READ ||
        s_state == APP_UHF_WRITE) {
        uint32_t now = SysTickHl_GetMs();
        if ((now - s_lastActivityMs) >= UHF_IDLE_DETECT_MS) {
            uhf_state(APP_UHF_READY);
            s_opExpire = 0;
        }
    }
}

AppUHFState_t App_UHF_GetState(void) { return s_state; }
int      App_UHF_GetLinkStatus(void) { return s_link; }
uint32_t App_UHF_GetTotalTags(void)  { return s_totalTags; }
int      App_UHF_IsBusy(void)
{
    return (s_state == APP_UHF_INVENTORY || s_state == APP_UHF_READ ||
            s_state == APP_UHF_WRITE || s_state == APP_UHF_ANT_CHECK) ? 1 : 0;
}
