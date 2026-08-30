/* * App_Usb_HL.c - USB 硬件层 (严格 1:1 调用顺序)
 * 顺序参考 src/SysCfg.c Sys_Init():
 * 1) Sys_CfgClock - PLL + USBCLKConfig(PLL/1.5=48M)
 * 2) Sys_CfgPeriphClk - 开 USB 时钟 (RCC_APB1Periph_USB)
 * 3) Sys_DisableInt - cpsid i (可选, 初始化前关中断)
 * 4) Sys_CtrlIOInit - GPIO_PinRemapConfig(JTAGDisable) + USB_EN=PA8
 * 5) USB_InitInterface - PA11/12 AF_PP
 * 6) USB_ConfigInt - NVIC USB_LP_CAN1_RX0
 * 7) USB_Init - 触发 CustomHID_init -> PowerOn
 * 步骤 1 由 system_stm32f10x.c 的 SetSysClockTo72 完成 (PLL×6=72M).
 * 这里只做 2-7 + 上层数据接口.*/
#include "App_Usb_HL.h"
#include "App_Led_HL.h"
#include "App_SysTick_HL.h"   /* SysTickHl_GetMs (USB TX 等待)*/
/* 注意: 必须先 include stm32f10x.h, 让 __STM32F10x_H 被定义,
 * usb_type.h 的 typedef 块 (u8/FlagStatus 等) 才会被跳过,
 * 避免与 stm32f10x.h 自身的同名 typedef 冲突.*/
#include "App_Config.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_gpio.h"
#include "misc.h"
#include "usb_lib.h"
#include "usb_conf.h"
/* RCC 时钟使能由 System_PeriphClkInit() 统一开启 (main.c)
 * 本模块需要: GPIOA, GPIOB, AFIO, USB (见 APP_RCC_APB2_PERIPH / APP_RCC_APB1_PERIPH)
 * RCC_USBCLKConfig (PLL/1.5 = 48MHz) 由 App_Sys_CfgClock 完成.*/

/* ====================================================================
 * 状态 (供 App_Usb.c 查询)
 * ====================================================================*/
volatile uint8_t App_Usb_Configured = 0;   /* 由 ST 库 CustomHID_SetConfiguration 钩子置 1*/
volatile uint8_t App_Usb_Connected  = 0;   /* 由 ST 库 CustomHID_Reset 钩子置 1*/

/* 端点数据 - EP1 IN (TX) 缓冲, EP2 OUT (RX) 缓冲*/
#define USB_RX_BUF_SZ  64
__attribute__((aligned(4))) uint8_t App_Usb_RxBuf[USB_RX_BUF_SZ];
__attribute__((aligned(4))) uint8_t App_Usb_TxBuf[USB_RX_BUF_SZ];
volatile uint8_t App_Usb_TxBusy = 0;

/* 上层钩子: App_Usb_OnReceive 由 App_Usb.c 实现*/
extern void App_Usb_OnReceive(const uint8_t *data, uint16_t len);
/* App_Usb_Transport_OnTxComplete 由 App_Usb_Transport.c 实现
 * (无独立头文件, 这里 extern 声明供 EP1_IN_User 调用)*/
extern void App_Usb_Transport_OnTxComplete(void);

/* ====================================================================
 * 顺序: 时钟使能 -> 引脚 -> NVIC -> USB_Init
 * (时钟使能已由 System_PeriphClkInit() 在 main 开头统一处理, 这里跳过)
 * ====================================================================*/
static void App_Usb_HL_GpioInit(void)
{
    GPIO_InitTypeDef gpio;

    /* JTAG 释放 PB3/PB4/PA15, 保留 SWD (Sys_CtrlIOInit)*/
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    /* USB_EN: PA8 推挽输出, 下拉关闭 USB (D+ 上拉断开)*/
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Pin   = USB_EN_GPIO_PIN;
    GPIO_Init(USB_EN_GPIO_PORT, &gpio);
    GPIO_ResetBits(USB_EN_GPIO_PORT, USB_EN_GPIO_PIN);

    /* PA11/PA12 USB D-/D+ 复用推挽 (USB_InitInterface)
     * 注: 必须显式重设 GPIO_Mode = AF_PP; 否则沿用上一段 Out_PP,
     * PA11/PA12 (USB 差分线) 配成普通推挽, 主机永远枚举不到设备!
     * 防御: 用寄存器裸写再覆盖一遍, 跳过可能未生效的 GPIO_Init 折叠路径*/
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin   = GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_Init(GPIOA, &gpio);

    /* 寄存器裸写兜底: PA11 = AF_PP@50MHz (0xb << 12), PA12 = AF_PP@50MHz (0xb << 16)*/
    GPIOA->CRH = (GPIOA->CRH & ~(0xFFu << 12)) | ((0xBBu) << 12);
}

static void App_Usb_HL_NvicConfig(void)
{
    NVIC_InitTypeDef nvic;
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    nvic.NVIC_IRQChannel                   = USB_LP_CAN1_RX0_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
}

/* ====================================================================
 * ST 库钩子: 由通用 USB 库 (usb_pwr.c / usb_int.c) 调用
 * ====================================================================*/
