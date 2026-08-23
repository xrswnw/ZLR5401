#include "App_Param.h"
#include "App_CustomProtocol.h"
#include "App_Param_HL.h"

#define UID_ADDR  0x1FFFF7E8U
#define RAMCODE __attribute__((section(".ramcode"), used, noinline))

uint32_t RAMCODE Crc32Calc(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }
    return crc;
}

uint32_t RAMCODE Crc32CalcUid(void) {
    return Crc32Calc((const uint8_t *)UID_ADDR, 12);
}

uint32_t RAMCODE Crc32CalcReflect(const uint8_t *data, uint32_t len,
                                    const uint8_t *suffix, uint32_t suffixLen) {
    /* 计算 data + suffix 的CRC32, 用于BindVerify*/
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }
    for (uint32_t i = 0; i < suffixLen; i++) {
        crc ^= (uint32_t)suffix[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }
    return crc;
}

void VerStrCpy(char *dst, const char *src, uint32_t maxLen) {
    uint32_t i;
    for (i = 0; i < maxLen; i++) {
        if (src[i] == '\0') break;
        dst[i] = src[i];
    }
    for (; i < maxLen; i++)
        dst[i] = '\0';
}

void ParamInit(DeviceParam_t *p) {
    p->magic           = PARAM_MAGIC;
    p->version         = PARAM_VERSION;
    p->deviceStatus    = PARAM_STATUS_IDLE;
    p->deviceAddr      = PROTO_DEV_ADDR_DEFAULT;
    p->baudRate        = 115200U;
    p->appCrc32        = 0U;
    p->appSize         = 0U;
    p->upgradeCount    = 0U;
    p->deviceUidHash   = 0U;
    p->bindVerify      = 0U;
    VerStrCpy(p->hwVersion,   DEV_HW_VERSION,   VER_STR_LEN);
    VerStrCpy(p->swVersion,   DEV_SW_VERSION,   VER_STR_LEN);
    VerStrCpy(p->bootVersion, DEV_BOOT_VERSION, VER_STR_LEN);
}

int ParamLoad(DeviceParam_t *p) {
    Memcpy(p, (const void *)PARAM_FLASH_ORIGIN, sizeof(DeviceParam_t));

    /* 主区CRC校验 — 覆盖0x00~0xFB. mainCrc32 位于偏移0x20(在覆盖范围内),
     * 计算前先清零该字段再算, 与ParamSave同一约定, 使CRC不含自身.
     * 注: 注释原写"mainCrc32位于0xFC"为设计意图, 实际结构体该字段在0x20,
     * 落入自身覆盖范围 -> 原实现主区CRC必校验不过, 双副本退化为单副本.*/
    uint32_t savedCrc = p->mainCrc32;
    p->mainCrc32 = 0;
    uint32_t crc = Crc32Calc((const uint8_t *)p, PARAM_MAIN_SIZE - 4);
    p->mainCrc32 = savedCrc;   /* 还原, 供后续影子恢复/默认回写使用原值*/
    if (p->magic == PARAM_MAGIC && crc == savedCrc)
        return 0;

    /* 主区失败 → 影子区恢复*/
    uint32_t shadowCrc = Crc32Calc((const uint8_t *)PARAM_FLASH_ORIGIN + PARAM_MAIN_SIZE,
                                    32);  /* 影子区0x100~0x11F = 32B*/
    if (p->shadowMagic == PARAM_MAGIC && shadowCrc == p->shadowCrc32) {
        p->magic          = p->shadowMagic;
        p->version        = p->shadowVersion;
        p->deviceStatus   = p->shadowDeviceStatus;
        p->deviceAddr     = p->shadowDeviceAddr;
        p->baudRate       = p->shadowBaudRate;
        p->appCrc32       = p->shadowAppCrc32;
        p->appSize        = p->shadowAppSize;
        p->upgradeCount   = p->shadowUpgradeCount;
        p->deviceUidHash  = p->shadowDeviceUidHash;
        p->bindVerify     = p->shadowBindVerify;
        Memcpy(p->hwVersion, p->shadowHwVersion, VER_STR_LEN);
        Memcpy(p->swVersion, p->shadowSwVersion, VER_STR_LEN);
        Memcpy(p->bootVersion, p->shadowBootVersion, VER_STR_LEN);
        return 0;
    }

    /* 主区与影子区都损坏 → 使用默认值并写回 flash (下次启动正常)*/
    ParamInit(p);
    (void)ParamSave(p);
    return 0;
}

