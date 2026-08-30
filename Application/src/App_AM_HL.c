#include "App_AM_HL.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"

/* =====================================================================
 * AM 消磁器/解码器硬件抽象层 — RS232(USART1) 驱动 + 2A A2 帧收发
 *
 * 引脚 (见原理图 RS232_TXD1/RXD1):
 *   USART1_TX = PA9, USART1_RX = PA10 (全双工 RS232, 无方向控制脚)
 * 时钟: USART1 在 APB2 (72MHz), 115200 波特率。
 * ===================================================================== */

#define AM_HL_BAUD         115200u

/* ---------- 接收环形缓冲 ---------- */
#define AM_HL_RX_BUF_SIZE  640u
static volatile uint8_t  s_rxBuf[AM_HL_RX_BUF_SIZE];
static volatile uint16_t s_rxHead;
static volatile uint16_t s_rxTail;

void AM_HL_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    /* PA9/PA10 复用 USART1 (RS232 TXD/RXD) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_9;             /* PA9 = USART1_TX (AF_PP) */
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin  = GPIO_Pin_10;             /* PA10 = USART1_RX (浮空输入) */
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = AM_HL_BAUD;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART1, &usart);

    /* 收中断: 每字节进环形缓冲 */
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_SetPriority(USART1_IRQn, 4);
    NVIC_EnableIRQ(USART1_IRQn);

    USART_Cmd(USART1, ENABLE);

    s_rxHead = 0; s_rxTail = 0;
}

void AM_HL_DeInit(void)
{
    USART_Cmd(USART1, DISABLE);
    NVIC_DisableIRQ(USART1_IRQn);
    USART_DeInit(USART1);
}

/* USART1 接收中断: RXNE -> 环形缓冲 (非满才写入, 覆盖旧数据) */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        uint8_t b = (uint8_t)(USART1->DR & 0xFF);
        uint16_t next = (uint16_t)((s_rxHead + 1) % AM_HL_RX_BUF_SIZE);
        if (next != s_rxTail) {
            s_rxBuf[s_rxHead] = b;
            s_rxHead = next;
        }
    }
    if (USART_GetITStatus(USART1, USART_IT_TXE) != RESET) {
        USART_ClearITPendingBit(USART1, USART_IT_TXE);
    }
}

/* ---------- 1 字节带超时阻塞发送 ---------- */
static int am_uart_send_byte(uint8_t b, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= timeoutMs) return -1;
    }
    USART_SendData(USART1, b);
    return 0;
}

void AM_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t datalen)
{
    uint8_t chk = cmd;          /* 校验 = cmd+包数+包次+包长+data 累加 & 0xFF (不含头) */
    const uint8_t cnt = 1u;     /* 包数=1 (单包) */
    const uint8_t idx = 1u;     /* 包次=1 (第1包) */
    const uint8_t len = datalen;

    chk = (uint8_t)(chk + cnt + idx + len);
    for (uint8_t i = 0; i < datalen; i++) chk = (uint8_t)(chk + data[i]);

    (void)am_uart_send_byte(AM_HL_FRAME_HDR1, 50);
    (void)am_uart_send_byte(AM_HL_FRAME_HDR2, 50);
    (void)am_uart_send_byte(cmd, 50);
    (void)am_uart_send_byte(cnt, 50);
    (void)am_uart_send_byte(idx, 50);
    (void)am_uart_send_byte(len, 50);
    for (uint8_t i = 0; i < datalen; i++)
        (void)am_uart_send_byte(data[i], 50);
    (void)am_uart_send_byte(chk, 50);
}

/* 在接收缓冲中提取一帧; 返回总字节数, 0=无完整帧 */
static uint16_t am_uart_peek_frame(uint8_t *out, uint16_t maxlen)
{
    if (s_rxHead == s_rxTail) return 0;

    /* 定位帧头 2A A2 */
    uint16_t h = s_rxTail;
    while (h != s_rxHead) {
        uint16_t nx = (uint16_t)((h + 1) % AM_HL_RX_BUF_SIZE);
        if (s_rxBuf[h] == AM_HL_FRAME_HDR1 && nx != s_rxHead &&
            s_rxBuf[nx] == AM_HL_FRAME_HDR2)
            break;
        h = nx;
    }
    if (h == s_rxHead) return 0;

    /* 校验帧完整可用: 从帧头累计的字节数须 ≥ 最小帧 (2A A2 + 4 + chk = 7) */
    uint16_t have = 0;
    {
        uint16_t i = h;
        while (i != s_rxHead) { have++; i = (uint16_t)((i + 1) % AM_HL_RX_BUF_SIZE); }
    }
    if (have < 7u) return 0;

    uint8_t  datalen = s_rxBuf[(uint16_t)((h + 5) % AM_HL_RX_BUF_SIZE)];  /* 包长 */
    uint16_t total = 6u + datalen + 1u;    /* 2A A2 cmd cnt idx len + data + chk */
    if (total > maxlen) total = maxlen;
    if (have < total) return 0;

    for (uint16_t i = 0; i < total; i++) {
        out[i] = s_rxBuf[h];
        h = (uint16_t)((h + 1) % AM_HL_RX_BUF_SIZE);
    }
    s_rxTail = h;
    return total;
}

