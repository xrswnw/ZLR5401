#include "App_UHF.h"
#include "App_UHF_HL.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * UHF 控制面状态机 (SIM7500 / Silion Impinj E710)
 *
 * 控制面 / 数据面解耦:
 *   控制面  = App_UHF:  状态机 (IDLE/READY/INVENTORY/READ/WRITE/ERROR)
 *                       + 配置 (功率/天线/校验) get/set, 上电/下电/停止
 *   数据面  = 标签环形缓冲: App_UHF_TagTake 取走盘点/读取结果
 *   硬件层  = App_UHF_HL: USART1 传输 + 0xBB 帧收发 + CRC16 (由应用驱动)
 *
 * 状态机迁移:
 *   IDLE --Open--> READY --Inventory/Read/Write--> 忙态 --完成或Stop--> READY
 *   忙态 --链路失败--> ERROR --Query/Clear--> READY
 * ===================================================================== */

/* 协议级同步超时 (等待元命令响应) */
#define UHF_CMD_TIMEOUT_MS       500u
/* 操作超时: 盘点/读写没有固定时长, 由 Stop 或空闲检测结束 */
#define UHF_IDLE_DETECT_MS       3000u    /* 无新标签/响应后视为本次操作结束 */

static AppUHFState_t  s_state;
static AppUHFConfig_t s_cfg;
static AppUHFConfig_t s_cfgSaved;        /* 上次成功下发的配置备份 */
static int            s_link;            /* 0=正常 */
static uint8_t        s_powered;
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
    s_totalTags = 0;
    s_tagHead = 0; s_tagTail = 0;
    s_actStartMs = 0; s_lastActivityMs = 0; s_opExpire = 0;
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

/* ---------- 下发电平参数到模块 (功率/天线/会话/Q) ---------- */
static int uhf_apply_config(void)
{
    uint8_t tmp[4];

    /* 功率 */
    tmp[0] = s_cfg.powerDbm;
    UHF_HL_SendFrame(UHF_CMD_SET_POWER, tmp, 1u);
    {
        uint8_t cmd, data[UHF_HL_PAYLOAD_MAX]; uint16_t len;
        if (UHF_HL_RecvFrame(&cmd, data, &len, UHF_CMD_TIMEOUT_MS) != 0)
            return APP_UHF_ERR_LINK;
    }
    /* 天线 */
    tmp[0] = s_cfg.antenna;
    UHF_HL_SendFrame(UHF_CMD_SET_ANTENNA, tmp, 1u);
    {
        uint8_t cmd, data[UHF_HL_PAYLOAD_MAX]; uint16_t len;
        if (UHF_HL_RecvFrame(&cmd, data, &len, UHF_CMD_TIMEOUT_MS) != 0)
            return APP_UHF_ERR_LINK;
    }
    return APP_UHF_ERR_OK;
}

int App_UHF_Open(void)
{
    if (s_powered) return APP_UHF_ERR_OK;
    UHF_HL_SetPowerEn(1u);
    SysTickHl_DelayMs(100u);               /* 模块上电稳定 */
    UHF_HL_RxFlush();
    s_powered = 1;
    int r = uhf_apply_config();
    uhf_state(r == APP_UHF_ERR_OK ? APP_UHF_READY : APP_UHF_ERROR);
    return r;
}

int App_UHF_Close(void)
{
    (void)App_UHF_Stop();
    UHF_HL_SetPowerEn(0u);
    s_powered = 0;
    uhf_state(APP_UHF_IDLE);
    return APP_UHF_ERR_OK;
}

int App_UHF_Stop(void)
{
    UHF_HL_SendFrame(UHF_CMD_STOP, (const uint8_t *)0, 0u);
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
    UHF_HL_SendFrame(UHF_CMD_INVENTORY, (const uint8_t *)0, 0u);
    return APP_UHF_ERR_OK;
}

int App_UHF_ReadTag(const uint8_t *epc, uint8_t epcLen, uint8_t bank,
                    uint8_t addr, uint8_t cnt)
{
    uint8_t p[2 + 16 + 3];
    uint16_t pos = 0;
    p[pos++] = 0x00;                       /* session (UHF_CMD_READ_TAG) */
    if (epcLen > 16u) epcLen = 16u;
    p[pos++] = epcLen;
    for (uint8_t i = 0; i < epcLen; i++) p[pos++] = epc[i];
    p[pos++] = bank;
    p[pos++] = addr;
    p[pos++] = cnt;

    int r = uhf_enter_busy(APP_UHF_READ, 1u);
    if (r != APP_UHF_ERR_OK) return r;
    UHF_HL_SendFrame(UHF_CMD_READ_TAG, p, pos);
    return APP_UHF_ERR_OK;
}

