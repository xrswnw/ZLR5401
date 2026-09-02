#include "App_UHF.h"
#include "App_UHF_HL.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"          /* 同步阻塞期间喂狗 */
#include "App_NewPeriph_HL.h"     /* 扫描识别到标签蜂鸣提示 */

/* =====================================================================
 * UHF 控制面状态机 (SIM7500 / EX10 平台)
 *
 * 控制面 / 数据面解耦:
 *   控制面  = App_UHF:  状态机 (IDLE/READY/INVENTORY/READ/WRITE/ERROR)
 *                       + 配置 (功率/天线/校验) get/set, 上电/下电/停止
 *   数据面  = 标签环形缓冲: App_UHF_TagTake 取走盘点/读取结果
 *   硬件层  = App_UHF_HL: USART3 传输 + EX10 0xFF 帧收发 + CRC16 (由应用驱动)
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
#define UHF_EX_INVENTORY_MULTI    0x22u   /* 同步/普通盘存: 超时内盘端口内所有标签入模块缓冲 */
#define UHF_EX_READ_GEN2          0x28u   /* 读存储区 */
#define UHF_EX_GET_BUFFER         0x29u   /* 获取标签缓冲区 (取回 0x22 盘存结果) */
#define UHF_EX_WRITE_GEN2         0x2Du   /* 块写存储区 */
#define UHF_EX_GET_VERSION        0x05u   /* 获取版本 (链路探测) */
#define UHF_EX_VSWR               0xAA4Au /* 扩展指令: 回波检测 (驻波比) */

/* 0x29 取缓冲的元数据: BIT0 ReadCount + BIT1 RSSI + BIT2 AntID + BIT4 Timestamp
 * 每标签块 = ReadCount(1) RSSI(1) AntID(1) Timestamp(4) EPCLen(2) PC(2) EPCID TagCRC(2)
 * 固定(元数据+EPCLen+PC+TagCRC) = 1+1+1+4 + 2 + 2 + 2 = 13 字节/标签, 不含 EPC */
#define UHF_META_FLAGS            0x0017u

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

/* ---- 自动扫描状态 (连续盘点驱动) ---- */
static uint8_t        s_scanOn;          /* 1=自动扫描运行中 */
static uint16_t       s_scanCycleMs;     /* 每轮 0x22 盘存超时 (单轮时长) */
static uint32_t       s_scanNextMs;      /* 下一轮盘点启动时间戳 */

/* ---- 扫描中挂起的配置 (需求) ----
 * 自动扫描中的 SET_CONFIG 不同步下发 (避免打断 0x22->0x29 时序), 而是存到
 * s_pendingCfg, 由 Process 在下轮盘点前插入下发, 然后继续盘点. */
static AppUHFConfig_t s_pendingCfg;
static uint8_t        s_pendingCfgValid;

static AppUHFTag_t    s_tagBuf[APP_UHF_TAG_BUF_SIZE];
static uint16_t       s_tagHead, s_tagTail;

/* ---- 自动扫描 (预留: 目前移除了; 上位机触发 + 同步返回) ---- */

/* ---- 真机盘点诊断记录 (Round_050/051) ---- */
static AppUHFDump_t   s_dump;

static void uhf_dump_req(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    s_dump.reqCmd = cmd; s_dump.reqLen = (len > 5u) ? 5u : len;
    for (uint8_t i = 0; i < s_dump.reqLen; i++) s_dump.reqData[i] = data[i];
}
static void uhf_dump_rsp(uint16_t st, const uint8_t *data, uint16_t len)
{
    s_dump.cnt++;
    s_dump.rspStatus = st;
    s_dump.rspLen = (len > 40u) ? 40u : len;
    for (uint16_t i = 0; i < s_dump.rspLen; i++) s_dump.rspData[i] = data[i];
    UHF_HL_GetRawLast(&s_dump.rawDataLen, &s_dump.rspCmd);
}

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
    s_scanOn = 0; s_scanCycleMs = 0; s_scanNextMs = 0;
    s_pendingCfgValid = 0;
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
    /* 同步返回模式: 标签入缓冲, 由调用方 (如同步盘点) 在响应帧一并返回, 无主动推送.
     * 自动扫描模式: 识别到标签即蜂鸣 50ms 提示 (到期由 IrBuzzer 自动停, 不阻塞). */
    if (s_scanOn) App_NewPeriph_BeepPulse(50u);
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
    (void)App_UHF_ScanStop();
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
        s_state == APP_UHF_WRITE || s_state == APP_UHF_GETBUF)
        uhf_state(APP_UHF_READY);
    s_opExpire = 0;
    return APP_UHF_ERR_OK;
}

