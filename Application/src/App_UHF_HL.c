#include "App_UHF_HL.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"

/* =====================================================================
 * UHF 模块硬件抽象层 — USART1 驱动 + 电源/天线控制 + 0xBB 帧收发
 *
 * 引脚 (自选, 避开 USB PA11/12、LED PB4/PB5、SWD、SPI2 PB12~15):
 *   USART1_TX = PA9, USART1_RX = PA10  (AF_PP)
 *   UHF_EN     = PA8  (输出, 高电平上电)
 *   UHF_ANT    = PC13 (输出, 低=ANT1 / 高=ANT2)
 * 时钟: USART1 在 APB2 (72MHz), 需 RCC_APB2Periph_USART1 + GPIOA + GPIOC。
 * ===================================================================== */

/* ---------- 引脚 (可按硬件调整) ---------- */
#define UHF_HL_EN_PORT      GPIOA
#define UHF_HL_EN_PIN       GPIO_Pin_8
#define UHF_HL_ANT_PORT     GPIOC
#define UHF_HL_ANT_PIN      GPIO_Pin_13

#define UHF_HL_BAUD         115200u

/* ---------- 接收环形缓冲 ---------- */
#define UHF_HL_RX_BUF_SIZE  256u
static volatile uint8_t  s_rxBuf[UHF_HL_RX_BUF_SIZE];
static volatile uint16_t s_rxHead;
static volatile uint16_t s_rxTail;

/* ---------- 发送状态 ---------- */
static volatile uint8_t  s_txBusy;

static uint16_t UHF_HL_Crc16_step(const uint8_t *data, uint16_t len, uint16_t crc);

void UHF_HL_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    /* 时钟: GPIOA(GPIOB 已由全局使能)、GPIOC、USART1(APB2) */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_USART1 |
                           RCC_APB2Periph_GPIOA, ENABLE);

    /* UHF_EN: 推挽输出, 默认低 (模块不上电) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = UHF_HL_EN_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(UHF_HL_EN_PORT, &gpio);
    GPIO_ResetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);

    /* UHF_ANT: 推挽输出, 默认低 (ANT1) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = UHF_HL_ANT_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(UHF_HL_ANT_PORT, &gpio);
    GPIO_ResetBits(UHF_HL_ANT_PORT, UHF_HL_ANT_PIN);

    /* USART1 TX=PA9 (AF_PP), RX=PA10 (输入浮空) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_9;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin  = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = UHF_HL_BAUD;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART1, &usart);

    /* 收中断: 每字节进中断写入环形缓冲 */
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_SetPriority(USART1_IRQn, 3);
    NVIC_EnableIRQ(USART1_IRQn);

    USART_Cmd(USART1, ENABLE);

    s_rxHead = 0; s_rxTail = 0; s_txBusy = 0;
}

void UHF_HL_DeInit(void)
{
    USART_Cmd(USART1, DISABLE);
    NVIC_DisableIRQ(USART1_IRQn);
    USART_DeInit(USART1);
    GPIO_ResetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);
}

/* USART1 接收中断: RXNE -> 环形缓冲 (覆盖旧数据, 无阻塞) */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        uint8_t b = (uint8_t)(USART1->DR & 0xFF);
        uint16_t next = (uint16_t)((s_rxHead + 1) % UHF_HL_RX_BUF_SIZE);
        if (next != s_rxTail) {                 /* 非满才写入 */
            s_rxBuf[s_rxHead] = b;
            s_rxHead = next;
        }
    }
    if (USART_GetITStatus(USART1, USART_IT_TXE) != RESET) {
        /* TXE 中断未使能; 仅为清理状态位 */
        USART_ClearITPendingBit(USART1, USART_IT_TXE);
    }
}

void UHF_HL_SetPowerEn(uint8_t en)
{
    if (en) GPIO_SetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);
    else    GPIO_ResetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);
}

void UHF_HL_SetAntenna(uint8_t idx)
{
    if (idx) GPIO_SetBits(UHF_HL_ANT_PORT, UHF_HL_ANT_PIN);
    else     GPIO_ResetBits(UHF_HL_ANT_PORT, UHF_HL_ANT_PIN);
}

/* ---------- 1 字节带超时阻塞发送 ---------- */
static int uart_send_byte(uint8_t b, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= timeoutMs) return -1;
    }
    USART_SendData(USART1, b);
    return 0;
}

void UHF_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint16_t crc;
    uint32_t t0;
    s_txBusy = 1;
    t0 = SysTickHl_GetMs();
    (void)uart_send_byte(UHF_HL_FRAME_HDR, 50);
    (void)uart_send_byte((uint8_t)(len + 1u), 50);   /* len = cmd+data */
    (void)uart_send_byte(cmd, 50);
    for (uint16_t i = 0; i < len; i++)
        (void)uart_send_byte(data[i], 50);
    crc = UHF_HL_Crc16(&cmd, 1u);
    if (len > 0u)
        crc = UHF_HL_Crc16_step(data, len, crc);
    (void)uart_send_byte((uint8_t)(crc & 0xFF), 50);
    (void)uart_send_byte((uint8_t)(crc >> 8), 50);
    /* 等待 TDR 完全移位送出 (TC), 再清发送忙 */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= 100u) break;
    }
    s_txBusy = 0;
}

