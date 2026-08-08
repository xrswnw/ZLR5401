#ifndef __CONFIG_H
#define __CONFIG_H

/* 必须先 include stm32f10x.h (定义 __STM32F10x_H),
 * 这样后续 include 的 STM32_USB/usb_type.h 才能跳过自身的 typedef 块.*/
#include "stm32f10x.h"
#include <stdint.h>

/* 类型别名, 统一映射到 stdint*/
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef int8_t    s8;
typedef int16_t  s16;
typedef int32_t  s32;
/* BOOL 定义. 与 STM32_USB/usb_type.h 共享 (均 typedef unsigned char BOOL),
 * 用 BOOL_DEFINED 守卫避免重复.*/
#ifndef BOOL_DEFINED
typedef unsigned char BOOL;
#define BOOL_DEFINED
#endif
#ifndef TRUE
#define TRUE    1u
#endif
#ifndef FALSE
#define FALSE   0u
#endif

/* ===================== MCU =====================*/
/* 硬件为 GD32F103C8T6 (与 STM32F103 引脚/寄存器兼容, 使用 STM32F10x 标准外设库)*/
#define MCU_SERIES         STM32F10X_MD
#define MCU_PART           "GD32F103C8T6"

/* ===================== Clock =====================*/
/* 本设备晶振为 12MHz (非默认 8MHz)*/
#define HSE_VALUE_CFG      12000000U
#define SYS_CLK_MHZ        72

/* ===================== Flash Partition =====================
 * C8T6: Flash 64KB @0x08000000, 页大小 1KB(MD)。
 * 分区: BOOT代码(22K) + PARAM(2K预留, 实占512B) + APP(40K, 回收顶部2K)。
 * 0x08000000 BOOT_CODE 22K
 * 0x08005800 PARAM 2K (主256B + 影子256B, 余1.5K空置)
 * 0x08006000 APP 40K
 * 0x08010000 Flash末尾
 * BOOT_FLASH_SIZE = BOOT_CODE_SIZE + PARAM_REGION_SIZE = 24K (APP_ORIGIN据此推导)*/
#define FLASH_PAGE_SIZE     1024U              /* HD=2KB, MD(C8T6)=1KB*/
#define BOOT_FLASH_ORIGIN   0x08000000U
#define BOOT_CODE_SIZE      (22U * 1024U)      /* BOOT 代码区(链接脚本 FLASH LENGTH)*/
#define PARAM_FLASH_ORIGIN  (BOOT_FLASH_ORIGIN + BOOT_CODE_SIZE)  /* 0x08005800, 升级不擦除*/
#define PARAM_FLASH_SIZE    512U              /* 实际参数: 主区256B + 影子区256B*/
#define PARAM_REGION_SIZE   (2U * 1024U)      /* 链接脚本预留 PARAM 区(含余量)*/
#define BOOT_FLASH_SIZE     (BOOT_CODE_SIZE + PARAM_REGION_SIZE)  /* 24K = 22K code + 2K param*/
#define APP_FLASH_ORIGIN    (BOOT_FLASH_ORIGIN + BOOT_FLASH_SIZE) /* 0x08006000*/
#define APP_FLASH_SIZE      (40U * 1024U)     /* 回收顶部2K: 0x08006000~0x08010000*/
#define APP_FLASH_END       (APP_FLASH_ORIGIN + APP_FLASH_SIZE)  /* 0x08010000 = Flash末尾*/

#define RAM_ORIGIN          0x20000000U
#define RAM_SIZE            (20U * 1024U)

/* ===================== 步进电机 DRV8434S (SPI2 自选引脚, 见 Drv_Stepper_HL) =====================
 * DRV8434S 16-bit SPI 控制 (Mode1: CPOL=0, CPHA=1), SPI_STEP/SPI_DIR 走 SPI, 无需 STEP/DIR 引脚。
 * 引脚自选(避开 USB PA1/PA11/PA12、LED PB4/PB5、SWD PA13~15):
 *   SCK=PB13, MISO=PB14, MOSI=PB15, NSS=PB12(手动 GPIO 片选, 帧间高电平>=500ns)
 *   nFAULT 输入 = PB6 (1=正常, 0=故障)    nSLEEP 输出 = PB7 (1=运行, 0=睡眠)
 * 需 RCC_APB1Periph_SPI2 + GPIOB。*/
#define MOTOR_SPI               SPI2
#define MOTOR_SPI_SCK_PIN       GPIO_Pin_13
#define MOTOR_SPI_MISO_PIN      GPIO_Pin_14
#define MOTOR_SPI_MOSI_PIN      GPIO_Pin_15
#define MOTOR_SPI_CS_PIN        GPIO_Pin_12
#define MOTOR_SPI_GPIO_PORT     GPIOB
#define MOTOR_NFAULT_PIN        GPIO_Pin_6
#define MOTOR_NSLEEP_PIN        GPIO_Pin_7
#define MOTOR_CTRL_GPIO_PORT    GPIOB

