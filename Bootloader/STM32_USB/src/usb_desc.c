/******************** (C) COPYRIGHT 2008 STMicroelectronics ********************
* File Name : usb_desc.c
* Author : MCD Application Team
* Version : V2.2.1
* Date : 09/22/2008
* Description : Descriptors for Custom HID Demo
********************************************************************************
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*******************************************************************************/

/* Includes ------------------------------------------------------------------*/
#include "usb_lib.h"
#include "usb_desc.h"
/* BOOT_USB_VID_LE_* / BOOT_USB_PID_LE_* 来自 Boot_Usb_HL.h, 在编译期拆分成 LE 字节*/
#include "Boot_Usb_HL.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Extern variables ----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* USB Standard Device Descriptor*/
const u8 CustomHID_DeviceDescriptor[CUSTOMHID_SIZ_DEVICE_DESC] =
  {
    0x12,                       /*bLength*/
    USB_DEVICE_DESCRIPTOR_TYPE, /*bDescriptorType*/
    0x00,                       /*bcdUSB*/
    0x02,
    0x00,                       /*bDeviceClass*/
    0x00,                       /*bDeviceSubClass*/
    0x00,                       /*bDeviceProtocol*/
    USB_FRAME_LEN,              /*bMaxPacketSize 40*/
    BOOT_USB_VID_LE_LO,        /*idVendor LE low ← BOOT_USB_VID*/
    BOOT_USB_VID_LE_HI,        /*idVendor LE high*/
    BOOT_USB_PID_LE_LO,        /*idProduct LE low ← BOOT_USB_PID*/
    BOOT_USB_PID_LE_HI,        /*idProduct LE high*/
    0x00,                       /*bcdDevice rel. 1.00*/
    0x02,
    1,                          /*Index of string descriptor describing
 manufacturer*/
    2,                          /*Index of string descriptor describing
 product*/
    3,                          /*Index of string descriptor describing the
 device serial number*/
    0x01                        /*bNumConfigurations*/
  }
  ; /* CustomHID_DeviceDescriptor*/

/* USB Configuration Descriptor*/
/* All Descriptors (Configuration, Interface, Endpoint, Class, Vendor*/
const u8 CustomHID_ConfigDescriptor[CUSTOMHID_SIZ_CONFIG_DESC] =
  {
    0x09, /* bLength: Configuation Descriptor size*/
    USB_CONFIGURATION_DESCRIPTOR_TYPE, /* bDescriptorType: Configuration*/
    CUSTOMHID_SIZ_CONFIG_DESC,
    /* wTotalLength: Bytes returned*/
    0x00,
    0x01,         /* bNumInterfaces: 1 interface*/
    0x01,         /* bConfigurationValue: Configuration value*/
    0x00,         /* iConfiguration: Index of string descriptor describing
 the configuration*/
    0xC0,         /* bmAttributes: Bus powered*/
                  /*Bus powered: 7th bit, Self Powered: 6th bit, Remote wakeup: 5th bit, reserved: 4..0 bits*/
    0x32,         /* MaxPower 100 mA: this current is used for detecting Vbus*/
// 0x96, /* MaxPower 300 mA: this current is used for detecting Vbus */
    /************** Descriptor of Custom HID interface ****************/
    /* 09*/
    0x09,         /* bLength: Interface Descriptor size*/
    USB_INTERFACE_DESCRIPTOR_TYPE,/* bDescriptorType: Interface descriptor type*/
    0x00,         /* bInterfaceNumber: Number of Interface*/
    0x00,         /* bAlternateSetting: Alternate setting*/
    0x02,         /* bNumEndpoints*/
    0x03,         /* bInterfaceClass: HID*/
    0x00,         /* bInterfaceSubClass : 1=BOOT, 0=no boot*/
    0x00,         /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse*/
    0,            /* iInterface: Index of string descriptor*/
    /******************** Descriptor of Custom HID HID ********************/
    /* 18*/
    0x09,         /* bLength: HID Descriptor size*/
    HID_DESCRIPTOR_TYPE, /* bDescriptorType: HID*/
    0x00,         /* bcdHID: HID Class Spec release number*/
    0x01,
    0x00,         /* bCountryCode: Hardware target country*/
    0x01,         /* bNumDescriptors: Number of HID class descriptors to follow*/
    0x22,         /* bDescriptorType*/
    CUSTOMHID_SIZ_REPORT_DESC,/* wItemLength: Total length of Report descriptor*/
    0x00,
    /******************** Descriptor of Custom HID endpoints ******************/
    /* 27*/
    0x07,          /* bLength: Endpoint Descriptor size*/
    USB_ENDPOINT_DESCRIPTOR_TYPE, /* bDescriptorType:*/

    0x81,          /* bEndpointAddress: Endpoint Address (IN), EA=1 (ENDP1)*/
    //端点2 输入 // bit 3...0 : the endpoint number
    //向PC发数据 // bit 6...4 : reserved
                    // bit 7 : 0(OUT), 1(IN)
    0x03,          /* bmAttributes: Interrupt endpoint*/
    USB_FRAME_LEN,  /* wMaxPacketSize:SendLength Bytes max*///向PC发送SendLength字节数据
    0x00,
    0x0A,          /* bInterval: Polling Interval (16ms)*/
    /* 34*/

    0x07,	/* bLength: Endpoint Descriptor size*/
    USB_ENDPOINT_DESCRIPTOR_TYPE,	/* bDescriptorType:*/
			/*	Endpoint descriptor type*/
    0x02,	/* bEndpointAddress: EA=2 (ENDP2 OUT)*/
	//接收pc发过来的数据 ReceiveLength个 字节
	//接收PC向单片机发送的数据
	0x03,	/* bmAttributes: Interrupt endpoint*/
    USB_FRAME_LEN,/* wMaxPacketSize: ReceiveLength Bytes max*/
    0x00,//接收pc发过来的数据 ReceiveLength个 字节
    0x0A,	/* bInterval: Polling Interval (16 ms)*/
    /* 41*/
  }
  ; /* CustomHID_ConfigDescriptor*/