int ParamSyncVersion(DeviceParam_t *p) {
    int changed = 0;
    if (Strncmp(p->hwVersion, DEV_HW_VERSION, VER_STR_LEN) != 0) {
        VerStrCpy(p->hwVersion, DEV_HW_VERSION, VER_STR_LEN);
        changed = 1;
    }
    if (Strncmp(p->swVersion, DEV_SW_VERSION, VER_STR_LEN) != 0) {
        VerStrCpy(p->swVersion, DEV_SW_VERSION, VER_STR_LEN);
        changed = 1;
    }
    if (Strncmp(p->bootVersion, DEV_BOOT_VERSION, VER_STR_LEN) != 0) {
        VerStrCpy(p->bootVersion, DEV_BOOT_VERSION, VER_STR_LEN);
        changed = 1;
    }
    if (changed)
        return ParamSave(p);
    return 0;
}

int RAMCODE ParamSave(const DeviceParam_t *p) {
    DeviceParam_t tmp;
    Memcpy(&tmp, p, sizeof(DeviceParam_t));

    /* 填充主区CRC — 覆盖0x00~0xFB. mainCrc32位于偏移0x20(在覆盖范围内),
     * 先清零该字段再算, 与ParamLoad同一约定, 使CRC不含自身.
     * 否则保存时算出的CRC(含旧mainCrc32)与加载时算出的CRC(含已存值)必不相等,
     * 主区永远校验不过 -> 双副本退化为单副本, 抗掉电能力减半.*/
    tmp.mainCrc32 = 0;
    tmp.mainCrc32 = Crc32Calc((const uint8_t *)&tmp,
                               PARAM_MAIN_SIZE - 4);

    /* 填充影子区*/
    tmp.shadowMagic          = tmp.magic;
    tmp.shadowVersion        = tmp.version;
    tmp.shadowDeviceStatus   = tmp.deviceStatus;
    tmp.shadowDeviceAddr     = tmp.deviceAddr;
    tmp.shadowBaudRate       = tmp.baudRate;
    tmp.shadowAppCrc32       = tmp.appCrc32;
    tmp.shadowAppSize        = tmp.appSize;
    tmp.shadowUpgradeCount   = tmp.upgradeCount;
    tmp.shadowDeviceUidHash  = tmp.deviceUidHash;
    tmp.shadowBindVerify     = tmp.bindVerify;
    Memcpy(tmp.shadowHwVersion, tmp.hwVersion, VER_STR_LEN);
    Memcpy(tmp.shadowSwVersion, tmp.swVersion, VER_STR_LEN);
    Memcpy(tmp.shadowBootVersion, tmp.bootVersion, VER_STR_LEN);
    tmp.shadowCrc32 = Crc32Calc((const uint8_t *)&tmp + PARAM_MAIN_SIZE, 32);

    FlashHl_SaveBegin();

    /* 注意: STM32F10x_hd 页大小2KB, 参数区512B仅占页的1/4
 整页擦除后需重写全部内容*/
    if (FlashHl_ErasePage(PARAM_FLASH_ORIGIN) != 0) {
        FlashHl_SaveFinish();
        return -1;
    }

    /* Phase 1: 写影子区(0x100~0x1FF) — 先写, 掉电时主区损坏但有影子可恢复*/
    const uint32_t *src = (const uint32_t *)&tmp;
    for (int i = (PARAM_MAIN_SIZE / 4); i < (int)(sizeof(DeviceParam_t) / 4); i++) {
        if (FlashHl_WriteWord(PARAM_FLASH_ORIGIN + i * 4, src[i]) != 0) {
            FlashHl_SaveFinish();
            return -2;
        }
    }

    /* Phase 2: 写主区(0x00~0xFF)*/
    for (int i = 0; i < (int)(PARAM_MAIN_SIZE / 4); i++) {
        if (FlashHl_WriteWord(PARAM_FLASH_ORIGIN + i * 4, src[i]) != 0) {
            FlashHl_SaveFinish();
            return -2;
        }
    }

    FlashHl_SaveFinish();
    return 0;
}

/* ---------- UHF 配置持久化 (userParam 区) ---------- */
void UhfParam_Default(UHFUserCfg_t *cfg)
{
    if (!cfg) return;
    cfg->powerDbm   = 20u;
    cfg->antenna    = 0u;
    cfg->checksumEn = 1u;
    cfg->session    = 0u;
    cfg->target     = 0u;
    cfg->q          = 0u;
    cfg->band       = 0x01u;   /* 北美 (默认工作频段) */
}