/* ===================== LED (单灯: 绿灯 PB4, 高电平点亮) =====================*/
#define LED_G_GPIO_PORT    GPIOB
#define LED_G_GPIO_PIN    GPIO_Pin_4

/* ===================== USB_EN =====================*/

/* ===================== UHF 模块 SIM7500 (USART1, 自选引脚) =====================
 * 接口: USART1 (PA9=TX / PA10=RX), 115200-8-N-1 (TTL)
 * 控制: UHF_EN=PA8 (高电平上电), UHF_ANT=PC13 (0=ANT1 / 1=ANT2)
 * 引脚自选避开: USB PA1/PA11/PA12、LED PB4/PB5、SWD PA13~15、SPI2 PB12~15。*/
#define UHF_USART               USART1
#define UHF_BAUD                115200U
#define UHF_TX_GPIO_PORT        GPIOA
#define UHF_TX_GPIO_PIN         GPIO_Pin_9
#define UHF_RX_GPIO_PORT        GPIOA
#define UHF_RX_GPIO_PIN         GPIO_Pin_10
#define UHF_EN_GPIO_PORT        GPIOA
#define UHF_EN_GPIO_PIN         GPIO_Pin_8
#define UHF_ANT_GPIO_PORT       GPIOC
#define UHF_ANT_GPIO_PIN        GPIO_Pin_13
/* 需求: USB_EN=PA1, 拉高才启用 USB (D+ 上拉)。
 * 依据: D3232GZ 工作参考工程的 USB_ENABLE_PORT = {GPIOA, GPIO_Pin_1}。
 * 硬件实测: 用 PB3(高电平) 固件设备不枚举; PA1 才是激活主机可见 D+ 上拉的脚。*/
#define USB_EN_GPIO_PORT   GPIOA
#define USB_EN_GPIO_PIN    GPIO_Pin_1

/* ===================== AM 解码器 (USART2, 自选引脚) =====================
 * 接口: USART2 (PA2=TX / PA3=RX), 115200-8-N-1 (TTL)
 * 协议: 2A A2 帧, 校验 = 命令+包数+包次+包长+数据 & 0xFF
 * 引脚自选避开: USB PA1/PA11/PA12、LED PB4/PB5、SWD PA13~15、UHF PA9/10、
 *                SPI2 PB12~15。USART2 位于 APB1 (36MHz)。*/
#define AM_USART                USART2
#define AM_BAUD                 115200U
#define AM_TX_GPIO_PORT         GPIOA
#define AM_TX_GPIO_PIN          GPIO_Pin_2
#define AM_RX_GPIO_PORT         GPIOA
#define AM_RX_GPIO_PIN          GPIO_Pin_3

/* ===================== 外设 RCC 时钟集合 (统一在 System_PeriphClkInit 开启) =====================*/
/* 一次性开启, 替代各 HL 层分散调用 RCC_APBxPeriphClockCmd / RCC_AHBPeriphClockCmd。
 * DISABLE 仍由各 HL 的 DeInit 自行处理 (单外设回收)。
 * 当前仅保留: LED(TIM3_CH1 PWM, PB4) + USB(HID)。 */
#define APP_RCC_APB2_PERIPH    (RCC_APB2Periph_GPIOA  | RCC_APB2Periph_GPIOB | \
                                RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO   | \
                                RCC_APB2Periph_USART1)
#define APP_RCC_APB1_PERIPH    (RCC_APB1Periph_USB | \
                                RCC_APB1Periph_TIM3 | \
                                RCC_APB1Periph_SPI2 | \
                                RCC_APB1Periph_USART2) /* USB HID; TIM3_CH1 LED 呼吸 PWM (PB4); SPI2 步进电机; USART2 AM 解码器*/

/* ===================== Boot Timeout =====================*/
#define BOOT_TIMEOUT_MS     500U

/* ===================== IWDG =====================
 * Boot 启用 IWDG 后, JumpToApp 为直接跳转(非复位), IWDG 持续到 App,
 * 故 App 必须同步启用并在主循环/故障循环喂狗, 否则跳 App 后约 2s 复位死循环.
 * 超时与 Boot 一致 2000ms, 主循环每次迭代喂狗.*/
#define USE_IWDG              1
#define IWDG_TIMEOUT_MS       2000U
#define IWDG_COUNTER_RATE     40000U
#define IWDG_RELOAD_DIV       64U

/* ===================== Device Version Info =====================*/
#define DEV_HW_VERSION      "C8T6_V1.0"
#define DEV_SW_VERSION      "ZLR5401_V1.0"
#define DEV_BOOT_VERSION    "BOOT_V1.0"
#define VER_STR_LEN         16U

/* ===================== Debug =====================*/
/* #define USE_FULL_ASSERT 1*/

#endif /* __CONFIG_H*/
