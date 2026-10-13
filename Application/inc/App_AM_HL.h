#ifndef __APP_AM_HL_H
#define __APP_AM_HL_H

#include <stdint.h>

/* =====================================================================
 * 解码器硬件抽象层 — RS232(USART1) + 帧收发 (Round_021 双模参数化)
 *  - 接口: USART1 (PA9=TX / PA10=RX), 115200-8-N-1, 全双工 RS232 (无方向控制脚)
 *  - 帧格式 (AM/RF 同构, 仅帧头两字节不同):
 *      | H1 | H2 | cmd | 包数 | 包次 | 包长 | data... | 校验 |
 *      校验 = (cmd + 包数 + 包次 + 包长 + data...) & 0xFF
 *     AM: 头 2A A2 / RF: 头 B5 A1 — 协议文档自述两族校验同为
 *     "除帧头与校验外累加和取低位" (RF Sum_L 待样机实测核对, 算法已
 *     参数化, 不一致时仅需换 HL 内核函数)
 *     Data0 为数据高位, Data1 为低位
 *  - Round_021: 帧头常量改运行时注入 (AM_HL_SetHeader), 由后端在
 *    切换时配置; 未注入时缺省 AM (2A A2), 兼容上电即 AM 的旧行为。
 *  配套实现: App_AM (状态机/控制面/配置) 与各后端 (App_AM_Backend / RF)
 * ===================================================================== */

#define AM_HL_FRAME_HDR1      0x2Au    /* AM 缺省帧头 (RF 后端注入 B5 A1) */
#define AM_HL_FRAME_HDR2      0xA2u
#define AM_HL_FRAME_MAX       (2u + 1u + 1u + 1u + 1u + 40u + 1u)  /* 头+cmd+包数+包次+包长+data+校验;
                                                                   * data 容量 40B (RF 0x01 总查询单帧:
                                                                   * 2 版本+5 ID+4 报警次数+28 参数, 计 39B) */
#define AM_HL_PAYLOAD_MAX     40u

/* 波形帧 (AM cmd 0x64): 4 包 x 100 点 + EF FE 收尾, 无常规校验位 (AM 专属, RF 无此功能) */
#define AM_HL_WAVE_PAYLOAD    100u
#define AM_HL_WAVE_PKT        (6u + 100u + 2u)                     /* 头+cmd+包数+包次+包长 + 100 + EF FE */
#define AM_HL_WAVE_TRAILER_LO 0xEFu
#define AM_HL_WAVE_TRAILER_HI 0xFEu

/* 健康状态 */
#define AM_HL_LINK_OK         0u
#define AM_HL_LINK_TIMEOUT    1u
#define AM_HL_LINK_BAD_CRC    2u

/* ---- AM 解码器命令字 (协议参数命令表) ---- */
#define AM_CMD_MODE           0x50u   /* 工作模式 0-2: 0检测消磁/1仅检测/2待机 */
#define AM_CMD_THRESHOLD      0x11u   /* 接收阀值 0-30 */
#define AM_CMD_HIT_COUNT      0x12u   /* 命中次数 3-8 */
#define AM_CMD_FREQ_RANGE     0x13u   /* 频率范围 0宽/1中/2窄 */
#define AM_CMD_RECV_DELAY     0x14u   /* 接收延迟 0-50 (掩藏参数) */
#define AM_CMD_RECV_LENGTH    0x53u   /* 接收长短 0长/1短 */
#define AM_CMD_PHASE_INVERT   0x51u   /* 零火翻转 0否/1是 */
#define AM_CMD_PHASE_SYNC     0x52u   /* 相位同步 0-2000 */
#define AM_CMD_DECODE_VOLT    0x67u   /* 解码电压 0低/1中/2高 */
#define AM_CMD_MAINS_FREQ     0x5Du   /* 市电频率 0:50Hz/1:60Hz */
#define AM_CMD_HEARTBEAT      0x04u   /* 心跳 (30分钟一次) */
#define AM_CMD_ATAG_REPORT    0x17u   /* 标签检测主动上报 (~1Hz, 设备->本机) */
#define AM_CMD_WAVE           0x64u   /* 波形 (请求-响应, 4包 x 100点 + EF FE) */
#define AM_CMD_QUERY_ALL      0x63u   /* 总查询 (真读回全部参数) */

/* ---- 接口 (硬件层) ---- */
void     AM_HL_Init(void);
void     AM_HL_DeInit(void);

/* Round_021: 帧头注入 (后端切换时配置; 接收帧定位与发送组帧同步生效) */
void     AM_HL_SetHeader(uint8_t h1, uint8_t h2);

/* 帧传输: SendFrame 自动组帧 (当前注入帧头) + 校验; RecvFrame 解析/校验收到的帧 */
void     AM_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t datalen);
int      AM_HL_RecvFrame(uint8_t *cmd, uint8_t *data, uint8_t *len,
                         uint32_t timeoutMs);   /* 0=OK, 负=超时/校验错 */
int      AM_HL_TryRecvFrame(uint8_t *cmd, uint8_t *data, uint8_t *len); /* 0=OK, -1=暂无完整帧 */

/* 波形专用接收: 收一个 0x64 波形包 (100 点, EF FE 收尾, 无校验), pkt 存 100 点, idx=包次.
 * 0=OK, 负=超时/无效包(丢弃继续等下一有效包). 仅 0x64 采集用. */
int      AM_HL_RecvWavePkt(uint8_t pkt[AM_HL_WAVE_PAYLOAD], uint8_t *idx, uint32_t timeoutMs);

/* 数据平面字节流 */
uint16_t AM_HL_RxAvailable(void);
uint16_t AM_HL_RxRead(uint8_t *buf, uint16_t maxlen);
void     AM_HL_RxFlush(void);

#endif /* __APP_AM_HL_H */
