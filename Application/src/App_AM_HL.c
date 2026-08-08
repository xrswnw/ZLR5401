#include "App_AM_HL.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"

/* =====================================================================
 * AM 解码器硬件抽象层 — USART2 驱动 + 2A A2 帧收发
 *
 * 引脚 (自选, 避开 USB PA11/12、LED PB4/PB5、SWD、UHF USART1 PA9/10、
 *       SPI2 PB12~15):
 *   USART2_TX = PA2, USART2_RX = PA3  (AF_PP)
 * 时钟: USART2 在 APB1 (36MHz), 115200 波特率。
 * ===================================================================== */

#define AM_HL_BAUD         115200u

/* ---------- 接收环形缓冲 ---------- */
#define AM_HL_RX_BUF_SIZE  256u
static volatile uint8_t  s_rxBuf[AM_HL_RX_BUF_SIZE];
static volatile uint16_t s_rxHead;
static volatile uint16_t s_rxTail;

void AM_HL_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    /* PA2/PA3 复用 USART2 (AFIO 时钟在 System_PeriphClk 已开) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_2;             /* PA2 = USART2_TX (AF_PP) */
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin  = GPIO_Pin_3;              /* PA3 = USART2_RX (浮空输入) */
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = AM_HL_BAUD;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &usart);

    /* 收中断: 每字节进环形缓冲 */
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
    NVIC_SetPriority(USART2_IRQn, 4);
    NVIC_EnableIRQ(USART2_IRQn);

    USART_Cmd(USART2, ENABLE);

    s_rxHead = 0; s_rxTail = 0;
}

void AM_HL_DeInit(void)
{
    USART_Cmd(USART2, DISABLE);
    NVIC_DisableIRQ(USART2_IRQn);
    USART_DeInit(USART2);
}

/* USART2 接收中断: RXNE -> 环形缓冲 (非满才写入, 覆盖旧数据) */
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET) {
        uint8_t b = (uint8_t)(USART2->DR & 0xFF);
        uint16_t next = (uint16_t)((s_rxHead + 1) % AM_HL_RX_BUF_SIZE);
        if (next != s_rxTail) {
            s_rxBuf[s_rxHead] = b;
            s_rxHead = next;
        }
    }
    if (USART_GetITStatus(USART2, USART_IT_TXE) != RESET) {
        USART_ClearITPendingBit(USART2, USART_IT_TXE);
    }
}

/* ---------- 1 字节带超时阻塞发送 ---------- */
static int am_uart_send_byte(uint8_t b, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= timeoutMs) return -1;
    }
    USART_SendData(USART2, b);
    return 0;
}

void AM_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t datalen)
{
    uint8_t chk = cmd;          /* 校验 = cmd+包数+包次+包长+data 累加 & 0xFF */
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

int AM_HL_RecvFrame(uint8_t *cmd, uint8_t *data, uint8_t *len, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    uint8_t tmp[AM_HL_FRAME_MAX];

    while ((SysTickHl_GetMs() - t0) < timeoutMs) {
        uint16_t total = am_uart_peek_frame(tmp, sizeof(tmp));
        if (total == 0u) continue;
        if (total < 7u) return AM_HL_LINK_BAD_CRC;  /* 头+cmd+cnt+idx+len+chk 最小7 */

        uint8_t datalen = tmp[5];                   /* 包长位于偏移5 */
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
