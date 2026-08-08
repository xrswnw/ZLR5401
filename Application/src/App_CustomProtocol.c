#include "App_CustomProtocol.h"
#include "App_Param.h"
#include "App_SysTick_HL.h"   /* SysTickHl_GetMs (USB TX 等待)*/
#include "usb_lib.h"           /* ENDP1, GetEPTxStatus*/
#include <stddef.h>

#define RAMCODE_ATTR __attribute__((section(".ramcode"), used, noinline))

/* 协议层帧上下文 (模块私有)*/
typedef struct {
    ProtoParser_t Parser;
    ProtoFrame_t  RxFrame;
    uint8_t       TxBuf[PROTO_MAX_DATA + 11];
} ProtoCtx_t;

static ProtoCtx_t g_sProto;

/* 传输通道注册表: 按 channel 索引, 每通道独立解析器*/
typedef struct {
    const ProtoTransport_t *ops;
    ProtoParser_t           parser;
} ProtoChannel_t;

static ProtoChannel_t g_sChannels[PROTO_CH_MAX];
static uint8_t g_u8DefaultCh = PROTO_CH_USB;  /* 系统仅走 USB HID*/

static ProtoFrameCb_t g_sFrameCb = (ProtoFrameCb_t)0;
static uint8_t g_u8DevAddr = 0x01;

RAMCODE_ATTR void Memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

