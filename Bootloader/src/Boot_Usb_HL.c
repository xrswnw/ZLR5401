/* * Boot_Usb_HL.c - USB Hardware Layer for Bootloader (STM32 official USB lib)*/
#include "Boot_Usb_HL.h"
#include "Boot_Config.h"
#include "usb_lib.h"
#include "usb_conf.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_gpio.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c)
 * 本模块需要: GPIOA, GPIOB, AFIO, USB (见 BOOT_RCC_*_PERIPH)
 * RCC_USBCLKConfig (PLL/1.5 = 48MHz) 由 Sys_CfgClock 完成.*/

__attribute__((aligned(4))) u8 g_boot_usb_rx_buf[64];
__attribute__((aligned(4))) u8 g_boot_usb_tx_buf[64];

volatile u8 g_boot_usb_configured = 0;
volatile u8 g_boot_usb_connected  = 0;
volatile u8 g_boot_usb_tx_busy    = 0;

extern void App_Usb_OnReceive(const uint8_t *data, uint16_t len);

static void BootUsbHL_EnablePower(void)
{
    GPIO_InitTypeDef gpio;
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Pin   = GPIO_Pin_8;   /* USB_EN = PA8, 拉高驱动 D+ 上拉*/
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(GPIOA, GPIO_Pin_8);
}

static void BootUsbHL_InitClkGpio(void)
{
    GPIO_InitTypeDef gpio;
    /* PA11/PA12 USB D-/D+ 复用推挽 — 必须显式设 GPIO_Mode = AF_PP*/
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin   = GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_Init(GPIOA, &gpio);

    /* 寄存器裸写兜底 (同步 App_Usb_HL.c:78,): 标准库 GPIO_Init 在
     * 某些工具链折叠路径下对 PA11/PA12 不真正写入 CRH, 导致 USB D-/D+ 停在
     * 复位态 -> 主机无法枚举. App 已验证此裸写为必需, Boot 此前缺失 = 本轮根因.*/
    GPIOA->CRH = (GPIOA->CRH & ~(0xFFu << 12)) | ((0xBBu) << 12);

    NVIC_InitTypeDef nvic;
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    nvic.NVIC_IRQChannel                   = USB_LP_CAN1_RX0_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
}

void Boot_Usb_HL_Init(void)
{
    BootUsbHL_EnablePower();
    BootUsbHL_InitClkGpio();
    USB_Init();
    /* Sys_Init() 末尾 Sys_EnableUsb(): 拉高 USB_EN=PA8 触发主机枚举*/
    GPIO_SetBits(GPIOA, GPIO_Pin_8);
}

void Boot_Usb_HL_DeInit(void)
{
    _SetCNTR(CNTR_PDWN | CNTR_FRES);
    /* DISABLE 保留在 HL DeInit: 单外设回收, 不引入与全局开关的对偶依赖*/
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, DISABLE);
    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    g_boot_usb_configured = 0;
}

void Boot_Usb_HL_Poll(void) {}
void Boot_Usb_HL_Connect(void) {}
void Boot_Usb_HL_Disconnect(void) { _SetCNTR(CNTR_PDWN | CNTR_FRES); }

uint8_t Boot_Usb_HL_IsConfigured(void)
{
  extern vu32 bDeviceState;
  return (bDeviceState == 3u);
}

/* Boot 接自定义命令 - 由 IAP 协议调用
 * (同步 App_Usb_HL.c:185): 放宽 bDeviceState != 3u 硬校验
 * (枚举抖动/上拉时序偏移时 CONFIGURED 可能未及时置位, 硬校验会丢弃所有上行
 * 响应 -> 放大"看起来不通"; App 已 DIAG 跳过, Boot 对齐). 仍保留 tx_busy 防重入.
 * (同步 App): IN 缓冲尾部清零至满 64B, 防止短响应残留上次
 * 长响应尾字节 (版本串/UID hash 泄漏).*/
void Boot_Usb_HL_Transmit(const uint8_t *data, uint16_t len)
{
    extern vu32 bDeviceState;
    (void)bDeviceState;  /* 放宽 CONFIGURED 硬校验, 对齐 App DIAG*/
    if (g_boot_usb_tx_busy) return;
    if (len > 63) len = 63;
    g_boot_usb_tx_buf[0] = 0x02;
    for (uint16_t i = 0; i < len; i++) g_boot_usb_tx_buf[i + 1] = data[i];
    /* 尾部清零: PMA 尾部恒为 0, 防短帧残留*/
    for (uint16_t i = len + 1; i < 64; i++) g_boot_usb_tx_buf[i] = 0;
    g_boot_usb_tx_busy = 1;
    UserToPMABufferCopy(g_boot_usb_tx_buf, ENDP1_TXADDR, 64);
    SetEPTxValid(ENDP1);
}

/* ====================================================================
 * ST 库回调 (经 usb_conf.h 宏映射, 实际函数名 EPx_IN_User / EPx_OUT_User)
 * usb_istr.c pEpInt_IN[] 用标识符 EP1_IN_Callback 等初始化, 然后由
 * usb_conf.h 把标识符 #define 成这里定义的 _User 函数。
 * ====================================================================*/
/* EP1 IN 完成回调: 设备→主机 发送完成, 清 tx_busy*/
extern void Boot_Usb_Transport_OnTxComplete(void);  /* Boot_Usb.c (同步 App)*/
void EP1_IN_User(void)
{
    g_boot_usb_tx_busy = 0;
    Boot_Usb_Transport_OnTxComplete();   /* 清 transport 层 TxBusy,
                                          * 让 Proto_TxResponse 等待循环正确同步*/
}

/* EP2 OUT 完成回调: 主机→设备 接收完成, 取走 PMA 数据*/
extern void Boot_Usb_OnReceive(const uint8_t *data, uint16_t len);
void EP2_OUT_User(void)
{
    /* 长度: ENDP2_OUT 包最大 64B (USB_FRAME_LEN).
     * Bootloader 走 USB HID: 取走 PMA 数据交上层 OnReceive (写环形缓冲 + 喂协议层)*/
    uint16_t count = (uint16_t)GetEPRxCount(ENDP2);
    if (count > 0 && count <= 64) {
        static uint8_t s_rx[64];
        PMAToUserBufferCopy(s_rx, ENDP2_RXADDR, count);
        Boot_Usb_OnReceive(s_rx, count);
    }
    SetEPRxValid(ENDP2);
}