/* ---------- CRC16 (CCITT, poly 0x1021, init 0xFFFF) ---------- */
uint16_t UHF_HL_Crc16(const uint8_t *data, uint16_t len)
{
    return UHF_HL_Crc16_step(data, len, 0xFFFFu);
}

uint16_t UHF_HL_Crc16_step(const uint8_t *data, uint16_t len, uint16_t crc)
{
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else               crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* 在接收缓冲中提取一帧; 返回实际长度 (不含帧头), 0=无完整帧 */
static uint16_t uart_peek_frame(uint8_t *out, uint16_t maxlen)
{
    if (s_rxHead == s_rxTail) return 0;

    uint16_t p = s_rxTail;
    /* 找帧头 0xBB */
    while (p != s_rxHead && s_rxBuf[p] != UHF_HL_FRAME_HDR)
        p = (uint16_t)((p + 1) % UHF_HL_RX_BUF_SIZE);
    if (p == s_rxHead) return 0;

    /* 读 len */
    uint16_t lp = (uint16_t)((p + 1) % UHF_HL_RX_BUF_SIZE);
    if (lp == s_rxHead) return 0;
    uint16_t payLen = s_rxBuf[lp];
    uint16_t total = 3u + payLen + 2u;      /* hdr + len + (cmd+data) + crc16 */
    if (total > maxlen) total = maxlen;

    /* 帧是否完整到达 */
    uint16_t have = 0;
    {
        uint16_t i = p, cnt = 0;
        while (i != s_rxHead) { cnt++; i = (uint16_t)((i + 1) % UHF_HL_RX_BUF_SIZE); }
        have = cnt;
    }
    if (have < total) return 0;

    for (uint16_t i = 0; i < total; i++) {
        out[i] = s_rxBuf[p];
        p = (uint16_t)((p + 1) % UHF_HL_RX_BUF_SIZE);
    }
    s_rxTail = p;
    return total;
}

int UHF_HL_RecvFrame(uint8_t *cmd, uint8_t *data, uint16_t *len, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    uint8_t tmp[UHF_HL_FRAME_MAX];

    while ((SysTickHl_GetMs() - t0) < timeoutMs) {
        uint16_t total = uart_peek_frame(tmp, sizeof(tmp));
        if (total == 0u) continue;

        if (total < 6u) return UHF_HL_LINK_BAD_CRC;   /* 头+len+cmd+2crc */
        uint16_t payLen = tmp[1];
        uint16_t expTotal = 3u + payLen + 2u;
        if (total != expTotal) return UHF_HL_LINK_BAD_CRC;

        /* 校验 CRC16 (覆盖 len..data, 即 tmp[1..total-3]) */
        uint16_t calc = UHF_HL_Crc16(&tmp[1], (uint16_t)(total - 3u));
        uint16_t recv = (uint16_t)tmp[total - 2] | ((uint16_t)tmp[total - 1] << 8);
        if (calc != recv) return UHF_HL_LINK_BAD_CRC;

        *cmd = tmp[2];
        uint16_t dlen = payLen - 1u;
        if (dlen > UHF_HL_PAYLOAD_MAX) dlen = UHF_HL_PAYLOAD_MAX;
        for (uint16_t i = 0; i < dlen; i++) data[i] = tmp[3 + i];
        *len = dlen;
        return 0;
    }
    return UHF_HL_LINK_TIMEOUT;
}

uint16_t UHF_HL_RxAvailable(void)
{
    if (s_rxHead >= s_rxTail) return (uint16_t)(s_rxHead - s_rxTail);
    return (uint16_t)(UHF_HL_RX_BUF_SIZE - s_rxTail + s_rxHead);
}

uint16_t UHF_HL_RxRead(uint8_t *buf, uint16_t maxlen)
{
    uint16_t avail = UHF_HL_RxAvailable();
    if (avail == 0u) return 0;
    if (maxlen > avail) maxlen = avail;
    for (uint16_t i = 0; i < maxlen; i++) {
        buf[i] = s_rxBuf[s_rxTail];
        s_rxTail = (uint16_t)((s_rxTail + 1) % UHF_HL_RX_BUF_SIZE);
    }
    return maxlen;
}

void UHF_HL_RxFlush(void)
{
    s_rxHead = 0; s_rxTail = 0;
}

int UHF_HL_WaitTxIdle(uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    while (s_txBusy) {
        if ((SysTickHl_GetMs() - t0) >= timeoutMs) return -1;
    }
    return 0;
}