/* PowerOn() 在 CustomHID_init 末尾调用 (bDeviceState = UNCONNECTED 之前)
 * 这里在 PowerOn 之前开中断, 模拟原顺序.*/
void EnableUSBInt(void)
{
    /* 在 CustomHID_init 内 PowerOn() 之前开中断
     * ST 库的 CustomHID_init 流程 (usb_init.c:113):
     * pInformation->Current_Configuration = 0;
     * PowerOn();
     * _SetISTR(0);
     * wInterrupt_Mask = IMR_MSK;
     * _SetCNTR(wInterrupt_Mask);
     * bDeviceState = UNCONNECTED;
     * 中断开关在 _SetCNTR 之后, 但 NVIC 应在更早开启.*/
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
}

/* ====================================================================
 * ST 库回调 (经 usb_conf.h 宏映射, 实际函数名 EPx_IN_User / EPx_OUT_User)
 * usb_istr.c pEpInt_IN[] 用标识符 EP1_IN_Callback 等初始化, 然后由
 * usb_conf.h 把标识符 #define 成这里定义的 _User 函数。
 * ====================================================================*/
/* EP1 IN 完成回调: 设备→主机 发送完成, 清 TxBusy*/
void EP1_IN_User(void)
{
    App_Usb_TxBusy = 0;
    App_Usb_Transport_OnTxComplete();   /* 清 transport 层 TxBusy,
                                         * 让 Proto_TxResponse 的等待循环能正确同步*/
}

/* EP2 OUT 完成回调: 主机→设备 接收完成, 取走 PMA 数据*/
void EP2_OUT_User(void)
{
    uint16_t count = (uint16_t)GetEPRxCount(ENDP2);
    if (count > 0 && count <= USB_RX_BUF_SZ) {
        PMAToUserBufferCopy(App_Usb_RxBuf, ENDP2_RXADDR, count);
        /* protocol-layer feeding: Proto_Poll 在主循环解析帧并回调*/
        App_Usb_OnReceive(App_Usb_RxBuf, count);
    }
    SetEPRxValid(ENDP2);
}

/* ====================================================================
 * 公开 API
 * ====================================================================*/
void App_Usb_HL_Init(uint32_t reg_base)
{
    (void)reg_base;

    /* 顺序 (时钟由 System_PeriphClkInit -> 引脚 -> NVIC -> USB_Init)*/
    App_Usb_HL_GpioInit();
    App_Usb_HL_NvicConfig();

    /* 触发 ST 库: PowerOn -> 设置中断屏蔽*/
    USB_Init();

    /* Sys_Init() 末尾 Sys_EnableUsb():
     * 拉高 USB_EN=PA8 激活 D+ 上拉电阻, 主机开始枚举。
     * 必须在 USB_Init 完成 + NVIC 准备好之后, 否则主机收到无效状态。*/
    GPIO_SetBits(USB_EN_GPIO_PORT, USB_EN_GPIO_PIN);
}

void App_Usb_HL_DeInit(void)
{
    _SetCNTR(CNTR_PDWN | CNTR_FRES);
    /* DISABLE 保留在 HL DeInit: 单外设回收, 不引入与全局开关的对偶依赖*/
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, DISABLE);
    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    App_Usb_Configured = 0;
}

void App_Usb_HL_Connect(void)   { }
void App_Usb_HL_Disconnect(void) { _SetCNTR(CNTR_PDWN | CNTR_FRES); }
void App_Usb_HL_Poll(void)      { /* 中断驱动*/ }

uint8_t App_Usb_HL_IsConfigured(void)
{
  /* bDeviceState == CONFIGURED (3) is set by ST lib CustomHID_SetConfiguration hook.
     * The App_Usb_Configured local var is never set to 1, so we read bDeviceState directly.*/
  extern vu32 bDeviceState;
  return (bDeviceState == 3u);
}

/* 发包 (ReportID 0x02 + payload)
 * 同步由调用方 Proto_TxResponse 保证, HL 层不再等
 * EP1 IN wMaxPacketSize=64, UserToPMABufferCopy 只复制 len+1 字节,
 * PMA 尾部保留上次长响应残留 (短响应后会泄漏上次尾字节, 如版本串/UID hash).
 * 改为清 TxBuf 尾部 + 复制满 64B, 保证 IN report 尾部恒为 0.*/
void App_Usb_HL_Transmit(const uint8_t *data, uint16_t len)
{
    extern vu32 bDeviceState;
    (void)bDeviceState;  /* DIAG: skip CONFIGURED check temporarily*/
    if (len > USB_RX_BUF_SZ - 1) len = USB_RX_BUF_SZ - 1;

    App_Usb_TxBuf[0] = 0x02;   /* IN Report ID*/
    for (uint16_t i = 0; i < len; i++) App_Usb_TxBuf[i + 1] = data[i];
    /* 清尾部, 防止短帧残留上次长响应数据*/
    for (uint16_t i = len + 1; i < USB_RX_BUF_SZ; i++) App_Usb_TxBuf[i] = 0;

    App_Usb_TxBusy = 1;
    UserToPMABufferCopy(App_Usb_TxBuf, ENDP1_TXADDR, USB_RX_BUF_SZ);
    SetEPTxValid(ENDP1);
}