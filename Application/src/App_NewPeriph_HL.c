#include "App_NewPeriph_HL.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"

/* =====================================================================
 * 新增外设硬件抽象层 (骨架, 见原理图)
 *   光电接口 (TLP181): MCU_IR1_DET = PC4 (输入, 检测红外/光电)
 *   行程开关:  MCU_KEY_UP=PC8 (输入), MCU_KEY_DOWN=PC9 (输入)
 *   蜂鸣器:    MCU_BEEP5V0_CTL=PC12 (输出, 高电平响)
 *   调试串口:  UART4 TX=PC10 (AF_PP), RX=PC11 (输入浮空)
 *   RCC: GPIOB/GPIOC(APB2) + UART4(APB1) 由 System_PeriphClkInit 统一开启。
 * ===================================================================== */

/* APP_RCC_APB1_PERIPH 需含 RCC_APB1Periph_UART4 (已在 App_Config.h 并入) */

/* ---- 输入引脚配置: 上拉输入 (行程开关/光电, 高电平有效) ---- */
static void gpio_cfg_input(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef g;
    GPIO_StructInit(&g);
    g.GPIO_Pin  = pin;
    g.GPIO_Mode = GPIO_Mode_IPU;        /* 上拉输入, 默认读 1=无触发, 拉低=触发 */
    GPIO_Init(port, &g);
}

/* ---- 输出引脚配置: 推挽输出, 默认低 (熄蜂鸣器) ---- */
static void gpio_cfg_output(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef g;
    GPIO_StructInit(&g);
    g.GPIO_Pin   = pin;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(port, &g);
    GPIO_ResetBits(port, pin);
}

void App_NewPeriph_Init(void)
{
    /* 输入: 光电 PC4, 上行程 PC8 (高电平有效)
     * 下行程 KEY_DOWN=PC9 已改作 USB_EN, 不再作为输入配置 */
    gpio_cfg_input(IR_DET_GPIO_PORT,     IR_DET_GPIO_PIN);
    gpio_cfg_input(KEY_UP_GPIO_PORT,     KEY_UP_GPIO_PIN);
    /* 下行程 KEY_DOWN=PC9 已改作 USB_EN, 输入配置暂禁用 */
    /* gpio_cfg_input(KEY_DOWN_GPIO_PORT, KEY_DOWN_GPIO_PIN); */

    /* 输出: 蜂鸣器 PC12 (高电平响) */
    gpio_cfg_output(BEEP_GPIO_PORT, BEEP_GPIO_PIN);

    /* 调试串口 UART4 (PC10 已改作 USB_EN, 默认关闭; APP_DEBUG_SERIAL_EN=0 时跳过)*/
#if APP_DEBUG_SERIAL_EN
    App_NewPeriph_DebugInit();
#endif
}

/* ---- 光电 / 行程开关 ---- */
uint8_t App_NewPeriph_ReadIr(void)
{
    return (GPIO_ReadInputDataBit(IR_DET_GPIO_PORT, IR_DET_GPIO_PIN) != RESET) ? 1u : 0u;
}
uint8_t App_NewPeriph_ReadKeyUp(void)
{
    return (GPIO_ReadInputDataBit(KEY_UP_GPIO_PORT, KEY_UP_GPIO_PIN) != RESET) ? 1u : 0u;
}
/* 下行程 KEY_DOWN=PC9 已改作 USB_EN, 读取函数暂禁用 */
/* uint8_t App_NewPeriph_ReadKeyDown(void)
{
    return (GPIO_ReadInputDataBit(KEY_DOWN_GPIO_PORT, KEY_DOWN_GPIO_PIN) != RESET) ? 1u : 0u;
} */

/* ---- 蜂鸣器 (PC12, 高电平响) ---- */
void App_NewPeriph_Beep(uint8_t on)
{
    if (on) GPIO_SetBits(BEEP_GPIO_PORT, BEEP_GPIO_PIN);
    else    GPIO_ResetBits(BEEP_GPIO_PORT, BEEP_GPIO_PIN);
}

/* ---- 光电 IR (PC4) + 蜂鸣器 联动: 触发(低电平)期间 100ms 周期循环响/停 ----
 * 反转电平: IR 检测到低 (光电遮挡) 时, 蜂鸣器每 100ms 周期: 响 100ms / 停 100ms 循环;
 * IR 为高 (无遮挡) 时蜂鸣器关. 由主循环周期调用 (ms 粒度). */
