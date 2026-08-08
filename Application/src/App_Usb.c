/*
 * App_Usb.c - USB Application Layer Implementation
 */
#include "App_Usb.h"
#include "App_Usb_HL.h"
#include "App_CustomProtocol.h"
#include "stm32f10x_gpio.h"

/* Forward declaration */
extern void Proto_OnDataFromUsb(const uint8_t *data, uint16_t len);

/* ==================== USB Initialization ==================== */
void App_Usb_Init(void)
{
    /* Toggle PB5 LED to show USB init started */
    GPIOB->ODR ^= GPIO_Pin_5;  /* Toggle PB5 (LED) */

    /* Initialize USB hardware layer */
    App_Usb_HL_Init(0x40005C00UL);

    /* Register USB as a transport channel for CustomProtocol */
    Proto_RegisterTransport(PROTO_CH_USB, App_Usb_GetTransport());
}

/* ==================== USB Polling ==================== */
void App_Usb_Poll(void)
{
    App_Usb_HL_Poll();
}

/* ==================== USB Status ==================== */
uint8_t App_Usb_IsConnected(void)
{
    return App_Usb_HL_IsConfigured();
}

/* ==================== USB Transmission ==================== */
void App_Usb_Transmit(const uint8_t *data, uint16_t len)
{
    App_Usb_HL_Transmit(data, len);
}

uint8_t App_Usb_IsTxBusy(void)
{
    return 0;
}
