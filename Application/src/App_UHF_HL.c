#include "App_UHF_HL.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"

/* =====================================================================
 * UHF 模块硬件抽象层 — UART4 驱动 + 电源/IO 控制 + EX10 0xFF 帧收发
 *
 * 引脚 (见原理图):
 *   UART4_TX = PC10, UART4_RX = PC11  (AF_PP)
 *   UHF_EN     = PB12 (输出, 高电平上电)
 *   UHF_OUT2   = PB13, UHF_IN1 = PB14, UHF_IN2 = PB15 (模块 IO, 推挽输出)
 *   UHF_NRST   = PC6 (输出, 低=复位), UHF_OUT1 = PC7 (输出)
 * 时钟: UART4 在 APB1 (36MHz), GPIOB + GPIOC 由 System_PeriphClkInit 统一开启。
 * ===================================================================== */

/* ---------- 引脚 (按 App_Config.h / 原理图) ---------- */
#define UHF_HL_EN_PORT      UHF_EN_GPIO_PORT      /* GPIOB */
#define UHF_HL_EN_PIN       UHF_EN_GPIO_PIN       /* PB12 */
#define UHF_HL_REG_PORT     UHF_REG_GPIO_PORT     /* GPIOB */
#define UHF_HL_OUT2_PIN     UHF_REG_OUT2_PIN      /* PB13 */
#define UHF_HL_IN1_PIN      UHF_REG_IN1_PIN       /* PB14 */
#define UHF_HL_IN2_PIN      UHF_REG_IN2_PIN       /* PB15 */
#define UHF_HL_NRST_PORT    UHF_NRST_GPIO_PORT    /* GPIOC */
#define UHF_HL_NRST_PIN     UHF_NRST_GPIO_PIN     /* PC6 */
#define UHF_HL_OUT1_PORT    UHF_OUT1_GPIO_PORT    /* GPIOC */
#define UHF_HL_OUT1_PIN     UHF_OUT1_GPIO_PIN     /* PC7 */

#define UHF_HL_BAUD         115200u

/* ---------- 接收环形缓冲 ---------- */
#define UHF_HL_RX_BUF_SIZE  512u
static volatile uint8_t  s_rxBuf[UHF_HL_RX_BUF_SIZE];
static volatile uint16_t s_rxHead;
static volatile uint16_t s_rxTail;

/* ---------- 发送状态 ---------- */
static volatile uint8_t  s_txBusy;

void UHF_HL_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    /* 时钟: GPIOB + GPIOC (APB2), UART4 (APB1) */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_UART4, ENABLE);

    /* UHF_EN: 推挽输出, 默认低 (模块不上电) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = UHF_HL_EN_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(UHF_HL_EN_PORT, &gpio);
    GPIO_ResetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);

    /* 模块 IO: OUT2/IN1/IN2 (GPIOB) + NRST/OUT1 (GPIOC), 推挽输出.
     * NRST 默认高 (非复位), 其余默认低. */
    gpio.GPIO_Pin = UHF_HL_OUT2_PIN | UHF_HL_IN1_PIN | UHF_HL_IN2_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(UHF_HL_REG_PORT, &gpio);
    GPIO_ResetBits(UHF_HL_REG_PORT, UHF_HL_OUT2_PIN | UHF_HL_IN1_PIN | UHF_HL_IN2_PIN);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = UHF_HL_NRST_PIN | UHF_HL_OUT1_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(UHF_HL_NRST_PORT, &gpio);
    GPIO_SetBits(UHF_HL_NRST_PORT, UHF_HL_NRST_PIN);     /* NRST 高=运行 */
    GPIO_ResetBits(UHF_HL_NRST_PORT, UHF_HL_OUT1_PIN);

    /* UART4 TX=PC10 (AF_PP), RX=PC11 (输入浮空) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_10;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin  = GPIO_Pin_11;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = UHF_HL_BAUD;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(UART4, &usart);

    /* 收中断: 每字节进中断写入环形缓冲 */
    USART_ITConfig(UART4, USART_IT_RXNE, ENABLE);
    NVIC_SetPriority(UART4_IRQn, 3);
    NVIC_EnableIRQ(UART4_IRQn);

    USART_Cmd(UART4, ENABLE);

    s_rxHead = 0; s_rxTail = 0; s_txBusy = 0;
}