RAMCODE_ATTR void Memset8(void *dst, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

void Proto_Init(void) {
    Memset8(&g_sProto, 0, sizeof(ProtoCtx_t));
    Memset8(g_sChannels, 0, sizeof(g_sChannels));
    for (uint8_t i = 0; i < PROTO_CH_MAX; i++)
        ProtoParserInit(&g_sChannels[i].parser);
    g_sFrameCb = (ProtoFrameCb_t)0;
    g_u8DefaultCh = PROTO_CH_UART;
}

void Proto_SetDeviceAddr(uint8_t addr) {
    g_u8DevAddr = addr;
}

void Proto_RegisterFrameCb(ProtoFrameCb_t cb) {
    g_sFrameCb = cb;
}

void Proto_RegisterTransport(uint8_t channel, const ProtoTransport_t *t) {
    if (channel == 0 || channel >= PROTO_CH_MAX) return;
    g_sChannels[channel].ops = t;
    ProtoParserInit(&g_sChannels[channel].parser);
}

void Proto_SetDefaultChannel(uint8_t channel) {
    if (channel == 0 || channel >= PROTO_CH_MAX) return;
    g_u8DefaultCh = channel;
}

void ProtoParserInit(ProtoParser_t *p) {
    Memset8(p, 0, sizeof(ProtoParser_t));
    p->state = PROTO_STATE_IDLE;
}

int ProtoParseByte(ProtoParser_t *p, uint8_t byte, ProtoFrame_t *frame) {
    switch (p->state) {
    case PROTO_STATE_IDLE:
        if (byte == 0x53) {                  /* 帧头 0x53 'S'*/
            p->state = PROTO_STATE_HEAD1;
            p->pos = 0;
            p->frameEnd = 0;  /* 复位帧尾指针, 否则沿用上一帧 frameEnd
                               * 导致长度不同的后续帧在错误偏移做 CRC 校验而被丢*/
            p->buf[p->pos++] = byte;
        }
        break;

    case PROTO_STATE_HEAD1:
        if (byte == 0x77) {                  /* 帧头 0x77 'w'*/
            p->state = PROTO_STATE_DEV_ADDR;
            p->buf[p->pos++] = byte;
        } else {
            p->state = PROTO_STATE_IDLE;
        }
        break;

    case PROTO_STATE_DEV_ADDR:
        p->buf[p->pos++] = byte;
        p->state = PROTO_STATE_DEV_RESERVED;
        break;

    case PROTO_STATE_DEV_RESERVED:
        if (byte != 0x00) {            /* reserved 字段固定 0x00, 非法则丢弃*/
            p->state = PROTO_STATE_IDLE;
            break;
        }
        p->buf[p->pos++] = byte;
        p->state = PROTO_STATE_LEN;
        break;

    case PROTO_STATE_LEN:
        p->buf[p->pos++] = byte;
        if (p->pos == 6 && p->frameEnd == 0) {
            p->lenTarget = (uint16_t)p->buf[4] | ((uint16_t)p->buf[5] << 8);
            if (p->lenTarget < PROTO_FRAME_MIN || p->lenTarget > (PROTO_MAX_DATA + 5)) {
                p->state = PROTO_STATE_IDLE;
                break;
            }
            p->frameEnd = 6 + p->lenTarget; /* 6(header2+addr2+len2) + payload*/
        }
        if (p->frameEnd > 4 && p->pos >= p->frameEnd) {
            uint32_t calc = Crc32Calc(p->buf, p->frameEnd - 4);
            uint32_t recv = (uint32_t)p->buf[p->frameEnd - 1] << 24 |
                            (uint32_t)p->buf[p->frameEnd - 2] << 16 |
                            (uint32_t)p->buf[p->frameEnd - 3] << 8  |
                            (uint32_t)p->buf[p->frameEnd - 4];
            if (calc != recv) {
                p->state = PROTO_STATE_IDLE;
                break;
            }
            frame->header   = PROTO_HEADER;
            frame->devAddr   = p->buf[2];
            frame->reserved  = p->buf[3];
            frame->length   = p->lenTarget;
            frame->func     = p->buf[6];
            frame->dataLen = p->lenTarget - 5; /* payload - FC(1) - CRC32(4)*/
            if (frame->dataLen > 0)
                Memcpy(frame->data, &p->buf[7], frame->dataLen);
            frame->crc32 = recv;
            p->state = PROTO_STATE_IDLE;
            return 1;
        }
        break;

    default:
        p->state = PROTO_STATE_IDLE;
        break;
    }
    return 0;
}

int Proto_BuildFrame(uint8_t devAddr, uint8_t func,
                     const uint8_t *data, uint16_t dataLen, uint8_t *out, uint16_t *outLen) {
    if (dataLen > PROTO_MAX_DATA)
        return -1;

    uint16_t payload = 1 + dataLen + 4; /* FC + data + CRC32*/
    uint16_t total = 2 + 2 + 2 + payload;   /* header + devAddr+reserved + len + payload*/

    out[0] = 0x53;                          /* 帧头 'S'*/
    out[1] = 0x77;                          /* 帧头 'w'*/
    out[2] = devAddr;
    out[3] = PROTO_RESERVED;
    out[4] = (uint8_t)(payload);
    out[5] = (uint8_t)(payload >> 8);
    out[6] = func;
    if (dataLen > 0 && data)
        Memcpy(&out[7], data, dataLen);

    uint32_t crc = Crc32Calc(out, total - 4);
    out[total - 4] = (uint8_t)(crc);
    out[total - 3] = (uint8_t)(crc >> 8);
    out[total - 2] = (uint8_t)(crc >> 16);
    out[total - 1] = (uint8_t)(crc >> 24);

    *outLen = total;
    return 0;
}

int Proto_BuildResponse(uint8_t devAddr, uint8_t reqFc, const uint8_t *data,
                        uint16_t dataLen, uint8_t *out, uint16_t *outLen) {
    return Proto_BuildFrame(devAddr, FC_RSP(reqFc), data, dataLen, out, outLen);
}

/* 解析 channel -> 注册表实体; NONE 或未注册则回落到默认通道*/
static const ProtoTransport_t *Proto_GetTransport(uint8_t channel) {
    if (channel == PROTO_CH_NONE)
        channel = g_u8DefaultCh;
    if (channel == 0 || channel >= PROTO_CH_MAX) return (const ProtoTransport_t *)0;
    return g_sChannels[channel].ops;
}

/* 等 EP1 IN 状态回 NAK + App_Usb_TxBusy 清零, 双保险.
 * USB HID 主机 poll 间隔典型 16ms; 50ms 上限避免主循环长时间阻塞.*/
extern uint16_t GetEPTxStatus(uint8_t bEpNum);
extern volatile uint8_t App_Usb_TxBusy;
#define EP_TX_VALID 0x30

static void WaitEp1Ready(void)
{
    uint32_t t0 = SysTickHl_GetMs();
    while (((GetEPTxStatus(ENDP1) & EP_TX_VALID) || App_Usb_TxBusy) &&
           (SysTickHl_GetMs() - t0) < 50) { /* spin*/ }
}

void Proto_TxFrame(uint8_t channel, uint8_t fc, const uint8_t *data, uint16_t len) {
    const ProtoTransport_t *t = Proto_GetTransport(channel);
    if (!t) return;
    uint16_t outLen;
    Proto_BuildFrame(g_u8DevAddr, fc, data, len, g_sProto.TxBuf, &outLen);
    WaitEp1Ready();
    t->TxDma(g_sProto.TxBuf, outLen);
}

void Proto_TxResponse(uint8_t channel, uint8_t reqFc, const uint8_t *data, uint16_t len) {
    const ProtoTransport_t *t = Proto_GetTransport(channel);
    if (!t) return;
    uint16_t outLen;
    Proto_BuildResponse(g_u8DevAddr, reqFc, data, len, g_sProto.TxBuf, &outLen);
    WaitEp1Ready();
    t->TxDma(g_sProto.TxBuf, outLen);
}

void Proto_TxResponseTo(uint8_t channel, uint8_t devAddr, uint8_t reqFc,
                        const uint8_t *data, uint16_t len) {
    const ProtoTransport_t *t = Proto_GetTransport(channel);
    if (!t) return;
    uint16_t outLen;
    Proto_BuildResponse(devAddr, reqFc, data, len, g_sProto.TxBuf, &outLen);
    WaitEp1Ready();
    t->TxDma(g_sProto.TxBuf, outLen);
}

/* 轮询所有已注册通道: 取字节 -> 解析 -> 写入收帧 channel -> 回调应用层*/
void Proto_Poll(void) {
    uint8_t buf[PROTO_MAX_DATA + 11];
    for (uint8_t ch = 1; ch < PROTO_CH_MAX; ch++) {
        ProtoChannel_t *pc = &g_sChannels[ch];
        const ProtoTransport_t *t = pc->ops;
        if (!t) continue;

        uint16_t n = t->RxAvailable();
        if (n == 0) continue;
        if (n > sizeof(buf)) n = sizeof(buf);
        n = t->RxRead(buf, n);
        if (n == 0) continue;

        for (uint16_t i = 0; i < n; i++) {
            if (ProtoParseByte(&pc->parser, buf[i], &g_sProto.RxFrame)) {
                g_sProto.RxFrame.channel = ch;   /* 记录收帧通道, 供 Tx 回路由*/
                /* 地址过滤 (协议层职责): 仅本机或广播帧回调应用层*/
                if (g_sProto.RxFrame.devAddr != g_u8DevAddr &&
                    g_sProto.RxFrame.devAddr != PROTO_DEV_ADDR_BROADCAST) {
                    continue;
                }
                if (g_sFrameCb)
                    g_sFrameCb(&g_sProto.RxFrame);
            }
        }
    }
}

uint32_t Proto_Crc32(const uint8_t *data, uint32_t len) {
    return Crc32Calc(data, len);
}

int Strncmp(const char *s1, const char *s2, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (s1[i] != s2[i])
            return (uint8_t)s1[i] - (uint8_t)s2[i];
        if (s1[i] == '\0')
            return 0;
    }
    return 0;
}

/* Called by App_Usb when HID OUT data is received*/
void Proto_OnDataFromUsb(const uint8_t *data, uint16_t len)
{
    /* Forward data to protocol parser for USB channel*/
    ProtoChannel_t *pc = &g_sChannels[PROTO_CH_USB];
    if (pc->ops == NULL) {
        return;
    }

    for (uint16_t i = 0; i < len; i++) {
        if (ProtoParseByte(&pc->parser, data[i], &g_sProto.RxFrame)) {
            g_sProto.RxFrame.channel = PROTO_CH_USB;
            /* Address filtering*/
            if (g_sProto.RxFrame.devAddr != g_u8DevAddr &&
                g_sProto.RxFrame.devAddr != PROTO_DEV_ADDR_BROADCAST) {
                continue;
            }
            if (g_sFrameCb)
                g_sFrameCb(&g_sProto.RxFrame);
        }
    }
}
