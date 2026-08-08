/*
 * App_Usb.h - USB Application Layer for Application
 */
#ifndef __APP_USB_H
#define __APP_USB_H

#include <stdint.h>
#include "App_CustomProtocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== USB Initialization ==================== */
/**
 * @brief Initialize USB stack
 */
void App_Usb_Init(void);

/**
 * @brief USB main loop polling
 */
void App_Usb_Poll(void);

/* ==================== USB Status ==================== */
/**
 * @brief Check if USB is connected and configured
 * @return 1 if configured, 0 otherwise
 */
uint8_t App_Usb_IsConnected(void);

/* ==================== USB Transmission ==================== */
/**
 * @brief Send data via USB HID
 * @param data Data buffer
 * @param len Data length
 */
void App_Usb_Transmit(const uint8_t *data, uint16_t len);

/**
 * @brief Check if USB TX is busy
 * @return 1 if busy, 0 if idle
 */
uint8_t App_Usb_IsTxBusy(void);

/* ==================== Protocol Layer Integration ==================== */
/**
 * @brief Get USB transport operations for CustomProtocol
 * @return Pointer to ProtoTransport_t structure
 */
const ProtoTransport_t *App_Usb_GetTransport(void);

/* ==================== USB Receive Callback ==================== */
/**
 * @brief Called by USB hardware layer when data is received
 * @param data Received data buffer
 * @param len Received data length
 */
void App_Usb_OnReceive(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __APP_USB_H */