void UHF_HL_DeInit(void)
{
    USART_Cmd(UART4, DISABLE);
    NVIC_DisableIRQ(UART4_IRQn);
    USART_DeInit(UART4);
    GPIO_ResetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);
}

/* UART4 接收中断: RXNE -> 环形缓冲 (覆盖旧数据, 无阻塞) */
void UART4_IRQHandler(void)
{
    if (USART_GetITStatus(UART4, USART_IT_RXNE) != RESET) {
        uint8_t b = (uint8_t)(UART4->DR & 0xFF);
        uint16_t next = (uint16_t)((s_rxHead + 1) % UHF_HL_RX_BUF_SIZE);
        if (next != s_rxTail) {                 /* 非满才写入 */
            s_rxBuf[s_rxHead] = b;
            s_rxHead = next;
        }
    }
    if (USART_GetITStatus(UART4, USART_IT_TXE) != RESET) {
        USART_ClearITPendingBit(UART4, USART_IT_TXE);
    }
}

void UHF_HL_SetPowerEn(uint8_t en)
{
    if (en) GPIO_SetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);
    else    GPIO_ResetBits(UHF_HL_EN_PORT, UHF_HL_EN_PIN);
}

void UHF_HL_SetAntenna(uint8_t idx)
{
    /* 板上无 MCU 天线切换脚 (SIM7500 ANT 在模块上); 天线选择经模块命令。
     * 保留接口为无操作, 上层调用不受影响。 */
    (void)idx;
}

/* ---------- 1 字节带超时阻塞发送 ---------- */
static int uart_send_byte(uint8_t b, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    while (USART_GetFlagStatus(UART4, USART_FLAG_TXE) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= timeoutMs) return -1;
    }
    USART_SendData(UART4, b);
    return 0;
}

/* ---------- CRC16 (EX10 附录5: init 0xFFFF, poly 0x1021, MSB first; 覆盖 0xFF 之后) ---------- */
uint16_t UHF_HL_Crc16(const uint8_t *data, uint16_t len, uint16_t init)
{
    uint16_t crc = init;
    for (uint16_t i = 0; i < len; i++) {
        for (int dcdBit = 7; dcdBit >= 0; dcdBit--) {
            uint16_t xorFlag = (uint16_t)(crc >> 15);
            crc = (uint16_t)((crc << 1) | ((data[i] >> dcdBit) & 1u));
            if (xorFlag) crc ^= 0x1021u;
        }
    }
    return crc;
}

void UHF_HL_SendFrame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint32_t t0;
    uint8_t  l = (uint8_t)(len & 0xFFu);
    s_txBusy = 1;
    /* CRC 覆盖 0xFF 之后的所有字节: len cmd data */
    uint16_t crc = UHF_HL_Crc16(&l, 1u, 0xFFFFu);
    crc = UHF_HL_Crc16(&cmd, 1u, crc);
    if (len > 0u) crc = UHF_HL_Crc16(data, len, crc);

    (void)uart_send_byte(UHF_HL_FRAME_HDR, 50);
    (void)uart_send_byte(l, 50);
    (void)uart_send_byte(cmd, 50);
    for (uint16_t i = 0; i < len; i++)
        (void)uart_send_byte(data[i], 50);
    (void)uart_send_byte((uint8_t)(crc >> 8), 50);   /* CRC 高在前 */
    (void)uart_send_byte((uint8_t)(crc & 0xFF), 50);

    /* 等待 TDR 完全移位送出 (TC) */
    t0 = SysTickHl_GetMs();
    while (USART_GetFlagStatus(UART4, USART_FLAG_TC) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= 100u) break;
    }
    s_txBusy = 0;
}

/* 在接收缓冲中提取一帧. 返回一帧总长 (0xFF 起), 0=无完整帧.
 * 完整帧长度: Data Length+7 (头1 + len1 + cmd1 + status2 + dataN + crc2) */