int App_UHF_ScanStart(uint16_t cycleMs)
{
    if (!s_powered) return APP_UHF_ERR_NOT_READY;
    if (s_scanOn) return APP_UHF_ERR_OK;              /* 已在扫, 幂等 */
    if (s_state != APP_UHF_READY && s_state != APP_UHF_IDLE &&
        s_state != APP_UHF_ERROR)
        return APP_UHF_ERR_BUSY;                      /* 其它操作进行中 */

    if (cycleMs == 0u) cycleMs = 1000u;
    if (cycleMs > 10000u) cycleMs = 10000u;

    /* 清空缓冲计数并进入扫描态; 首轮盘点由 Process 驱动立即发起 */
    s_tagHead = 0; s_tagTail = 0; s_totalTags = 0;
    s_scanCycleMs = cycleMs;
    s_scanOn = 1;
    s_scanNextMs = SysTickHl_GetMs();                 /* 使首轮立刻启动 */
    uhf_state(APP_UHF_SCAN);
    return APP_UHF_ERR_OK;
}

int App_UHF_ScanStop(void)
{
    if (!s_scanOn) return APP_UHF_ERR_OK;
    s_scanOn = 0;
    /* 停扫后立即补发扫描中挂起的配置, 避免配置丢失 */
    if (s_pendingCfgValid) {
        s_pendingCfgValid = 0;
        if (uhf_apply_config() == APP_UHF_ERR_OK) {
            s_cfgSaved = s_cfg;
            s_status.lastErr = APP_UHF_ERR_OK;
        }
    }
    /* 停掉当前进行中的盘点轮次 */
    if (s_state == APP_UHF_INVENTORY || s_state == APP_UHF_GETBUF ||
        s_state == APP_UHF_SCAN)
        uhf_state(APP_UHF_READY);
    s_opExpire = 0;
    return APP_UHF_ERR_OK;
}