const u8 CustomHID_ReportDescriptor[CUSTOMHID_SIZ_REPORT_DESC] =
{
#if 1
0x05, 0x8c, /* USAGE_PAGE (ST Page)*/
0x09, 0x01, /* USAGE (Demo Kit)*/
0xa1, 0x01, /* COLLECTION (Application)*/
/* 6*/

// The Input report
0x09,0x03, // USAGE ID - Vendor defined
0x15,0x00, // LOGICAL_MINIMUM (0)
0x25,0xFF, // LOGICAL_MAXIMUM (255)
0x75,0x08, // REPORT_SIZE (8)
0x95,USB_FRAME_LEN, // REPORT_COUNT :SendLength
0x81,0x02, // INPUT (Data,Var,Abs)
//19
// The Output report
0x09,0x04, // USAGE ID - Vendor defined
0x15,0x00, // LOGICAL_MINIMUM (0)
0x25,0xFF, // LOGICAL_MAXIMUM (255)
0x75,0x08, // REPORT_SIZE (8)
0x95,USB_FRAME_LEN, // REPORT_COUNT:ReceiveLength
0x91,0x02, // OUTPUT (Data,Var,Abs)
//32
// The Feature report

0x09, 0x05, // USAGE ID - Vendor defined
0x15,0x00, // LOGICAL_MINIMUM (0)
0x25,0xFF, // LOGICAL_MAXIMUM (255)
0x75,0x08, // REPORT_SIZE (8)
0x95,0x02, // REPORT_COUNT (2)
0xB1,0x02,

/* 45*/
0xc0 /* END_COLLECTION*/
#endif

#if 0
    //表示用途页为通用桌面设备
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    // COLLECTION (Application)
    0x09, 0x06,                    // USAGE (Keyboard)

    //分别从左ctrl键到右GUI键。这8个bits刚好构成一个字节，它位于报告的第一个字节。
    0xa1, 0x01,                    // COLLECTION (Application)

    //需要根据HID协议中规定的用途页表（HID Usage Tables）来确定。这里通常用来表示
    //特殊键，例如ctrl，shift，del键等
    //表示用途页为按键
    // USAGE_PAGE (Keyboard)
    //报告大小（即这个字段的宽度）为1bit，所以前面的逻辑最小值为0，逻辑最大值为1
    // REPORT_SIZE (1)
    //报告的个数为8，即总共有8个bits
    0x05, 0x07,                    // USAGE_PAGE (Keyboard)
    //用途最小值，这里为左ctrl键
    0x75, 0x01,                    // REPORT_SIZE (1)
    //用途最大值，这里为右GUI键，即window键
    0x95, 0x08,                    // REPORT_COUNT (8)
    //逻辑最小值为0
    0x19, 0xe0,                    // USAGE_MINIMUM (Keyboard LeftControl)
    //逻辑最大值为1
    0x29, 0xe7,                    // USAGE_MAXIMUM (Keyboard Right GUI)
    //输入用，变量，值，绝对值。像键盘这类一般报告绝对值，
    0x15, 0x00,                    // LOGICAL_MINIMUM (0)
    // INPUT (Data,Var,Abs)
    0x25, 0x01,                    // LOGICAL_MAXIMUM (1)
    //输入用，变量，值，绝对值。像键盘这类一般报告绝对值，
    //这样的数据段个数为1
    0x81, 0x02,                    // INPUT (Data,Var,Abs)

    // REPORT_SIZE (8)
    //输入用，常量，值，绝对值
    0x95, 0x01,                    // REPORT_COUNT (1)
    //每个段长度为8bits
    0x75, 0x08,                    // REPORT_SIZE (8)
    //有6个按键按下。没有按键按下时，全部返回0。如果按下的键太多，导致键盘扫描系统
    0x81, 0x03,                    // INPUT (Cnst,Var,Abs)

    //字节分别为相应的键值，以次类推。
    //报告个数为6
    // REPORT_COUNT (6)
    //每个段大小为8bits
    // REPORT_SIZE (8)
    //使用最小值为0
    0x95, 0x3E,                    // REPORT_COUNT (6)
    //使用最大值为0x65
    0x75, 0x08,                    // REPORT_SIZE (8)
    //逻辑最小值0
    0x19, 0x00,                    // USAGE_MINIMUM (Reserved (no event indicated))
    //逻辑最大值255
    0x29, 0x65,                    // USAGE_MAXIMUM (Keyboard Application)
    //输入用，变量，数组，绝对值
    0x15, 0x00,                    // LOGICAL_MINIMUM (0)
    //逻辑最大值255
    0x25, 0xFF,                    // LOGICAL_MAXIMUM (255)
    // USAGE_PAGE (LEDs)
    0x81, 0x00,                    // INPUT (Data,Ary,Abs)

    //每个段大小为1bit
    0x05, 0x08,                    // USAGE_PAGE (LEDs)
    //用途最小值是Num Lock，即数字键锁定灯
    0x95, 0x05,                    // REPORT_COUNT (5)
    //用途最大值是Kana，这个是什么灯我也不清楚^_^
    0x75, 0x01,                    // REPORT_SIZE (1)
    //如前面所说，这个字段是输出用的，用来控制LED。变量，值，绝对值。
    0x19, 0x01,                    // USAGE_MINIMUM (Num Lock)
    // OUTPUT (Data,Var,Abs)
    0x29, 0x05,                    // USAGE_MAXIMUM (Kana)
    // REPORT_COUNT (1)
    //每个段大小为3bits
    0x91, 0x02,                    // OUTPUT (Data,Var,Abs)
    //输出用，常量，值，绝对
    0x95, 0x01,                    // REPORT_COUNT (1)
    //由于要按字节对齐，而前面控制LED的只用了5个bit，
    0x75, 0x03,                    // REPORT_SIZE (3)
    //输出用，常量，值，绝对
    0x91, 0x03,                    // OUTPUT (Cnst,Var,Abs)
    // LOGICAL_MINIMUM (0)
    // LOGICAL_MAXIMUM (255)

    0x09, 0x05, // USAGE ID - Vendor defined
    0x15, 0x00, // LOGICAL_MINIMUM (0)
    0x25, 0xFF, // LOGICAL_MAXIMUM (255)
    0x75, 0x08, // REPORT_SIZE (8)
    0x95, 0x02, // REPORT_COUNT (2)
    0xB1, 0x02,

    //关集合，跟上面的对应
    0xc0                           // END_COLLECTION
#endif

#if 0
0x05,0x01,
0x09,0x06,
0xA1,0x01,

0x05,0x07,
0x19,0x00,
0x29,0xff,
0x15,0x00,
0x25,0xff,
0x75,0x01,
0x95,0x08,
0x91,0x02,

0x05,0x07,
0x19,0xE0,
0x29,0xE7,
0x95,0x08,
0x81,0x02,

0x75,0x08,
0x95,0x01,
0x81,0x01,

0x19,0x00,
0x29,0x93,
0x26,0xFF,0x00,
0x95,0x3E,
0x81,0x00,
0xC0

#endif

}; /* CustomHID_ReportDescriptor*/