static uint16_t uart_peek_frame(uint8_t *out, uint16_t maxlen)
{
    if (s_rxHead == s_rxTail) return 0;

    uint16_t p = s_rxTail;
    /* 找帧头 0xFF */
    while (p != s_rxHead && s_rxBuf[p] != UHF_HL_FRAME_HDR)
        p = (uint16_t)((p + 1) % UHF_HL_RX_BUF_SIZE);
    if (p == s_rxHead) return 0;

    /* 读 dataLen */
    uint16_t lp = (uint16_t)((p + 1) % UHF_HL_RX_BUF_SIZE);
    if (lp == s_rxHead) return 0;
    uint16_t dataLen = s_rxBuf[lp];
    uint16_t total   = (uint16_t)(dataLen + 7u);   /* 头+len+cmd+status(2)+data+crc(2) */
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

int UHF_HL_RecvFrame(uint8_t *cmd, uint16_t *status,
                     uint8_t *data, uint16_t *len, uint32_t timeoutMs)
{
    uint32_t t0 = SysTickHl_GetMs();
    uint8_t tmp[UHF_HL_FRAME_MAX];

    /* timeoutMs==0: 非阻塞单次尝试 (供数据面轮询取帧). >0: 阻塞等待. */
    do {
        uint16_t total = uart_peek_frame(tmp, sizeof(tmp));
        if (total == 0u) {
            if (timeoutMs == 0u) return UHF_HL_LINK_TIMEOUT;
            continue;
        }

        if (total < 7u) return UHF_HL_LINK_BAD_CRC;
        uint16_t dataLen = tmp[1];
        if (total != (uint16_t)(dataLen + 7u)) return UHF_HL_LINK_BAD_CRC;

        /* 校验 CRC16 (覆盖 len..data, 即 tmp[1..total-3]) */
        uint16_t calc = UHF_HL_Crc16(&tmp[1], (uint16_t)(total - 3u), 0xFFFFu);
        uint16_t recv = (uint16_t)((tmp[total - 2] << 8) | tmp[total - 1]);
        if (calc != recv) return UHF_HL_LINK_BAD_CRC;

        if (cmd)    *cmd = tmp[2];
        if (status) *status = (uint16_t)((tmp[3] << 8) | tmp[4]);
        /* 数据 = tmp[5 .. total-3-1] */
        uint16_t dlen = dataLen - 2u;
        if (dlen > UHF_HL_DATA_MAX) dlen = UHF_HL_DATA_MAX;
        if (data && dlen) for (uint16_t i = 0; i < dlen; i++) data[i] = tmp[5 + i];
        if (len) *len = dlen;
        return 0;
    } while ((SysTickHl_GetMs() - t0) < timeoutMs);
    return UHF_HL_LINK_TIMEOUT;
}

int UHF_HL_Transact(uint8_t cmd, const uint8_t *tx, uint16_t txLen,
                    uint16_t *rxStatus, uint8_t *rx, uint16_t *rxLen,
                    uint32_t timeoutMs)
{
    uint8_t  respCmd;
    uint16_t respStatus;
    uint16_t respLen = 0;

    /* 丢弃旧残留字节, 保证每个请求-响应原子配对 (若残留帧被当作本帧响应会误判) */
    UHF_HL_RxFlush();

    UHF_HL_SendFrame(cmd, tx, txLen);
    int r = UHF_HL_RecvFrame(&respCmd, &respStatus, rx, &respLen, timeoutMs);
    if (r != 0) {
        if (rxLen) *rxLen = 0;
        return r;
    }
    if (respCmd != cmd) {
        if (rxLen) *rxLen = 0;
        return UHF_HL_LINK_BAD_CRC;
    }
    if (rxStatus) *rxStatus = respStatus;
    if (rxLen)    *rxLen = respLen;
    return 0;
}

/* ---- 扩展指令 (0xAA) 传输 ---- */
static const uint8_t s_extMarker[UHF_HL_EXT_MARKER_LEN] = "Moduletech";

void UHF_HL_SendExt(uint16_t subCmd, const uint8_t *data, uint16_t len)
{
    uint32_t t0;
    uint8_t subcrc = (uint8_t)(subCmd >> 8);
    uint8_t sub[2];

    sub[0] = (uint8_t)(subCmd >> 8);   /* subCmd 高字节在前 */
    sub[1] = (uint8_t)(subCmd & 0xFF);
    subcrc = (uint8_t)(subcrc + sub[1]);
    for (uint16_t i = 0; i < len; i++) subcrc = (uint8_t)(subcrc + data[i]);

    s_txBusy = 1;
    /* 整帧 Data 长度 = marker(10) + subCmd(2) + data + SubCRC(1) + 0xBB(1) = len+14 */
    uint8_t dlen = (uint8_t)((len + 14u) & 0xFFu);

    /* CRC 覆盖 0xFF 之后所有字节 (len .. data .. SubCRC .. 0xBB) */
    uint16_t crc = UHF_HL_Crc16(&dlen, 1u, 0xFFFFu);
    uint8_t c = 0xAAu; crc = UHF_HL_Crc16(&c, 1u, crc);
    crc = UHF_HL_Crc16(s_extMarker, UHF_HL_EXT_MARKER_LEN, crc);
    crc = UHF_HL_Crc16(sub, 2u, crc);
    if (len > 0u) crc = UHF_HL_Crc16(data, len, crc);
    crc = UHF_HL_Crc16(&subcrc, 1u, crc);
    c = 0xBBu; crc = UHF_HL_Crc16(&c, 1u, crc);

    (void)uart_send_byte(UHF_HL_FRAME_HDR, 50);
    (void)uart_send_byte(dlen, 50);
    (void)uart_send_byte(0xAAu, 50);
    for (uint16_t i = 0; i < UHF_HL_EXT_MARKER_LEN; i++)
        (void)uart_send_byte(s_extMarker[i], 50);
    (void)uart_send_byte(sub[0], 50);
    (void)uart_send_byte(sub[1], 50);
    for (uint16_t i = 0; i < len; i++) (void)uart_send_byte(data[i], 50);
    (void)uart_send_byte(subcrc, 50);
    (void)uart_send_byte(0xBBu, 50);
    (void)uart_send_byte((uint8_t)(crc >> 8), 50);   /* CRC 高在前 */
    (void)uart_send_byte((uint8_t)(crc & 0xFF), 50);

    t0 = SysTickHl_GetMs();
    while (USART_GetFlagStatus(UART4, USART_FLAG_TC) == RESET) {
        if ((SysTickHl_GetMs() - t0) >= 100u) break;
    }
    s_txBusy = 0;
}

int UHF_HL_TransactExt(uint16_t subCmd, const uint8_t *tx, uint16_t txLen,
                       uint16_t *rxStatus, uint8_t *subData, uint16_t *subDataLen,
                       uint32_t timeoutMs)
{
    uint8_t  cmd, data[UHF_HL_FRAME_MAX];
    uint16_t st, len;

    UHF_HL_RxFlush();

    UHF_HL_SendExt(subCmd, tx, txLen);
    int r = UHF_HL_RecvFrame(&cmd, &st, data, &len, timeoutMs);
    if (r != 0) {
        if (subDataLen) *subDataLen = 0;
        return r;
    }
    /* 响应: cmd=0xAA, Data = marker(10) + subCmd(2) + subData */
    if (cmd != 0xAAu || len < (UHF_HL_EXT_MARKER_LEN + UHF_HL_EXT_SUB_LEN)) {
        if (subDataLen) *subDataLen = 0;
        return UHF_HL_LINK_BAD_CRC;
    }
    /* 校验 marker */
    for (uint8_t i = 0; i < UHF_HL_EXT_MARKER_LEN; i++) {
        if (data[i] != s_extMarker[i]) {
            if (subDataLen) *subDataLen = 0;
            return UHF_HL_LINK_BAD_CRC;
        }
    }
    uint16_t rspSub = (uint16_t)((data[10] << 8) | data[11]);
    if (rspSub != subCmd) {
        if (subDataLen) *subDataLen = 0;
        return UHF_HL_LINK_BAD_CRC;
    }
    uint16_t dlen = len - (UHF_HL_EXT_MARKER_LEN + UHF_HL_EXT_SUB_LEN);
    if (subData && dlen) for (uint16_t i = 0; i < dlen; i++) subData[i] = data[12 + i];
    if (rxStatus)    *rxStatus    = st;
    if (subDataLen)  *subDataLen  = dlen;
    return 0;
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
