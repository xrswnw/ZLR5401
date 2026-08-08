/*
 * App_Usb_HL.h - USB Hardware Layer for Application
 */
#ifndef __APP_USB_HL_H
#define __APP_USB_HL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== USB VID/PID Configuration ==================== */
/* 权威宏: 设备描述符 usb_desc.c 在编译期引用这些宏, 改这里即改 VID/PID */
#define APP_USB_VID              0x5377
#define APP_USB_PID              0x5378
#define APP_USB_MAX_POWER        100
#define APP_USB_LANGID_STRING    1033

/* VID/PID 拆成 LE 字节 (USB 协议规定小端序), 供 usb_desc.c 数组初始化使用 */
#define APP_USB_VID_LE_LO        ((uint8_t)(APP_USB_VID        & 0xFFu))
#define APP_USB_VID_LE_HI        ((uint8_t)((APP_USB_VID >> 8) & 0xFFu))
#define APP_USB_PID_LE_LO        ((uint8_t)(APP_USB_PID        & 0xFFu))
#define APP_USB_PID_LE_HI        ((uint8_t)((APP_USB_PID >> 8) & 0xFFu))

/* ==================== USB Endpoints ==================== */
#define APP_USB_EP0_ADDR         0x00
#define APP_USB_HID_IN_EP        0x81
#define APP_USB_HID_OUT_EP       0x02

/* ==================== USB Buffer Sizes ==================== */
#define APP_USB_EP0_BUF_SIZE     64
#define APP_USB_HID_IN_BUF_SIZE   64
#define APP_USB_HID_OUT_BUF_SIZE  64

/* ==================== USB HID Report Descriptor Size ==================== */
#define APP_USB_HID_REPORT_DESC_SIZE  40

/* ==================== USB Initialization ==================== */
/**
 * @brief Initialize USB hardware
 * @param reg_base USB peripheral base address
 */
void App_Usb_HL_Init(uint32_t reg_base);

/**
 * @brief Deinitialize USB hardware
 */
void App_Usb_HL_DeInit(void);

/**
 * @brief Connect USB (enable pull-up)
 */
void App_Usb_HL_Connect(void);

/**
 * @brief Disconnect USB (disable pull-up)
 */
void App_Usb_HL_Disconnect(void);

/* ==================== USB Polling ==================== */
/**
 * @brief USB main loop polling function
 * Call this in main loop
 */
void App_Usb_HL_Poll(void);

/* ==================== USB Status ==================== */
uint8_t App_Usb_HL_IsConfigured(void);

/* ==================== USB Transmission ==================== */
/**
 * @brief Send data via USB HID
 */
void App_Usb_HL_Transmit(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __APP_USB_HL_H */
