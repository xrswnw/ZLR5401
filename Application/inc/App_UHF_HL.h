#ifndef __APP_UHF_HL_H
#define __APP_UHF_HL_H

#include <stdint.h>

/* =====================================================================
 * UHF 超高频模块硬件抽象层 (SIM7500 / EX10 平台)
 *  - 接口: UART4 (PC10=TX, PC11=RX), 115200-8-N-1 (TTL 电平)
 *  - UHF_EN = PB12 (模块电源使能, 高电平上电)
 *  - 模块 IO: OUT2=PB13, IN1=PB14, IN2=PB15, NRST=PC6, OUT1=PC7
 *  - 协议: EX10 通用指令帧 (见《EX10系列模块通信协议》):
 *      | 0xFF | len | cmd | status(2,仅响应) | data... | crc16_hi | crc16_lo |
 *    len = data 字段字节数 (响应时 status 计入 data 前, 总长 = len + 7)
 *    CRC16 覆盖 0xFF 之后的所有字节 (附录5, init 0xFFFF, poly 0x1021)
 * 配套实现: App_UHF (状态机/控制面) 与 App_UHF_HL (传输/数据面)
 * ===================================================================== */

/* ---- 帧格式常量 ---- */
#define UHF_HL_FRAME_HDR       0xFFu        /* 帧头 (EX10 通用指令) */
#define UHF_HL_DATA_MAX        255u
#define UHF_HL_FRAME_MAX       (1u+1u+1u+2u+UHF_HL_DATA_MAX+2u)  /* 262 */

/* ---- 健康状态自检结果 (App_UHF 通过 App_UHF_HL 探测) ---- */
#define UHF_HL_LINK_OK         0u
#define UHF_HL_LINK_TIMEOUT    1u
#define UHF_HL_LINK_BAD_CRC    2u

/* ---- 接口 (硬件层) ---- */
void     UHF_HL_Init(void);                         /* UART4 + 引脚初始化 */
void     UHF_HL_DeInit(void);

/* 电源 / 天线 硬件控制 */
void     UHF_HL_SetPowerEn(uint8_t en);             /* 1=上电 */
void     UHF_HL_SetAntenna(uint8_t idx);            /* 无天线脚, 经模块命令 (保留接口为无操作) */

/* ---- 模块帧传输 ---- */
/* 发送一帧 (不等待响应): FF len cmd data crc */
void     UHF_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint16_t len);

/* 发送 + 等待同一 cmd 的响应帧, 解析状态码与数据.
 * 返回值: 0=OK (rxStatus 为模块状态码, 0=成功, 非0=模块错误码);
 *         UHF_HL_LINK_TIMEOUT / UHF_HL_LINK_BAD_CRC */
int      UHF_HL_Transact(uint8_t cmd, const uint8_t *tx, uint16_t txLen,
                         uint16_t *rxStatus, uint8_t *rx, uint16_t *rxLen,
                         uint32_t timeoutMs);

/* ---- 扩展指令传输 (Command Code=0xAA, 见协议 3.2) ----
 * 发送 Data: Moduletech(10) | subCmd(2 高在前) | data | SubCRC(1) | 0xBB
 *   SubCRC = (subCmd 高字节起至 data 末尾的累加) & 0xFF (附录7)
 * 响应 Data: Moduletech(10) | subCmd(2) | subData(N)            (无 SubCRC/0xBB)
 * 整帧 CRC16 覆盖 0xFF 之后的全部字节 (SubCRC/0xBB 计入)。 */
#define UHF_HL_EXT_MARKER_LEN   10u
#define UHF_HL_EXT_SUB_LEN      2u
void     UHF_HL_SendExt(uint16_t subCmd, const uint8_t *data, uint16_t len);

/* 发送扩展指令并等待响应, 校验 Moduletech marker 与 subCmd 匹配归一,
 * 解出 subData (不含 marker/subCmd). 0=OK, 其余同 UHF_HL_Transact */
int      UHF_HL_TransactExt(uint16_t subCmd, const uint8_t *tx, uint16_t txLen,
                            uint16_t *rxStatus, uint8_t *subData, uint16_t *subDataLen,
                            uint32_t timeoutMs);

/* 从接收环取一帧响应的底层封装 (供异步/流式使用) */
int      UHF_HL_RecvFrame(uint8_t *cmd, uint16_t *status,
                          uint8_t *data, uint16_t *len, uint32_t timeoutMs);

/* CRC16 (附录5: init 0xFFFF, poly 0x1021, MSB first; 覆盖 0xFF 之后字节) */
uint16_t UHF_HL_Crc16(const uint8_t *data, uint16_t len, uint16_t init);

/* 数据平面字节流 (供状态机解析标签流) */
uint16_t UHF_HL_RxAvailable(void);
uint16_t UHF_HL_RxRead(uint8_t *buf, uint16_t maxlen);
void     UHF_HL_RxFlush(void);

/* 阻塞发送完成等待 (简单轮询, 供控制面同步使用) */
int      UHF_HL_WaitTxIdle(uint32_t timeoutMs);

#endif /* __APP_UHF_HL_H */
