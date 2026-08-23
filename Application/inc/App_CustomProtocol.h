#ifndef __APP_CUSTOM_PROTOCOL_H
#define __APP_CUSTOM_PROTOCOL_H

#include <stdint.h>

/* =====================================================================*/
/* 协议层: 帧定义 / 组帧 / 解析 / CRC*/
/* 与传输媒介解耦 — 经 ProtoTransport_t 接口收发, UART/USB/NET 各自实现*/
/* 并注册; 帧带 channel 字段, 回复按收帧通道路由 (从哪收就从哪回)。*/
/* =====================================================================*/

#define PROTO_HEADER       0x7753U   /* 帧头 "Sw" LE (0x53 'S' + 0x77 'w')*/
#define PROTO_VERSION      2
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
#define FC_EXIT_BOOT        0x09   /* 退出升级 (Boot 处理): 校验 App 完好则清 UPG->RUN + 复位跳 App*/
#define FC_MOTOR_CTRL       0x0A   /* 步进电机 DRV8434S 控制 (子命令编码, 见下)*/
#define FC_UHF_CTRL         0x0B   /* UHF 超高频 SIM7500 模块控制 (子命令编码, 见下)*/
#define FC_AM_CTRL          0x0C   /* AM 解码器控制 (子命令编码, 见下)*/
#define FC_LOCKER_CTRL      0x0D   /* 开锁器业务编排 (子命令编码, 见下)*/
#define FC_RGB_CTRL         0x0E   /* RGB 三色灯控制 (见下)*/

/* ---- FC_RGB_CTRL (0x0E) — RGB 三色灯控制 ----
 * data: [mask, reserved]   mask=颜色位掩码(bit0=G,bit1=R,bit2=B, 其余预留), reserved=预留字节.
 * 响应: data[0]=mask(回显), data[1]=err(0=OK). err 值见 RGB_ERR_*. */
#define RGB_CMD_SET          0x01   /* data: [cmd,mask,reserved] */
#define RGB_ERR_OK           0
#define RGB_ERR_PARAM        1

/* ---- FC_LOCKER_CTRL (0x0D) 子命令编码 (data[0]) ----
 * 开锁器业务编排状态机 (依《约束/Link.txt》).
 * 所有响应同步: data[0]=cmd, data[1]=err(0=OK). err 值见 LOCKER_ERR_*.
 * 注: 一帧装不下的多硬标签, 可先 LOCKER_SUB_ADD 逐条追加再 START. */
#define LOCKER_SUB_CONFIGURE  0x01   /* data: [cmd,hardCountL,hardCountH,softCountL,softCountH]
                                        (v1 精简: 经 ADD 建清单, 此处仅设软标数/清零) */
#define LOCKER_SUB_ADD        0x02   /* data: [cmd,epcLen,epc..]  追加一条硬标签 EPC */
#define LOCKER_SUB_START      0x03   /* data: [cmd]  上电UHF+开扫, 进入可开锁 */
#define LOCKER_SUB_CANCEL     0x04   /* data: [cmd]  取消, 磁块回降回 IDLE */
#define LOCKER_SUB_QUERY      0x05   /* data: [cmd]  查询状态/计数: [cmd,err,state,hm,sc,su] */
#define LOCKER_SUB_CONSUME_SOFT 0x06 /* data: [cmd]  v1 软标: 上位机上报已解码一次 */
#define LOCKER_SUB_GET_EVENT  0x07   /* data: [cmd]  取一条上报事件 (见 AppLockerEvent_t) */
/* 逻辑错误码 (data[1]) */
#define LOCKER_ERR_OK         0
#define LOCKER_ERR_BUSY       1     /* 非空闲, 需先取消 */
#define LOCKER_ERR_PARAM      2     /* 参数非法/超上限 */
#define LOCKER_ERR_NO_EVENT   3     /* 无待取事件 */

/* ---- FC_MOTOR_CTRL (0x0A) 子命令编码 (data[0]) ----
 * data: [0]=cmd, 后续参数随 cmd 而定. 响应 data[0]=cmd, data[1]=err(0=OK), 其余随 cmd. */