int App_UHF_IsScanning(void)
{
    return s_scanOn ? 1 : 0;
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

static int uhf_inventory_start(uint16_t timeoutMs)
{
    int r = uhf_enter_busy(APP_UHF_INVENTORY, 1u);
    if (r != APP_UHF_ERR_OK) return r;

    /* 同步/普通盘存 0x22 (多标签): Data=[Option(1), SearchFlags(2), Timeout(2)].
     * 在 timeout 内盘存天线场内所有标签入模块内部缓冲(最多 1200), 随后需 0x29
     * 取回。Option=0 (无过滤, 无嵌入指令), SearchFlags=0, Timeout 用户可配。
     * 收到 0x22 响应后 -> GETBUF 态发 0x29 批次取标签 (见 App_UHF_Process). */
    uint8_t d[5];
    d[0] = 0x00u;                                        /* Option: 无过滤 */
    d[1] = 0x00u; d[2] = 0x00u;                          /* Search Flags */
    d[3] = (uint8_t)(timeoutMs >> 8); d[4] = (uint8_t)(timeoutMs & 0xFF);
    uhf_dump_req(UHF_EX_INVENTORY_MULTI, d, 5u);
    /* 与 Transact 不同, 这里是异步发送, 须先冲刷接收环, 否则先前配置命令
     * (UHF_HL_Transact) 残留的字节会被误当作本请求的响应帧解析. */
    UHF_HL_RxFlush();
    UHF_HL_SendFrame(UHF_EX_INVENTORY_MULTI, d, 5u);
    return APP_UHF_ERR_OK;
}

int App_UHF_InventoryTimeout(uint16_t timeoutMs)
{
    if (timeoutMs == 0u) timeoutMs = 1000u;
    if (timeoutMs > 10000u) timeoutMs = 10000u;
    return uhf_inventory_start(timeoutMs);
}

int App_UHF_Inventory(void)
{
    return uhf_inventory_start(1000u);
}

/* 同步阻塞盘点 (上位机触发): 全程阻塞完成 0x22 多标签盘存 -> 0x29 取回并解码,
 * 标签入缓冲。阻塞期间喂狗。返回值: 入缓冲标签数 (>=0) / 负=错误码 (APP_UHF_ERR_*).
 * 采用模块原始同步指令时序, 不在中断/状态机里自旋 OSTime. */
int App_UHF_InventorySync(uint16_t timeoutMs)
{
    if (timeoutMs == 0u) timeoutMs = 1000u;
    if (timeoutMs > 10000u) timeoutMs = 10000u;

    if (!s_powered) return APP_UHF_ERR_NOT_READY;
    if (s_scanOn) return APP_UHF_ERR_BUSY;   /* 自动扫描中进行, 拒绝单次同步盘点 */
    if (s_state != APP_UHF_READY && s_state != APP_UHF_IDLE &&
        s_state != APP_UHF_ERROR)
        return APP_UHF_ERR_BUSY;

    /* 清空上一轮缓冲与计数 */
    s_tagHead = 0; s_tagTail = 0; s_totalTags = 0;
    s_lastActivityMs = SysTickHl_GetMs();

    /* 发 0x22 (多标签同步盘存) */
    {
        uint8_t d[5];
        d[0] = 0x00u; d[1] = 0x00u; d[2] = 0x00u;
        d[3] = (uint8_t)(timeoutMs >> 8); d[4] = (uint8_t)(timeoutMs & 0xFF);
        uhf_dump_req(UHF_EX_INVENTORY_MULTI, d, 5u);
        UHF_HL_RxFlush();
        UHF_HL_SendFrame(UHF_EX_INVENTORY_MULTI, d, 5u);
    }

    /* 等待 0x22 响应: TagsFound 字节指示本模块是否盘到标签 */
    {
        uint8_t  cmd, data[UHF_HL_FRAME_MAX];
        uint16_t st, len;
        uint32_t t0 = SysTickHl_GetMs();
        int found = 0;
        while ((SysTickHl_GetMs() - t0) < (uint32_t)(timeoutMs + 500u)) {
            IwdgHl_Feed();
            if (UHF_HL_RecvFrame(&cmd, &st, data, &len, 0u) == 0) {
                if (cmd == UHF_EX_INVENTORY_MULTI) {
                    uhf_dump_rsp(st, data, len);
                    if (st == UHF_STATUS_OK && len >= 4u) found = data[3];
                    break;
                }
            }
        }
        if (!found) {
            uhf_state(APP_UHF_READY);
            return APP_UHF_ERR_NO_TAG;
        }
    }

    /* 发 0x29 取回标签缓冲 */
    {
        uint8_t d[3];
        d[0] = (uint8_t)(UHF_META_FLAGS >> 8);
        d[1] = (uint8_t)(UHF_META_FLAGS & 0xFF);
        d[2] = 0x00u;
        UHF_HL_RxFlush();
        UHF_HL_SendFrame(UHF_EX_GET_BUFFER, d, 3u);
    }

    /* 等待 0x29 响应并解码入缓冲 */
    {
        uint8_t  cmd, data[UHF_HL_FRAME_MAX];
        uint16_t st, len;
        uint32_t t0 = SysTickHl_GetMs();
        while ((SysTickHl_GetMs() - t0) < UHF_CMD_TIMEOUT_MS) {
            IwdgHl_Feed();
            if (UHF_HL_RecvFrame(&cmd, &st, data, &len, 0u) == 0) {
                if (cmd == UHF_EX_GET_BUFFER) {
                    uhf_dump_rsp(st, data, len);
                    if (st == UHF_STATUS_OK && len >= 4u) {
                        uint16_t pos  = 4u;                 /* MF(2)+Option(1)+TagCount(1) */
                        uint8_t  tcnt = data[3];
                        const uint16_t fixed = 7u + 2u + 2u + 2u;
                        while (tcnt-- > 0u) {
                            if (pos + fixed > len) break;
                            int8_t rssi = (int8_t)data[pos + 1u];
                            pos += 7u;                       /* 元数据 */
                            if (pos + 2u > len) break;
                            pos += 2u;                       /* EPC Length 域 (固定常量, 忽略) */
                            if (pos + 2u > len) break;
                            uint16_t pc = (uint16_t)((data[pos] << 8) | data[pos + 1]);
                            pos += 2u;
                            uint16_t epcLen = (uint16_t)((pc >> 11) * 2u);
                            if (pos + epcLen + 2u > len) break;
                            if (epcLen >= UHF_EPC_MIN_LEN && epcLen <= 16u) {
                                AppUHFTag_t t;
                                t.rssi = (uint32_t)rssi;
                                t.epcLen = (uint8_t)epcLen;
                                for (uint16_t k = 0; k < epcLen; k++) t.epc[k] = data[pos + k];
                                uhf_tag_push(&t);
                            }
                            pos += epcLen + 2u;
                        }
                    }
                    break;
                }
            }
        }
    }

    uhf_state(APP_UHF_READY);
    return (int)s_totalTags;
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
        if (s_scanOn) {
            /* 自动扫描中: 不立即下发 (避免打断 0x22->0x29 时序).
             * 挂起, 由 Process 在下轮盘点前插入下发, 返回 OK 让上层立即持久化. */
            s_pendingCfg = *cfg;
            s_pendingCfgValid = 1;
            s_status.lastErr = APP_UHF_ERR_OK;
            return APP_UHF_ERR_OK;
        }
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

void App_UHF_ClearTags(void)
{
    s_tagHead = 0u; s_tagTail = 0u;
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
        case UHF_EX_INVENTORY_MULTI: {
            /* 同步盘存 0x22 响应 Data: [Option(1), SearchFlags(2), TagsFound(1|4)].
             * 下一帧 0x22 响应对应上一帧 0x22 请求 (本轮盘点已结束).
             * 只要盘到 >=1 个标签, 切换到 GETBUF 态并发出 0x29 取回. */
            uhf_dump_rsp(st, data, len);
            if (st == UHF_STATUS_OK && s_state == APP_UHF_INVENTORY) {                uint8_t found = 0;
                if (len >= 4u) {
                    /* len = 1(Option) + 2(SearchFlags) + 1|4(TagsFound); 取第 4 字节 */
                    found = data[3];
                }
                if (found > 0u) {
                    uhf_state(APP_UHF_GETBUF);
                    s_opExpire = 0;
                    /* 发 0x29: [MetadataFlags(2), ReadOption(0)] 取未取部分 */
                    uint8_t d[3];
                    d[0] = (uint8_t)(UHF_META_FLAGS >> 8);
                    d[1] = (uint8_t)(UHF_META_FLAGS & 0xFF);
                    d[2] = 0x00u;
                    UHF_HL_SendFrame(UHF_EX_GET_BUFFER, d, 3u);
                } else {
                    uhf_state(APP_UHF_READY);
                    s_opExpire = 0;
                }
            }
            break;
        }
        case UHF_EX_GET_BUFFER: {
            /* 获取标签缓冲区 0x29 响应 (请求固定 MetadataFlags=0x0017):
             *   [MetadataFlags(2), ReadOption(1), TagCount(1),
             *    per tag: ReadCount(1) RSSI(1) AntID(1) Timestamp(4)
             *             EPCLen(2) PC(2) EPCID(N) TagCRC(2)]
             * 缓冲区取走后即从模块删除, 故一轮取完即回 READY.
             * 每标签固定部分 = 7 字节元数据 + EPCLen(2) + PC(2) + TagCRC(2). */
            uhf_dump_rsp(st, data, len);
            if (st == UHF_STATUS_OK && len >= 4u) {
                uint16_t pos  = 4u;                 /* 跳过 MF(2)+Option(1)+TagCount(1) */
                uint8_t  tcnt = data[3];
                const uint16_t fixed = 7u + 2u + 2u + 2u;   /* 元数据+EPCLen+PC+TagCRC */
                while (tcnt-- > 0u) {
                    if (pos + fixed > len) break;
                    uint16_t base = pos;
                    int8_t   rssi = (int8_t)data[base + 1u];  /* ReadCount, RSSI, AntID, Timestamp */
                    pos += 7u;                                 /* 元数据 */
                    if (pos + 2u > len) break;
                    pos += 2u;                            /* EPC Length 域 (固定常量, 忽略) */
                    if (pos + 2u > len) break;
                    uint16_t pc = (uint16_t)((data[pos] << 8) | data[pos + 1]);      /* PC */
                    pos += 2u;
                    /* 真实 EPC 长度取自 PC 高 5 位 (word 数 ×2 字节), 而非 EPC Length
                     * 域 (该域为固定常量, 如 0x0080, 不反映实际长度, 见协议 5.4 实例). */
                    uint16_t epcLen = (uint16_t)((pc >> 11) * 2u);
                    if (pos + epcLen + 2u > len) break;
                    if (epcLen >= UHF_EPC_MIN_LEN && epcLen <= 16u) {
                        AppUHFTag_t t;
                        t.rssi = (uint32_t)rssi;
                        t.epcLen = (uint8_t)epcLen;
                        for (uint16_t k = 0; k < epcLen; k++) t.epc[k] = data[pos + k];
                        uhf_tag_push(&t);
                    }
                    pos += epcLen + 2u;              /* EPCID + TagCRC */
                }
            }
            /* 取完缓冲即结束本轮多标签盘点 */
            if (s_state == APP_UHF_GETBUF) {
                uhf_state(APP_UHF_READY);
                s_opExpire = 0;
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

    /* 自动扫描驱动: 每轮盘点(0x22->0x29)完成回 READY 后, 到下一轮时刻再发起新一轮.
     * 单轮流程完全复用异步状态机; 标签经 0x29 解码入缓冲(uhf_tag_push), 不主动上报,
     * 主机用 GET_TAGS 拉取并带走. */
    if (s_scanOn) {
        uint32_t now = SysTickHl_GetMs();
        /* 空闲(READY/SCAN, 而 SCAN 只是扫描态占位)时:
         * 先插入挂起的配置下发(自动扫描中 SET_CONFIG 的下发在这里补), 再按周期起新一轮. */
        if (s_state == APP_UHF_READY || s_state == APP_UHF_SCAN) {
            if (s_pendingCfgValid) {
                s_pendingCfgValid = 0;
                if (uhf_apply_config() == APP_UHF_ERR_OK) {
                    s_cfgSaved = s_cfg;
                    s_status.lastErr = APP_UHF_ERR_OK;
                } else {
                    s_status.lastErr = APP_UHF_ERR_LINK;
                }
            }
            if ((int32_t)(now - s_scanNextMs) >= 0) {
                s_scanNextMs = now + s_scanCycleMs;
                uint8_t d[5];
                d[0] = 0x00u; d[1] = 0x00u; d[2] = 0x00u;
                d[3] = (uint8_t)(s_scanCycleMs >> 8); d[4] = (uint8_t)(s_scanCycleMs & 0xFF);
                uhf_dump_req(UHF_EX_INVENTORY_MULTI, d, 5u);
                UHF_HL_RxFlush();
                UHF_HL_SendFrame(UHF_EX_INVENTORY_MULTI, d, 5u);
                uhf_state(APP_UHF_INVENTORY);
                s_actStartMs = now; s_lastActivityMs = now; s_opExpire = 0;
            }
        }
    }

    /* 忙态: 若长时间无响应/标签视为本次操作结束 (超时保护).
     * 0x21/0x22 正常收到响应时已在解码里转态 (单标签回 READY, 多标签转 GETBUF). */
    if (s_state == APP_UHF_INVENTORY || s_state == APP_UHF_READ ||
        s_state == APP_UHF_WRITE || s_state == APP_UHF_GETBUF) {
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
int      App_UHF_IsPowered(void)     { return s_powered; }

/* ---- 业务扫描会话 S0 强制 (Round_098 优化 #21) ---- */
static AppUHFConfig_t s_scanSessSaved;   /* 用户会话配置快照 */
static uint8_t        s_scanSessActive;  /* 0=无窗 1=本就 S0 2=已临时切 S0 */

int App_UHF_ScanSessionBegin(void)
{
    if (s_scanSessActive != 0u) return 0;          /* 已在窗内 (幂等) */
    if (s_cfg.session == 0u) { s_scanSessActive = 1u; return 0; }
    s_scanSessSaved = s_cfg;
    s_cfg.session = 0u;                            /* 仅 RAM, 不落参数区 */
    if (s_powered) {
        /* 模块在上一轮被中止的 0x22 窗口内会短暂拒答 (单发 SET_GEN2CFG
         * 可失败), 无重试时扫描以 S2 开跑 -> 硬标签首读后标志不复位,
         * 匹配去抖饿死 (表现为 CONFIGURED 停 45s 无错误码). 300ms 间隔
         * 重试 3 次覆盖该忙窗. */
        for (uint8_t i = 0u; i < 3u; i++) {
            int r = uhf_apply_config();
            if (r == APP_UHF_ERR_OK) { s_scanSessActive = 2u; return 0; }
            SysTickHl_DelayMs(300u);
        }
        s_cfg = s_scanSessSaved;               /* 重试仍失败: 还原, 窗不开 */
        s_scanSessActive = 0u;
        return APP_UHF_ERR_LINK;
    }
    s_scanSessActive = 2u;
    return 0;
}

void App_UHF_ScanSessionEnd(void)
{
    uint8_t a = s_scanSessActive;
    s_scanSessActive = 0u;
    if (a != 2u) return;
    s_cfg = s_scanSessSaved;
    if (s_powered) (void)uhf_apply_config();       /* 还原用户会话, 尽力而为 */
}
int      App_UHF_IsBusy(void)
{
    return (s_state == APP_UHF_INVENTORY || s_state == APP_UHF_READ ||
            s_state == APP_UHF_WRITE || s_state == APP_UHF_ANT_CHECK ||
            s_state == APP_UHF_GETBUF || s_state == APP_UHF_SCAN) ? 1 : 0;
}

void App_UHF_GetDump(AppUHFDump_t *d)
{
    if (d) *d = s_dump;
}
