/* * Boot_Usb.c - USB Application Layer Implementation for Bootloader*/
#include "Boot_Usb.h"
#include "Boot_Usb_HL.h"
#include "Boot_CustomProtocol.h"
#include <stddef.h>

/* USB transport buffer*/
#define USB_TRANSPORT_RX_BUF_SIZE  512

static uint8_t s_au8RxBuf[USB_TRANSPORT_RX_BUF_SIZE];
static volatile uint16_t s_u16RxHead = 0;
static volatile uint16_t s_u16RxTail = 0;
static volatile uint8_t s_u8TxBusy = 0;

/* Forward declaration*/
extern void Proto_OnDataFromUsb(const uint8_t *data, uint16_t len);

/* ==================== USB Transport Implementation ====================*/
static uint16_t UsbTransport_RxAvailable(void)
{
    if (s_u16RxHead >= s_u16RxTail) {
        return s_u16RxHead - s_u16RxTail;
    } else {
        return USB_TRANSPORT_RX_BUF_SIZE - s_u16RxTail + s_u16RxHead;
    }
}

static uint16_t UsbTransport_RxRead(uint8_t *buf, uint16_t maxlen)
{
    uint16_t avail = UsbTransport_RxAvailable();
    if (avail == 0) {
        return 0;
    }

    if (maxlen > avail) {
        maxlen = avail;
    }

    for (uint16_t i = 0; i < maxlen; i++) {
        buf[i] = s_au8RxBuf[s_u16RxTail];
        s_u16RxTail = (s_u16RxTail + 1) % USB_TRANSPORT_RX_BUF_SIZE;
    }

    return maxlen;
}

/* 同步 App_Usb_Transport.c TxBusy 由 EP1_IN_User 发送完成中断异步清零,
 * 不在 TxDma 内立即清零. 否则 Proto_TxResponse 的 while(TxIsBusy()) 不等待前一个
 * USB IN 包发完, 连续 SetEPTxValid 被 STM32 USB IP 忽略 (EP_TX_VALID 不可重入),
 * 背靠背响应丢包.*/
extern volatile uint8_t g_boot_usb_tx_busy;

static uint8_t UsbTransport_TxIsBusy(void)
{
    return g_boot_usb_tx_busy;
}

static void UsbTransport_TxDma(const uint8_t *buf, uint16_t len)
{
    if (g_boot_usb_tx_busy) {
        return;
    }

    s_u8TxBusy = 1;
    Boot_Usb_HL_Transmit(buf, len);
    /* 不在此清零: 由 EP1_IN_User -> Boot_Usb_Transport_OnTxComplete 异步清零*/
}

/* EP1 IN 发送完成中断回调 (由 Boot_Usb_HL.c EP1_IN_User 调用)*/
void Boot_Usb_Transport_OnTxComplete(void)
{
    s_u8TxBusy = 0;
}

static const ProtoTransport_t g_usbTransport = {
    .RxAvailable = UsbTransport_RxAvailable,
    .RxRead     = UsbTransport_RxRead,
    .TxIsBusy   = UsbTransport_TxIsBusy,
    .TxDma      = UsbTransport_TxDma,
};

/* ==================== Public API ====================*/
void Boot_Usb_Init(void)
{
    Boot_Usb_HL_Init();
    /* 注册 USB 为协议传输通道 (Boot 层走 USB HID, 与 App 对齐)*/
    Proto_RegisterTransport(PROTO_CH_USB, (const ProtoTransport_t *)Boot_Usb_GetTransport());
}

const void *Boot_Usb_GetTransport(void)
{
    return &g_usbTransport;
}

void Boot_Usb_Poll(void)
{
    Boot_Usb_HL_Poll();
}

uint8_t Boot_Usb_IsConnected(void)
{
    return Boot_Usb_HL_IsConfigured();
}

void Boot_Usb_Transmit(const uint8_t *data, uint16_t len)
{
    if (g_boot_usb_tx_busy) {
        return;
    }

    s_u8TxBusy = 1;
    Boot_Usb_HL_Transmit(data, len);
    /* 异步清零: EP1_IN_User -> Boot_Usb_Transport_OnTxComplete*/
}

uint8_t Boot_Usb_IsTxBusy(void)
{
    return g_boot_usb_tx_busy;
}

void Boot_Usb_OnReceive(const uint8_t *data, uint16_t len)
{
    if (len == 0 || data == NULL) {
        return;
    }

    /* Skip report ID if present*/
    uint16_t offset = 0;
    if (len > 0 && data[0] == 0x01) {
        offset = 1;
        len--;
    }

    if (len == 0) {
        return;
    }

    /* Write to circular buffer. 协议处理交给主循环 Proto_Poll 异步取 (RxAvailable/RxRead),
     * 对齐 App_Usb_Transport.c App_Usb_OnReceive (不在此喂协议层).
     * 此前在中断(EP2_OUT_User)里同步调 Proto_OnDataFromUsb -> BootDispatch
     * -> Proto_TxResponse -> Boot_Usb_HL_Transmit -> SetEPTxValid, 其中 WaitEp1Ready
     * spin 等 EP_TX_VALID 清除, 而清除需 IN 完成中断 (当前正占着 USB 中断, 进不来)
     * -> 死等 50ms 超时 -> USB 子系统时序崩 -> 后续 OUT 全超时, 通信卡死.
     * App 无此行 = 协议处理在主循环上下文, WaitEp1Ready spin 安全. Boot 对齐.*/
    for (uint16_t i = 0; i < len; i++) {
        uint16_t nextHead = (s_u16RxHead + 1) % USB_TRANSPORT_RX_BUF_SIZE;
        if (nextHead == s_u16RxTail) {
            s_u16RxTail = (s_u16RxTail + 1) % USB_TRANSPORT_RX_BUF_SIZE;
        }
        s_au8RxBuf[s_u16RxHead] = data[offset + i];
        s_u16RxHead = nextHead;
    }
}
