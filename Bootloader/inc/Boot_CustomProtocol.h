#ifndef __BOOT_CUSTOM_PROTOCOL_H
#define __BOOT_CUSTOM_PROTOCOL_H

#include <stdint.h>

/* =====================================================================*/
/* 协议层: 帧定义 / 组帧 / 解析 / CRC*/
/* 与传输媒介解耦 — 经 ProtoTransport_t 接口收发, UART/USB/NET 各自实现*/
/* 并注册; 帧带 channel 字段, 回复按收帧通道路由 (从哪收就从哪回)。*/
/* =====================================================================*/

#define PROTO_HEADER       0x7753U   /* 帧头 "Sw" LE (0x53 'S' + 0x77 'w')*/
#define PROTO_VERSION      3
#define PROTO_MAX_DATA     1024
#define PROTO_FRAME_MIN    5

#define PROTO_DEV_ADDR_BROADCAST  0xFFU
#define PROTO_RESERVED           0x00U

/* ---- 传输通道标识 ----
 * 协议层与传输媒介解耦: 帧经哪个通道收到, 回复就经哪个通道发出
 * (从哪收就从哪回)。后续 USB/NET 各自实现 ProtoTransport_t 并注册即可。*/
#define PROTO_CH_NONE  0x00U   /* 未指定 — Tx 时回落到默认通道*/
#define PROTO_CH_UART  0x01U
#define PROTO_CH_USB   0x02U
#define PROTO_CH_NET   0x03U
#define PROTO_CH_MAX   0x04U   /* 注册表容量 (含 NONE 占位)*/

/* Request function codes*/
#define FC_HANDSHAKE        0x01
#define FC_ENTER_BOOT       0x02
#define FC_UPGRADE_START    0x03
#define FC_FW_DATA          0x04
#define FC_UPGRADE_VERIFY   0x05
#define FC_UPGRADE_EXEC     0x06
#define FC_DEVICE_INFO      0x07
#define FC_RESET            0x08   /* 软件复位 (空参数, 回 OK 后 NVIC_SystemReset;)*/
#define FC_EXIT_BOOT        0x09   /* 退出升级: 校验 App 完好则清 UPG->RUN + 复位跳 App*/

/* Response FC = req_FC ^ 0xFF*/
#define FC_RSP(x)           ((x) ^ 0xFF)

/* Result codes*/
#define RESULT_OK               0

/* FC=0x02 results*/
#define ENTER_BOOT_PARAM_ERR    2

/* FC=0x03 results*/
#define UPG_NO_SPACE            1
#define UPG_ERASE_FAIL          2

/* FC=0x04 results*/
#define DATA_FLASH_FAIL         2
#define DATA_ADDR_ERR           3

/* FC=0x05 results — 三级校验*/
#define VERIFY_CRC_MISMATCH    1
#define VERIFY_SIZE_MISMATCH   2
#define VERIFY_VECTOR_INVALID  3
#define VERIFY_BIND_FAIL       4

/* FC=0x09 (FC_EXIT_BOOT) results*/
#define EXIT_BOOT_NO_APP       1   /* App flash 无有效镜像 (MSP/Reset 向量非法)*/
#define EXIT_BOOT_CRC_MISMATCH  2   /* App CRC 与参数记录不符 (flash 被擦除/损坏)*/

/* Parser states*/
typedef enum {
    PROTO_STATE_IDLE = 0,
    PROTO_STATE_HEAD1,
    PROTO_STATE_DEV_ADDR,
    PROTO_STATE_DEV_RESERVED,
    PROTO_STATE_LEN
} ProtoState_t;

typedef struct {
    uint16_t header;
    uint8_t  devAddr;
    uint8_t  reserved;
    uint16_t length;
    uint8_t  func;
    uint8_t  data[PROTO_MAX_DATA];
    uint16_t dataLen;
    uint32_t crc32;
    uint8_t  channel;   /* 收帧所在通道 (Proto_Poll 写入), Tx 回复时据此路由*/
} ProtoFrame_t;

typedef struct {
    ProtoState_t state;
    uint8_t  buf[PROTO_MAX_DATA + 11];
    uint16_t pos;
    uint16_t lenTarget;
    uint16_t frameEnd;
    uint32_t timeoutMs;
} ProtoParser_t;

/* 帧回调类型: 协议层解析出完整帧后调用应用层*/
typedef void (*ProtoFrameCb_t)(ProtoFrame_t *frame);

/* ---- 传输层接口 (媒介无关) ----
 * UART/USB/NET 各自提供一个实体并通过 Proto_RegisterTransport 注册;
 * 协议层只通过函数指针调用, 不直接依赖任何具体传输。*/
typedef struct {
    uint16_t (*RxAvailable)(void);                      /* 可读字节数, 0=无*/
    uint16_t (*RxRead)(uint8_t *buf, uint16_t maxlen);   /* 拷出并清空, 返回拷出长度*/
    uint8_t  (*TxIsBusy)(void);                          /* 发送忙? 非0=忙*/
    void     (*TxDma)(const uint8_t *buf, uint16_t len); /* 启动发送*/
} ProtoTransport_t;

/* ---- 协议层 API (媒介无关) ----*/
void     Proto_Init(void);
void     Proto_SetDeviceAddr(uint8_t addr);   /* 设置本机地址, 用于收帧过滤*/
void     Proto_RegisterFrameCb(ProtoFrameCb_t cb);

/* 注册/默认传输通道: channel 取 PROTO_CH_UART/USB/NET*/
void     Proto_RegisterTransport(uint8_t channel, const ProtoTransport_t *t);
void     Proto_SetDefaultChannel(uint8_t channel);  /* Tx 通道为 NONE 时回落到此*/

void     ProtoParserInit(ProtoParser_t *p);
int      ProtoParseByte(ProtoParser_t *p, uint8_t byte, ProtoFrame_t *frame);
int      Proto_BuildFrame(uint8_t devAddr, uint8_t func, const uint8_t *data,
                          uint16_t dataLen, uint8_t *out, uint16_t *outLen);
int      Proto_BuildResponse(uint8_t devAddr, uint8_t reqFc, const uint8_t *data,
                             uint16_t dataLen, uint8_t *out, uint16_t *outLen);

/* 用本层帧缓冲组帧并经指定传输通道发出;
 * channel 取 PROTO_CH_UART/USB/NET, 传 PROTO_CH_NONE 则回落到默认通道。
 * 回复通常传收帧时的 frame->channel (从哪收就从哪回)。*/
void     Proto_TxFrame(uint8_t channel, uint8_t fc, const uint8_t *data, uint16_t len);
void     Proto_TxResponse(uint8_t channel, uint8_t reqFc, const uint8_t *data, uint16_t len);
/* 指定目标地址(回复给请求方)*/
void     Proto_TxResponseTo(uint8_t channel, uint8_t devAddr, uint8_t reqFc,
                            const uint8_t *data, uint16_t len);

/* 主循环调: 取传输层字节 -> 解析 -> 回调应用层*/
void     Proto_Poll(void);

uint32_t Proto_Crc32(const uint8_t *data, uint32_t len);

/* ---- 内存/字符串公共工具 (供 Param/Dispatch 复用) ----*/
void      Memcpy(void *dst, const void *src, uint32_t n);
void      Memset8(void *dst, uint8_t val, uint32_t n);
int       Strncmp(const char *s1, const char *s2, uint32_t n);

#endif /* __BOOT_CUSTOM_PROTOCOL_H*/