/* USB String Descriptors (optional)*/
const u8 CustomHID_StringLangID[CUSTOMHID_SIZ_STRING_LANGID] =
  {
    CUSTOMHID_SIZ_STRING_LANGID,
    USB_STRING_DESCRIPTOR_TYPE,
    0x09,
    0x04
  }
  ; /* LangID = 0x0409: U.S. English*/

const u8 CustomHID_StringVendor[CUSTOMHID_SIZ_STRING_VENDOR] =
  {
    CUSTOMHID_SIZ_STRING_VENDOR, /* Size of Vendor string*/
    USB_STRING_DESCRIPTOR_TYPE,  /* bDescriptorType*/
    'S', 0,
    'W', 0,
    ' ', 0,
    'C', 0,
    'O', 0,
    'M', 0,
    'P', 0,
    'O', 0,
    'S', 0,
    'I', 0,
    'T', 0,
    'E', 0
  };

const u8 CustomHID_StringProduct[CUSTOMHID_SIZ_STRING_PRODUCT] =
{
    CUSTOMHID_SIZ_STRING_PRODUCT,           /* bLength = 14 (6 char + 2 header)*/
    USB_STRING_DESCRIPTOR_TYPE,             /* bDescriptorType*/
    'S', 0,
    'w', 0,
    'C', 0,
    'c', 0,
    'r', 0,
    'd', 0
};
u8 CustomHID_StringSerial[CUSTOMHID_SIZ_STRING_SERIAL] =
  {
    CUSTOMHID_SIZ_STRING_SERIAL,        /* bLength = 14*/
    USB_STRING_DESCRIPTOR_TYPE,         /* bDescriptorType*/
    'S', 0,
    'w', 0,
    'C', 0,
    'c', 0,
    'r', 0,
    'd', 0
  };

/******************* (C) COPYRIGHT 2008 STMicroelectronics *****END OF FILE****/