void App_IrBuzzer_Process(void)
{
    static uint32_t s_lastMs = 0;
    static uint8_t  s_prevOn = 0;       /* 上一次 IR 是否高(触发) */
    static uint8_t  s_phase = 0;        /* 当前 100ms 相位: 0=响 1=停 */

    uint32_t now = SysTickHl_GetMs();
    uint8_t  on  = (App_NewPeriph_ReadIr() == 0u);   /* 低=检测到(触发) */

    /* 状态切换/上升沿: 重置相位与计时基准 */
    if (on != s_prevOn) {
        s_prevOn = on;
        s_phase = 0;
        s_lastMs = now;
    }

    if (!on) {
        App_NewPeriph_Beep(0);
        return;
    }

    /* 触发期间: 每 100ms 翻转相位 (响/停交替) */
    if ((now - s_lastMs) >= 100u) {
        s_lastMs = now;
        s_phase = (uint8_t)(s_phase ^ 1u);
    }
    App_NewPeriph_Beep((s_phase == 0u) ? 1u : 0u);
}

/* ---- 调试串口 UART4 (轮询, UART4_IRQ 不在 MD 向量表) ---- */
#if APP_DEBUG_SERIAL_EN
#define DBG_RX_BUF_SIZE  128u
static volatile uint8_t  s_dbgRx[DBG_RX_BUF_SIZE];
static volatile uint16_t s_dbgHead, s_dbgTail;
#endif

void App_NewPeriph_DebugInit(void)
{
#if APP_DEBUG_SERIAL_EN
    GPIO_InitTypeDef g;
    USART_InitTypeDef u;

    /* PC10 = UART4_TX (AF_PP), PC11 = UART4_RX (输入浮空) */
    GPIO_StructInit(&g);
    g.GPIO_Pin   = DBG_TX_GPIO_PIN;
    g.GPIO_Mode  = GPIO_Mode_AF_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DBG_TX_GPIO_PORT, &g);

    GPIO_StructInit(&g);
    g.GPIO_Pin  = DBG_RX_GPIO_PIN;
    g.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(DBG_RX_GPIO_PORT, &g);

    USART_StructInit(&u);
    u.USART_BaudRate            = DBG_BAUD;
    u.USART_WordLength          = USART_WordLength_8b;
    u.USART_StopBits            = USART_StopBits_1;
    u.USART_Parity              = USART_Parity_No;
    u.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    u.USART_HardwareFlowControl = USART_HardwareFlowControl_None;

    USART_Init(DBG_USART, &u);
    USART_Cmd(DBG_USART, ENABLE);

    s_dbgHead = 0; s_dbgTail = 0;
#endif
}

int App_NewPeriph_DebugSend(const uint8_t *buf, uint16_t len)
{
#if APP_DEBUG_SERIAL_EN
    for (uint16_t i = 0; i < len; i++) {
        uint32_t t0 = SysTickHl_GetMs();
        while (USART_GetFlagStatus(DBG_USART, USART_FLAG_TXE) == RESET) {
            if ((SysTickHl_GetMs() - t0) >= 100u) return -1;
        }
        USART_SendData(DBG_USART, buf[i]);
    }
    return (int)len;
#else
    (void)buf; (void)len;
    return 0;
#endif
}

int App_NewPeriph_DebugRecv(uint8_t *buf, uint16_t maxlen)
{
#if APP_DEBUG_SERIAL_EN
    uint16_t got = 0;
    while (got < maxlen && s_dbgHead != s_dbgTail) {
        buf[got++] = s_dbgRx[s_dbgTail];
        s_dbgTail = (uint16_t)((s_dbgTail + 1) % DBG_RX_BUF_SIZE);
    }
    return (int)got;
#else
    (void)buf; (void)maxlen;
    return 0;
#endif
}

void App_NewPeriph_DebugPutStr(const char *s)
{
#if APP_DEBUG_SERIAL_EN
    while (s && *s) {
        uint32_t t0 = SysTickHl_GetMs();
        while (USART_GetFlagStatus(DBG_USART, USART_FLAG_TXE) == RESET) {
            if ((SysTickHl_GetMs() - t0) >= 100u) return;
        }
        USART_SendData(DBG_USART, (uint8_t)*s++);
    }
#else
    (void)s;
#endif
}