int App_UHF_WriteTag(const uint8_t *epc, uint8_t epcLen, uint8_t bank,
                     uint8_t addr, const uint8_t *data, uint8_t len)
{
    uint8_t p[2 + 16 + 4];
    uint16_t pos = 0;
    p[pos++] = 0x00;
    if (epcLen > 16u) epcLen = 16u;
    p[pos++] = epcLen;
    for (uint8_t i = 0; i < epcLen; i++) p[pos++] = epc[i];
    p[pos++] = bank;
    p[pos++] = addr;
    p[pos++] = len;
    for (uint8_t i = 0; i < len && pos < sizeof(p); i++) p[pos++] = data[i];

    int r = uhf_enter_busy(APP_UHF_WRITE, 1u);
    if (r != APP_UHF_ERR_OK) return r;
    UHF_HL_SendFrame(UHF_CMD_WRITE_TAG, p, pos);
    return APP_UHF_ERR_OK;
}

int App_UHF_Query(void)
{
    UHF_HL_SendFrame(UHF_CMD_QUERY, (const uint8_t *)0, 0u);
    {
        uint8_t cmd, data[UHF_HL_PAYLOAD_MAX]; uint16_t len;
        int r = UHF_HL_RecvFrame(&cmd, data, &len, UHF_CMD_TIMEOUT_MS);
        if (r != 0) { s_link = UHF_HL_LINK_TIMEOUT; uhf_state(APP_UHF_ERROR); return APP_UHF_ERR_LINK; }
        s_link = 0;
        if (s_state == APP_UHF_ERROR) uhf_state(APP_UHF_READY);
        return APP_UHF_ERR_OK;
    }
}

int App_UHF_GetConfig(AppUHFConfig_t *cfg)
{
    if (!cfg) return APP_UHF_ERR_PARAM;
    *cfg = s_cfg;
    return APP_UHF_ERR_OK;
}

int App_UHF_SetConfig(const AppUHFConfig_t *cfg, int save)
{
    (void)save;   /* 持久化由上层 (协议层) 决定 */
    if (!cfg) return APP_UHF_ERR_PARAM;
    if (cfg->powerDbm < 5u || cfg->powerDbm > 30u) return APP_UHF_ERR_PARAM;
    if (cfg->antenna > 1u) return APP_UHF_ERR_PARAM;
    if (cfg->session > 3u) return APP_UHF_ERR_PARAM;
    if (cfg->target > 1u) return APP_UHF_ERR_PARAM;
    if (cfg->q > 15u) return APP_UHF_ERR_PARAM;

    s_cfg = *cfg;
    /* 运行时立即生效 (若已上电) */
    if (s_powered) {
        UHF_HL_SetAntenna(s_cfg.antenna);
        int r = uhf_apply_config();
        if (r != APP_UHF_ERR_OK) { uhf_state(APP_UHF_ERROR); return r; }
        s_cfgSaved = s_cfg;
    }
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

/* ---------- 数据面: 解析模块返回的盘点/读取帧 -------- */
static void uhf_handle_inventory_response(const uint8_t *data, uint16_t len)
{
    /* 常见 EPC 上报帧: [rssi][pc_hi][pc_lo][epc..] 或 [epc..], 尽量兼容.
     * 这里做保守解析: 若能找到 <16B 的 EPC 段则入缓冲; 否则只记 RSSI。 */
    AppUHFTag_t t;
    t.rssi = 0; t.epcLen = 0;

    uint16_t i = 0;
    if (len >= 1u && data[0] <= 100u) { t.rssi = data[0]; i = 1; }  /* 若首字节像 RSSI */

    /* 跳过可能的 PC 2 字节 */
    if (len - i >= 2u) i += 0u;   /* 模式 A: 直接认为余下是 EPC */

    if (len > i) {
        uint8_t n = (uint8_t)(len - i);
        if (n > 16u) n = 16u;
        for (uint8_t k = 0; k < n; k++) t.epc[k] = data[i + k];
        t.epcLen = n;
    }
    uhf_tag_push(&t);
}

static void uhf_decode_incoming(void)
{
    uint8_t cmd, data[UHF_HL_PAYLOAD_MAX];
    uint16_t len;
    while (UHF_HL_RecvFrame(&cmd, data, &len, 0u) == 0) {
        switch (cmd) {
        case UHF_CMD_INVENTORY:
        case UHF_CMD_QUERY:
            uhf_handle_inventory_response(data, len);
            break;
        case UHF_CMD_READ_TAG:
            /* 读到数据也计入标签流 (带数据) */
            uhf_handle_inventory_response(data, len);
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

    /* 若处于忙态且长时间无活动，视为本次操作结束 -> READY */
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
            s_state == APP_UHF_WRITE) ? 1 : 0;
}
