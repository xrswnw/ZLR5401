/* * App_Usb_Transport.c - USB Transport Adapter for CustomProtocol
 * Bridges USB HID with the protocol layer*/
#include "App_Usb.h"
#include "App_CustomProtocol.h"
#include "App_Led_HL.h"
#include <stddef.h>

/* USB transport buffer for protocol layer*/
#define USB_TRANSPORT_RX_BUF_SIZE  512

static uint8_t s_au8RxBuf[USB_TRANSPORT_RX_BUF_SIZE];
static volatile uint16_t s_u16RxHead = 0;
static volatile uint16_t s_u16RxTail = 0;
static volatile uint8_t s_u8TxBusy = 0;

/* ==================== USB Transport Implementation ====================*/

/* * @brief Check how many bytes are available in the RX buffer*/
static uint16_t UsbTransport_RxAvailable(void)
{
    if (s_u16RxHead >= s_u16RxTail) {
        return s_u16RxHead - s_u16RxTail;
    } else {
        return USB_TRANSPORT_RX_BUF_SIZE - s_u16RxTail + s_u16RxHead;
    }
}

/* * @brief Read bytes from RX buffer*/
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

/* * @brief Check if TX is busy
 * 直接读 HL 层的 App_Usb_TxBusy (由 EP1_IN_User 在包真正发完后清零).*/
static uint8_t UsbTransport_TxIsBusy(void)
{
    extern volatile uint8_t App_Usb_TxBusy;
    return App_Usb_TxBusy;
}

/* * @brief Start USB TX*/
static void UsbTransport_TxDma(const uint8_t *buf, uint16_t len)
{
    /* 置 s_u8TxBusy=1, 由 EP1_IN_User -> App_Usb_Transport_OnTxComplete 清零.
     * Proto_TxResponse 的 while(TxIsBusy()) 据此等待前一个 USB IN 包发完,
     * 否则连续 SetEPTxValid 会被 STM32 USB IP 忽略 (EP_TX_VALID 状态不可重入),
     * 导致背靠背响应丢包.*/
    s_u8TxBusy = 1;
    App_Usb_Transmit(buf, len);
}

/* Indicate EP1 IN TX no longer in progress. Called from EP1_IN_User in App_Usb_HL.c.*/
void App_Usb_Transport_OnTxComplete(void) {
    s_u8TxBusy = 0;
}

/* Transport operations*/
static const ProtoTransport_t g_usbTransport = {
    .RxAvailable = UsbTransport_RxAvailable,
    .RxRead     = UsbTransport_RxRead,
    .TxIsBusy   = UsbTransport_TxIsBusy,
    .TxDma      = UsbTransport_TxDma,
};

/* ==================== Public API ====================*/

/* * @brief Get USB transport operations*/
const ProtoTransport_t *App_Usb_GetTransport(void)
{
    return &g_usbTransport;
}

/* * @brief Called by App_Usb_HL when HID OUT data is received
 * Stores data in buffer for protocol layer to read*/
void App_Usb_OnReceive(const uint8_t *data, uint16_t len)
{
    if (len == 0 || data == NULL) {
        return;
    }

    /* Skip report ID if present (HID report starts with report ID)*/
    uint16_t offset = 0;
    if (len > 0 && data[0] == 0x01) {
        /* Report ID 0x01 = OUT report from host*/
        offset = 1;
        len--;
    }

    if (len == 0) {
        return;
    }

    /* Write to circular buffer*/
    for (uint16_t i = 0; i < len; i++) {
        uint16_t nextHead = (s_u16RxHead + 1) % USB_TRANSPORT_RX_BUF_SIZE;
        if (nextHead == s_u16RxTail) {
            /* Buffer full, discard oldest data*/
            s_u16RxTail = (s_u16RxTail + 1) % USB_TRANSPORT_RX_BUF_SIZE;
        }
        s_au8RxBuf[s_u16RxHead] = data[offset + i];
        s_u16RxHead = nextHead;
    }
}