int UhfParam_Load(UHFUserCfg_t *cfg)
{
    extern DeviceParam_t g_sParam;   /* App_Dispatch.c */
    const uint8_t *u = g_sParam.userParam;
    if (u[1] == UHF_PARM_MAGIC_HI && u[0] == UHF_PARM_MAGIC_LO &&
        (u[2] & 0x0F) == (UHF_PARM_VERSION & 0x0F)) {
        if (!cfg) return 0;
        cfg->powerDbm   = u[3];
        cfg->antenna    = u[4];
        cfg->checksumEn = u[5];
        cfg->session    = u[6];
        cfg->target     = u[7];
        cfg->q          = u[8];
        cfg->band       = u[9];   /* 追加字段; 旧记录未写入时为 0xFF, 归一为默认 */
        if (cfg->band == 0u || cfg->band == 0xFFu) cfg->band = 0x01u;
        return 0;
    }
    return -1;
}

int UhfParam_Save(const UHFUserCfg_t *cfg)
{
    if (!cfg) return -1;
    extern DeviceParam_t g_sParam;   /* App_Dispatch.c */
    uint8_t *u = g_sParam.userParam;
    u[0] = UHF_PARM_MAGIC_LO;
    u[1] = UHF_PARM_MAGIC_HI;
    u[2] = UHF_PARM_VERSION;
    u[3] = cfg->powerDbm;
    u[4] = cfg->antenna;
    u[5] = cfg->checksumEn;
    u[6] = cfg->session;
    u[7] = cfg->target;
    u[8] = cfg->q;
    u[9] = cfg->band;
    return ParamSave(&g_sParam);
}

/* ---------- AM 解码器配置持久化 (userParam 区, 偏移 16) ---------- */
void AmParam_Default(AMUserCfg_t *cfg)
{
    if (!cfg) return;
    cfg->threshold   = 5u;
    cfg->hitCount    = 6u;
    cfg->freqRange   = 1u;
    cfg->recvDelay   = 0u;
    cfg->recvLength  = 0u;
    cfg->phaseInvert = 0u;
    cfg->phaseSync   = 0u;
    cfg->decodeVolt  = 1u;
    cfg->mode        = 0u;
}

int AmParam_Load(AMUserCfg_t *cfg)
{
    extern DeviceParam_t g_sParam;   /* App_Dispatch.c */
    const uint8_t *u = g_sParam.userParam + AM_PARM_OFFSET;
    if (u[1] == AM_PARM_MAGIC_HI && u[0] == AM_PARM_MAGIC_LO &&
        u[2] == AM_PARM_VERSION) {
        if (!cfg) return 0;
        cfg->threshold   = (uint16_t)(((uint16_t)u[3] << 8) | u[4]);
        cfg->hitCount    = (uint16_t)(((uint16_t)u[5] << 8) | u[6]);
        cfg->freqRange   = u[7];
        cfg->recvDelay   = (uint16_t)(((uint16_t)u[8] << 8) | u[9]);
        cfg->recvLength  = u[10];
        cfg->phaseInvert = u[11];
        cfg->phaseSync   = (uint16_t)(((uint16_t)u[12] << 8) | u[13]);
        cfg->decodeVolt  = u[14];
        cfg->mode        = u[15];
        return 0;
    }
    return -1;
}

int AmParam_Save(const AMUserCfg_t *cfg)
{
    if (!cfg) return -1;
    extern DeviceParam_t g_sParam;   /* App_Dispatch.c */
    uint8_t *u = g_sParam.userParam + AM_PARM_OFFSET;
    u[0]  = AM_PARM_MAGIC_LO;
    u[1]  = AM_PARM_MAGIC_HI;
    u[2]  = AM_PARM_VERSION;
    u[3]  = (uint8_t)((cfg->threshold >> 8) & 0xFF);
    u[4]  = (uint8_t)(cfg->threshold & 0xFF);
    u[5]  = (uint8_t)((cfg->hitCount >> 8) & 0xFF);
    u[6]  = (uint8_t)(cfg->hitCount & 0xFF);
    u[7]  = cfg->freqRange;
    u[8]  = (uint8_t)((cfg->recvDelay >> 8) & 0xFF);
    u[9]  = (uint8_t)(cfg->recvDelay & 0xFF);
    u[10] = cfg->recvLength;
    u[11] = cfg->phaseInvert;
    u[12] = (uint8_t)((cfg->phaseSync >> 8) & 0xFF);
    u[13] = (uint8_t)(cfg->phaseSync & 0xFF);
    u[14] = cfg->decodeVolt;
    u[15] = cfg->mode;
    return ParamSave(&g_sParam);
}
