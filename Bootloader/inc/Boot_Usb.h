/*
 * Boot_Usb.h - USB Application Layer for Bootloader
 */
#ifndef __BOOT_USB_H
#define __BOOT_USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== USB Initialization ==================== */
void Boot_Usb_Init(void);
void Boot_Usb_Poll(void);

/* ==================== USB Status ==================== */
uint8_t Boot_Usb_IsConnected(void);

/* ==================== USB Transmission ==================== */
void Boot_Usb_Transmit(const uint8_t *data, uint16_t len);
uint8_t Boot_Usb_IsTxBusy(void);

/* ==================== Protocol Layer Integration ==================== */
const void *Boot_Usb_GetTransport(void);

/* ==================== USB Receive Callback ==================== */
void Boot_Usb_OnReceive(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_USB_H */
