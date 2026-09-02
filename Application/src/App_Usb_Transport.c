/* * App_Usb_Transport.c - USB Transport Adapter for CustomProtocol
 * Bridges USB HID with the protocol layer*/
#include "App_Usb.h"
#include "App_CustomProtocol.h"
#include "App_Led_HL.h"
#include "App_SysTick_HL.h"   /* SysTickHl_GetMs (分片间等待) */
#include <stddef.h>

/* USB transport buffer for protocol layer*/
#define USB_TRANSPORT_RX_BUF_SIZE  512
#define USB_TX_CHUNK               63u   /* 单 IN report 有效数据上限 (64B-1B ReportID) */

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

/* * @brief Start USB TX
 * 协议帧上限 PROTO_MAX_DATA=1024 (帧总长 ~1035B), 而单个 HID IN report 仅容
 * 63B 有效数据 — RX 方向的跨 report 流式重组早已实现, TX 方向此前却是整帧
 * 交 App_Usb_HL_Transmit 在 63B 处静默截断 (Round_098 BUG#2: AM WAVE_PAGE
 * 满 48 点页 65B 帧无响应, GET_TAGS/INVENTORY 多标签响应同病).
 * 修复: 在此按 63B 分片, 每片 [ReportID 0x02 + 63B] 独立 SetEPTxValid,
 * 片间等 EP1 IN 完成中断清 App_Usb_TxBusy (与主机端 HidLink.recv_frame
 * 逐 report 剥 0x02 前缀累加重组的语义正好互逆). 片间等待上限 1s: 实测
 * 主机 hidapi 取走一片 IN 报文可延迟 >64ms (Round_098 回归中 50ms 上限
 * 间歇性截断满页 WAVE_PAGE 帧), 而在 EP 仍 VALID 时强行 SetEPTxValid 会
 * 被 USB IP 忽略且 PMA 覆写在途分片 — 宁等不可抢发. 帧总长 ~1035B 最坏
 * 17 片也远小于主机侧秒级接收窗口. */
static void UsbTransport_TxDma(const uint8_t *buf, uint16_t len)
{
    /* 置 s_u8TxBusy=1, 由 EP1_IN_User -> App_Usb_Transport_OnTxComplete 清零.
     * Proto_TxResponse 的 while(TxIsBusy()) 据此等待前一个 USB IN 包发完,
     * 否则连续 SetEPTxValid 会被 STM32 USB IP 忽略 (EP_TX_VALID 状态不可重入),
     * 导致背靠背响应丢包.*/
    s_u8TxBusy = 1;
    if (len == 0u) return;

    uint16_t off = 0u;
    while (off < len) {
        uint16_t n = (uint16_t)(len - off);
        if (n > USB_TX_CHUNK) n = USB_TX_CHUNK;
        /* 片间等待: 首片时上一帧早已发完 (调用方 WaitEp1Ready 保证), 直接过;
         * EP_TX_VALID 不可重入 — 未等 IN 完成就 SetEPTxValid 会被 USB IP
         * 丢弃且 PMA 覆写会污染在途分片, 宁等 1s 不可抢发 (见函数头注释). */
        extern volatile uint8_t App_Usb_TxBusy;
        uint32_t t0 = SysTickHl_GetMs();
        while (App_Usb_TxBusy && (SysTickHl_GetMs() - t0) < 1000u) { /* spin */ }
        App_Usb_Transmit(&buf[off], n);
        off += n;
    }
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
