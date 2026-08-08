#ifndef __PARAM_H
#define __PARAM_H

#include <stdint.h>
#include "App_Config.h"

#define PARAM_MAGIC          0x35524C5AU   /* "ZLR5" 小端: Z=0x5A L=0x4C R=0x52 5=0x35 (ZLR54xx 工程) */
#define PARAM_VERSION        1

#define PARAM_STATUS_IDLE    0
#define PARAM_STATUS_RUN     1
#define PARAM_STATUS_UPG     2
#define PARAM_STATUS_FAULT   3

#define PROTO_DEV_ADDR_DEFAULT  0x01U

/* 校验级别 */
#define VERIFY_LEVEL_FULL    0       /* 全: CRC+Size+向量+绑定 */
#define VERIFY_LEVEL_MID     1       /* 中: CRC+Size+向量 */
#define VERIFY_LEVEL_BASIC   2       /* 基本: CRC+Size */

/* FC=0x05 立即校验相关 */
#define VERIFY_CRC_MISMATCH    1
#define VERIFY_SIZE_MISMATCH   2
#define VERIFY_VECTOR_INVALID  3
#define VERIFY_BIND_FAIL       4

/* 参数区总大小: 512B = 主区256B + 影子区256B */
#define PARAM_MAIN_SIZE       256U
#define PARAM_SHADOW_SIZE     256U

typedef struct {
    /* ---- 主区 (256B, 偏移0x00~0xFF) ---- */
    uint32_t magic;                        /* 0x00: 魔数 0x35524C5A ("ZLR5") */
    uint8_t  version;                      /* 0x04: 参数版本 */
    uint8_t  deviceStatus;                 /* 0x05: IDLE/RUN/UPG/FAULT */
    uint8_t  deviceAddr;                   /* 0x06: 设备地址 */
    uint8_t  reserved1;                    /* 0x07: 预留 */
    uint32_t baudRate;                     /* 0x08: 波特率原始值 */
    uint32_t appCrc32;                     /* 0x0C: APP CRC32 */
    uint32_t appSize;                      /* 0x10: APP大小 */
    uint32_t upgradeCount;                 /* 0x14: 升级次数 */
    uint32_t deviceUidHash;               /* 0x18: 设备UID哈希 */
    uint32_t bindVerify;                   /* 0x1C: 绑定校验值 */
    uint32_t mainCrc32;                    /* 0x20: 主区CRC32(覆盖0x00~0xFB) */
    char     hwVersion[VER_STR_LEN];       /* 0x24: 硬件版本 */
    char     swVersion[VER_STR_LEN];      /* 0x34: 软件版本 */
    char     bootVersion[VER_STR_LEN];    /* 0x44: BOOT版本 */
    uint8_t  userParam[172];               /* 0x54: 用户参数区 */

    /* ---- 影子区 (256B, 偏移0x100~0x1FF) ---- */
    uint32_t shadowMagic;                  /* 0x100 */
    uint8_t  shadowVersion;                /* 0x104 */
    uint8_t  shadowDeviceStatus;           /* 0x105 */
    uint8_t  shadowDeviceAddr;             /* 0x106 */
    uint8_t  shadowReserved1;              /* 0x107 */
    uint32_t shadowBaudRate;               /* 0x108 */
    uint32_t shadowAppCrc32;              /* 0x10C */
    uint32_t shadowAppSize;               /* 0x110 */
    uint32_t shadowUpgradeCount;          /* 0x114 */
    uint32_t shadowDeviceUidHash;         /* 0x118 */
    uint32_t shadowBindVerify;            /* 0x11C */
    uint32_t shadowCrc32;                  /* 0x120: 影子区CRC(覆盖0x100~0x11F) */
    char     shadowHwVersion[VER_STR_LEN]; /* 0x124 */
    char     shadowSwVersion[VER_STR_LEN];/* 0x134 */
    char     shadowBootVersion[VER_STR_LEN];/* 0x144 */
    uint8_t  shadowReserved[172];          /* 0x154 */
} DeviceParam_t;

_Static_assert(sizeof(DeviceParam_t) == PARAM_FLASH_SIZE, "DeviceParam_t size must match PARAM_FLASH_SIZE");

void      ParamInit(DeviceParam_t *p);
int       ParamLoad(DeviceParam_t *p);
int       ParamSave(const DeviceParam_t *p);
int       ParamSyncVersion(DeviceParam_t *p);
uint32_t  Crc32Calc(const uint8_t *data, uint32_t len);
uint32_t  Crc32CalcUid(void);
uint32_t  Crc32CalcReflect(const uint8_t *data, uint32_t len, const uint8_t *suffix, uint32_t suffixLen);
void      VerStrCpy(char *dst, const char *src, uint32_t maxLen);

/* Flash 控制器原语见 App_Param_HL.h */

#endif /* __PARAM_H */
