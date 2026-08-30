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
 *   行程开关:  MCU_KEY_UP=PC8 (输入), MCU_KEY_DOWN=PC9 (输入, 已恢复)
 *   蜂鸣器:    MCU_BEEP5V0_CTL=PC12 (输出, 高电平响)
 *   调试串口:  UART4 TX=PC10 (AF_PP), RX=PC11 (输入浮空, UHF 已回归 USART3 后释放)
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
    /* 输入: 光电 PC4, 上行程 PC8, 下行程 PC9 (高电平有效) */
    gpio_cfg_input(IR_DET_GPIO_PORT,     IR_DET_GPIO_PIN);
    gpio_cfg_input(KEY_UP_GPIO_PORT,     KEY_UP_GPIO_PIN);
    gpio_cfg_input(KEY_DOWN_GPIO_PORT,   KEY_DOWN_GPIO_PIN);

    /* 输出: 蜂鸣器 PC12 (高电平响) */
    gpio_cfg_output(BEEP_GPIO_PORT, BEEP_GPIO_PIN);

    /* 调试串口 UART4 (PC10/11 已释放, 默认关闭; APP_DEBUG_SERIAL_EN=0 时跳过)*/
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
uint8_t App_NewPeriph_ReadKeyDown(void)
{
    return (GPIO_ReadInputDataBit(KEY_DOWN_GPIO_PORT, KEY_DOWN_GPIO_PIN) != RESET) ? 1u : 0u;
}

/* ---- 蜂鸣器 (PC12, 高电平响) ---- */
/* 一次性脉冲调度状态 (供业务短鸣: 结账完成提示等; 到期自动静音) */
static uint32_t s_pulseEndMs = 0u;      /* 当前脉冲绝对截止时刻 */
static uint8_t  s_pulseActive = 0u;     /* 1=脉冲进行中 */

void App_NewPeriph_Beep(uint8_t on)
{
    if (on) GPIO_SetBits(BEEP_GPIO_PORT, BEEP_GPIO_PIN);
    else    GPIO_ResetBits(BEEP_GPIO_PORT, BEEP_GPIO_PIN);
}

/* ---- 一次性蜂鸣脉冲 (ms): 立即响, 持续 ms 后自动停 ----
 * 供业务反馈触发 (结账完成提示 / 回零成功长鸣等)。
 * 重复触发会延长截止时刻 (粘连抑制靠上层节流)。
 * 注: 原 IR 光电(PC4) 100ms 循环蜂鸣联动已按需求移除, 蜂鸣器专职
 *     业务脉冲提示; IR 检测读取仍可用 App_NewPeriph_ReadIr()。 */
void App_NewPeriph_BeepPulse(uint32_t ms)
{
    if (ms == 0u) { App_NewPeriph_Beep(0); s_pulseActive = 0u; return; }
    s_pulseEndMs = SysTickHl_GetMs() + ms;
    s_pulseActive = 1u;
    App_NewPeriph_Beep(1);
}

/* 查询 + 驱动脉冲: 每次要改变蜂鸣器输出前调用.
 * 返回 1=当前有进行中的脉冲 (应保持响); 0=无脉冲 (可静音).
 * 脉冲到期后自动静音并清零活动位. */
uint8_t App_NewPeriph_BeepPulseActive(void)
{
    if (!s_pulseActive) return 0u;
    if (SysTickHl_GetMs() < s_pulseEndMs)   /* 未到期 */
        return 1u;
    /* 到期: 静音清除 */
    App_NewPeriph_Beep(0);
    s_pulseActive = 0u;
    return 0u;
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