/* 解析一帧完整原始报 (tmp, 已由 am_uart_peek_frame 裁出); 0=OK, 负=校验错 */
static int am_frame_parse(const uint8_t *tmp, uint16_t total,
                          uint8_t *cmd, uint8_t *data, uint8_t *len)
{
    if (total < 7u) return AM_HL_LINK_BAD_CRC;      /* 头+cmd+cnt+idx+len+chk 最小7 */

    uint8_t datalen = tmp[5];                       /* 包长位于偏移5 */
    uint16_t expTotal = (uint16_t)(6u + datalen + 1u);
    if (total != expTotal) return AM_HL_LINK_BAD_CRC;

    /* 校验 = cmd+cnt+idx+len+data & 0xFF */
    uint8_t chk = (uint8_t)((uint8_t)tmp[2] + (uint8_t)tmp[3] +
                            (uint8_t)tmp[4] + (uint8_t)tmp[5]);
    for (uint16_t i = 0; i < datalen; i++) chk = (uint8_t)(chk + tmp[6 + i]);
    uint8_t recv = tmp[6 + datalen];
    if (chk != recv) return AM_HL_LINK_BAD_CRC;

    *cmd = tmp[2];
    if (datalen > AM_HL_PAYLOAD_MAX) datalen = AM_HL_PAYLOAD_MAX;
    for (uint16_t i = 0; i < datalen; i++) data[i] = tmp[6 + i];
    *len = datalen;
    return 0;
}

int AM_HL_RecvFrame(uint8_t *cmd, uint8_t *data, uint8_t *len, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    uint8_t tmp[AM_HL_FRAME_MAX];

    while ((SysTickHl_GetMs() - t0) < timeoutMs) {
        uint16_t total = am_uart_peek_frame(tmp, sizeof(tmp));
        if (total == 0u) continue;

        /* 校验错直接丢弃, 继续等下帧 (不回错, 避免误伤异步上报)*/
        if (am_frame_parse(tmp, total, cmd, data, len) == 0) return 0;
    }
    return AM_HL_LINK_TIMEOUT;
}

int AM_HL_TryRecvFrame(uint8_t *cmd, uint8_t *data, uint8_t *len)
{
    uint8_t tmp[AM_HL_FRAME_MAX];
    uint16_t total = am_uart_peek_frame(tmp, sizeof(tmp));
    if (total == 0u) return -1;
    return am_frame_parse(tmp, total, cmd, data, len);
}

/* 波形专用接收: 收一个 0x64 波形包. 帧 = 2A A2 64 04 <包次> 66 <100点> EF FE, 无校验位.
 * 校验错/非波形包则丢弃继续等下包 (与通用路径同为"不回错避免误伤"). */
int AM_HL_RecvWavePkt(uint8_t pkt[AM_HL_WAVE_PAYLOAD], uint8_t *idx, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    uint8_t tmp[AM_HL_WAVE_PKT];

    while ((SysTickHl_GetMs() - t0) < timeoutMs) {
        uint16_t total = am_uart_peek_frame(tmp, sizeof(tmp));
        if (total == 0u) continue;
        if (total != AM_HL_WAVE_PKT) continue;                       /* 非 108B 波形包 -> 丢弃 */

        if (tmp[2] == AM_CMD_WAVE && tmp[3] == 4u &&
            tmp[AM_HL_WAVE_PKT - 2] == AM_HL_WAVE_TRAILER_LO &&
            tmp[AM_HL_WAVE_PKT - 1] == AM_HL_WAVE_TRAILER_HI) {
            *idx = tmp[4];                                           /* 包次 0..3 */
            for (uint16_t i = 0; i < AM_HL_WAVE_PAYLOAD; i++)
                pkt[i] = tmp[6u + i];                                /* 100 点数据 */
            return 0;
        }
    }
    return AM_HL_LINK_TIMEOUT;
}

uint16_t AM_HL_RxAvailable(void)
{
    if (s_rxHead >= s_rxTail) return (uint16_t)(s_rxHead - s_rxTail);
    return (uint16_t)(AM_HL_RX_BUF_SIZE - s_rxTail + s_rxHead);
}

uint16_t AM_HL_RxRead(uint8_t *buf, uint16_t maxlen)
{
    uint16_t avail = AM_HL_RxAvailable();
    if (avail == 0u) return 0;
    if (maxlen > avail) maxlen = avail;
    for (uint16_t i = 0; i < maxlen; i++) {
        buf[i] = s_rxBuf[s_rxTail];
        s_rxTail = (uint16_t)((s_rxTail + 1) % AM_HL_RX_BUF_SIZE);
    }
    return maxlen;
}

void AM_HL_RxFlush(void)
{
    s_rxHead = 0; s_rxTail = 0;
}