#define MOTOR_CMD_MOVE      0x01   /* data: [cmd,dir,stepsL,stepsM,stepsH]  dir=0/1, steps=24bit. 启动运动 */
#define MOTOR_CMD_STOP      0x02   /* 停止并关断输出 */
#define MOTOR_CMD_SPEED     0x03   /* data: [cmd,hzL,hzH]  设定微步/秒 (1~2000) */
#define MOTOR_CMD_TORQUE    0x04   /* data: [cmd,pct]  转矩百分比 (6~100) */
#define MOTOR_CMD_QUERY     0x05   /* 查询: 状态/故障/步数 */
#define MOTOR_CMD_CLEAR     0x06   /* 清除故障 */
/* 运行错误码 */
#define MOTOR_ERR_OK            0
#define MOTOR_ERR_PARAM         1
#define MOTOR_ERR_FAULT         2

/* ---- FC_UHF_CTRL (0x0B) 子命令编码 (data[0]) ----
 * data: [0]=cmd, 后续参数随 cmd 而定. 响应 data[0]=cmd, data[1]=err(0=OK), 其余随 cmd. */
#define UHF_SUB_OPEN           0x01   /* data: [cmd]  上电 + 配置下发, 进入 READY */
#define UHF_SUB_CLOSE          0x02   /* data: [cmd]  停止并下电 */
#define UHF_SUB_INVENTORY      0x03   /* data: [cmd]  发起一次盘点 */
#define UHF_SUB_READ_TAG       0x04   /* data: [cmd,epcLen,epc..,bank,addr,cnt]  读标签 */
#define UHF_SUB_WRITE_TAG      0x05   /* data: [cmd,epcLen,epc..,bank,addr,len,data..]  写标签 */
#define UHF_SUB_STOP           0x06   /* data: [cmd]  停止当前操作 */
#define UHF_SUB_QUERY          0x07   /* data: [cmd]  查询链路/状态 */
#define UHF_SUB_GET_CONFIG     0x08   /* data: [cmd]  读取当前配置 (含 band) */
#define UHF_SUB_SET_CONFIG     0x09   /* data: [cmd,powerDbm,antenna,checksumEn,session,target,q,(band)]  设置配置 */
#define UHF_SUB_GET_TAGS       0x0A   /* data: [cmd,(count)]  count=0 取全部; >0 取前 count 条 */
#define UHF_SUB_GET_STATUS     0x0B   /* data: [cmd]  读取状态/错误监控 */
#define UHF_SUB_CHECK_ANT      0x0C   /* data: [cmd]  主动触发回波检测 */
/* 运行错误码 (data[1]) */
#define UHF_ERR_OK             0
#define UHF_ERR_PARAM          1
#define UHF_ERR_BUSY           2
#define UHF_ERR_NOT_READY      3
#define UHF_ERR_LINK           4
#define UHF_ERR_NO_TAG         5
#define UHF_ERR_TIMEOUT        6

/* ---- FC_AM_CTRL (0x0C) 子命令编码 (data[0]) ----
 * AM 解码器经 USART2 (2A A2 帧) 通信. 响应 data[0]=cmd, data[1]=err(0=OK), 其余随 cmd.
 * 值用 16bit (高字节在后) 表示: (dataH<<8)|dataL. */
#define AM_SUB_GET_CONFIG   0x01   /* data: [cmd]  读取当前配置 */
#define AM_SUB_SET_CONFIG   0x02   /* data: [cmd, thrH,thrL, hitH,hitL, freq, delayH,delayL,
                                               len, invert, syncH,syncL, volt, mode]  设置配置 */
#define AM_SUB_GET_PARAM    0x03   /* data: [cmd, amCmd]  读单个参数 (amCmd 为 AM 命令字) */
#define AM_SUB_SET_PARAM    0x04   /* data: [cmd, amCmd, valH, valL]  写单个参数 */
#define AM_SUB_QUERY        0x05   /* data: [cmd]  总查询, 探测链路 */
/* 运行错误码 (data[1]) */
#define AM_ERR_OK           0
#define AM_ERR_PARAM        1
#define AM_ERR_LINK         4
#define AM_ERR_TIMEOUT      6

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

#endif /* __APP_CUSTOM_PROTOCOL_H*/
