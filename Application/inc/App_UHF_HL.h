#ifndef __APP_UHF_HL_H
#define __APP_UHF_HL_H

#include <stdint.h>

/* =====================================================================
 * UHF 超高频模块硬件抽象层 (SIM7500 / Silion Impinj E710 平台)
 *  - 接口: USART3 (PB10=TX, PB11=RX), 115200-8-N-1 (TTL 电平)
 *  - UHF_EN = PB12 (模块电源使能, 高电平上电)
 *  - 模块 IO: OUT2=PB13, IN1=PB14, IN2=PB15, NRST=PC6, OUT1=PC7
 *  - 板上无 MCU 天线切换脚 (SIM7500 ANT 在模块上), 天线经模块命令
 *  - 模块采用 UHF RFID 通用 0xBB 帧族:
 *      | 0xBB | len | cmd | data... | crc16_lo | crc16_hi |
 *      len = cmd+data 字节数 (不含 0xBB), CRC16 = CCITT (poly 0x1021)
 *  配套实现: App_UHF (状态机/控制面) 与 App_UHF_HL (传输/数据面)
 * ===================================================================== */

/* ---- 帧格式常量 ---- */
#define UHF_HL_FRAME_HDR       0xBBu        /* 帧头 */
#define UHF_HL_FRAME_MAX       (2u + 1u + 1u + 128u + 2u)  /* 头+len+cmd+data+crc16 */
#define UHF_HL_PAYLOAD_MAX     (UHF_HL_FRAME_MAX - 6u)

/* ---- 模块命令字 (E710/Silion 平台通用) ---- */
#define UHF_CMD_INVENTORY      0x22u        /* 单次盘点 (数据含 EPC) */
#define UHF_CMD_READ_TAG       0x26u        /* 读标签数据 */
#define UHF_CMD_WRITE_TAG      0x27u        /* 写标签数据 */
#define UHF_CMD_STOP           0x23u        /* 停止盘点/操作 */
#define UHF_CMD_READ_GEN2      0x2Cu        /* 读 Gen2 存储区 (bank/addr/cnt) */
#define UHF_CMD_WRITE_GEN2     0x2Du        /* 写 Gen2 存储区 */
#define UHF_CMD_QUERY          0x01u        /* 查询状态/版本 */
#define UHF_CMD_SET_POWER      0x24u        /* 设置发射功率 (dBm) */
#define UHF_CMD_GET_POWER      0x25u        /* 读取当前功率 */
#define UHF_CMD_SET_ANTENNA    0x2Au        /* 设置天线 */
#define UHF_CMD_GET_ANTENNA    0x2Bu        /* 读取天线 */

/* ---- 健康状态自检结果 (App_UHF 通过 App_UHF_HL 探测) ---- */
#define UHF_HL_LINK_OK         0u
#define UHF_HL_LINK_TIMEOUT    1u
#define UHF_HL_LINK_BAD_CRC    2u

/* ---- 接口 (硬件层) ---- */
void     UHF_HL_Init(void);                         /* USART3 + 引脚初始化 */
void     UHF_HL_DeInit(void);

/* 电源 / 天线 硬件控制 */
void     UHF_HL_SetPowerEn(uint8_t en);             /* 1=上电 */
void     UHF_HL_SetAntenna(uint8_t idx);            /* 无天线脚, 经模块命令 (保留接口为无操作) */

/* 模块帧传输 */
void     UHF_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint16_t len);
int      UHF_HL_RecvFrame(uint8_t *cmd, uint8_t *data, uint16_t *len,
                          uint32_t timeoutMs);      /* 0=OK, 负=超时/CRC错 */
uint16_t UHF_HL_Crc16(const uint8_t *data, uint16_t len);    /* CCITT, init 0xFFFF */

/* 数据平面字节流 (供状态机解析标签流) */
uint16_t UHF_HL_RxAvailable(void);
uint16_t UHF_HL_RxRead(uint8_t *buf, uint16_t maxlen);
void     UHF_HL_RxFlush(void);

/* 阻塞发送完成等待 (简单轮询, 供控制面同步使用) */
int      UHF_HL_WaitTxIdle(uint32_t timeoutMs);

#endif /* __APP_UHF_HL_H */
