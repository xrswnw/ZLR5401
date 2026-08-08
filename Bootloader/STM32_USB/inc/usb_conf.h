/******************** (C) COPYRIGHT 2008 STMicroelectronics ********************
* File Name          : usb_conf.h
* Author             : MCD Application Team
* Version            : V2.2.1
* Date               : 09/22/2008
* Description        : Custom HID demo configuration file
********************************************************************************
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*******************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USB_CONF_H
#define __USB_CONF_H

/* explicit include to declare NOP_Process and ENDPx_ADDR */
#include "stm32f10x.h"
#include "usb_core.h"

#define USB_FRAME_LEN		64  //usb数据的长度
//只需修改这两个长度，就可以改变收发长度的大小
//其实用到这两个宏的文件有：
//main.c :数据操作发送
//desc.c :描述符配置
//endp.c :端点回调函数，数据收发
//prop.c :hid复位配置长度，这个必须的，别忘了

/* Includes ------------------------------------------------------------------*/
/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
/* External variables --------------------------------------------------------*/
/*-------------------------------------------------------------*/
/* EP_NUM */
/* defines how many endpoints are used by the device */
/*-------------------------------------------------------------*/
#define EP_NUM     (3)

/*-------------------------------------------------------------*/
/* --------------   Buffer Description Table  -----------------*/
/*-------------------------------------------------------------*/
/* buffer table base address */
/* buffer table base address */
#define BTABLE_ADDRESS      (0x00)

/* EP0  */
/* rx/tx buffer base address */
#define ENDP0_RXADDR        (0x18)  //0x40
#define ENDP0_TXADDR        (0x58)  //0x40

// EP1 TX (device->PC, IN)
#define ENDP1_TXADDR        (0x98)  //0x40
//EP2 输入 device->PC
#define ENDP2_RXADDR        (0xD8) //0xC8
/*-------------------------------------------------------------*/
/* -------------------   ISTR events  -------------------------*/
/*-------------------------------------------------------------*/
/* IMR_MSK */
/* mask defining which events has to be handled */
/* by the device application software */
#define IMR_MSK (CNTR_CTRM  | CNTR_WKUPM | CNTR_SUSPM | CNTR_ERRM  | CNTR_SOFM \
                 | CNTR_ESOFM | CNTR_RESETM )

/* CTR service routines */
/* associated to defined endpoints */
/* 注: usb_istr.c 用标识符名 EP1_IN_Callback / EP1_OUT_Callback 等初始化
 * pEpInt_IN[] / pEpInt_OUT[], 这里把标识符映射到实际函数。
 * 端点方向: EP1 IN (TX), EP2 OUT (RX) */
#define  EP1_IN_Callback   EP1_IN_User        /* Boot_Usb_HL.c: TX 完成清 tx_busy */
#define  EP2_OUT_Callback  EP2_OUT_User       /* Boot_Usb_HL.c: 主机下发的 RX 数据 */
#define  EP1_OUT_Callback  NOP_Process
#define  EP2_IN_Callback   NOP_Process
#define  EP3_IN_Callback   NOP_Process
#define  EP4_IN_Callback   NOP_Process
#define  EP5_IN_Callback   NOP_Process
#define  EP6_IN_Callback   NOP_Process
#define  EP7_IN_Callback   NOP_Process
#define  EP3_OUT_Callback  NOP_Process
#define  EP4_OUT_Callback  NOP_Process
#define  EP5_OUT_Callback  NOP_Process
#define  EP6_OUT_Callback  NOP_Process
#define  EP7_OUT_Callback  NOP_Process

#endif /*__USB_CONF_H*/

/******************* (C) COPYRIGHT 2008 STMicroelectronics *****END OF FILE****/

