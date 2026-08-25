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
/* 硬件为 GD32F303RCT6 高密度 (Cortex-M4, 256K flash/48K RAM), 与 STM32F103 引脚/寄存器兼容,
 * 使用 STM32F10x 标准外设库. HD 使能 UART4/DMA2 等外设中断向量. */
#define MCU_SERIES         STM32F10X_HD
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

/* ===================== 步进电机 DRV8434S (SPI3 + GPIO, 见原理图) =====================
 * DRV8434S 16-bit SPI 配置寄存器 (Mode1: CPOL=0, CPHA=1) + 硬件 STEP/DIR 引脚控制。
 * 引脚(见原理图):
 *   SCLK=PB3(SPI3_SCK), SDI=PB5(SPI3_MOSI→DRV SDI), SDO=PB4(SPI3_MISO←DRV SDO)
 *   NSCS=PD2(手动 GPIO 片选, 帧间高电平>=500ns), STEP=PB6, DIR=PB7
 *   NSLEEP=PB8(1=运行,0=睡眠), ENABLE=PB9(1=使能)
 *   nFAULT 在板上接故障指示灯, 未连 MCU; 故障状态经 SPI 读取。
 * 需 RCC_APB1Periph_SPI3 + GPIOB + GPIOD。*/
#define MOTOR_SPI               SPI3
#define MOTOR_SPI_SCK_PIN       GPIO_Pin_3
#define MOTOR_SPI_MISO_PIN      GPIO_Pin_4
#define MOTOR_SPI_MOSI_PIN      GPIO_Pin_5
#define MOTOR_SPI_GPIO_PORT     GPIOB
#define MOTOR_NSCS_PORT         GPIOD
#define MOTOR_NSCS_PIN          GPIO_Pin_2
#define MOTOR_STEP_PIN          GPIO_Pin_6
#define MOTOR_DIR_PIN           GPIO_Pin_7
#define MOTOR_NSLEEP_PIN        GPIO_Pin_8
#define MOTOR_ENABLE_PIN        GPIO_Pin_9
#define MOTOR_CTRL_GPIO_PORT    GPIOB

/* ===================== LED (RUN 呼吸灯 PA2 + ERR 故障灯 PA3, 高电平点亮) =====================
 * RUN=PA2: 呼吸用软件 PWM (TIM3 时基 + 中断翻转), AppLedProcess 持续驱动。
 * ERR=PA3: 故障/状态指示, 默认灭; 无定时器通道, 直接 GPIO 开关。*/
#define LED_RUN_GPIO_PORT    GPIOA
#define LED_RUN_GPIO_PIN     GPIO_Pin_2
#define LED_ERR_GPIO_PORT    GPIOA
#define LED_ERR_GPIO_PIN     GPIO_Pin_3

/* ===================== RGB 三色灯 (G=PA4 / R=PA5 / B=PA6) =====================
 * 三色灯直接 GPIO 开关 (无调光), 经 RgbLedHl_Set(掩码) 设置位.
 * 颜色位掩码: G=bit0, R=bit1, B=bit2. 协议 RGB 命令按此位序下发 (见 App_Protocol.h).
 * 注: B=PA6 已改作 USB_EN(LED_BLUE), 蓝灯位不再由 RGB 驱动, 仅供引脚/注释语义保留;
 *     RGB 驱动只操作 G/R, 蓝灯位忽略 (见 App_RgbLed_HL.c).*/
#define RGB_G_GPIO_PORT     GPIOA
#define RGB_G_GPIO_PIN      GPIO_Pin_4
#define RGB_R_GPIO_PORT     GPIOA
#define RGB_R_GPIO_PIN      GPIO_Pin_5
#define RGB_B_GPIO_PORT     GPIOA
#define RGB_B_GPIO_PIN      GPIO_Pin_6
#define RGB_BIT_G           0x01U   /* 绿 */
#define RGB_BIT_R           0x02U   /* 红 */
#define RGB_BIT_B           0x04U   /* 蓝 (PA6 现作 USB_EN, RGB 不驱动) */

/* ===================== USB_EN =====================*/

/* ===================== UHF 模块 SIM7500 (UART4, 见原理图) =====================
 * 接口: UART4 (PC10=TX / PC11=RX), 115200-8-N-1 (TTL)
 * 注: UHF 原 USART3(PB10/11) 已改为 UART4(PC10/11); 旧调试串口(UART4)功能被注释禁用。
 * 引脚(见原理图): UART4_TX=PC10, UART4_RX=PC11  (AF_PP)
 *   UHF_EN=PB12 (高电平上电), UHF_OUT2=PB13, UHF_IN1=PB14, UHF_IN2=PB15,
 *   UHF_NRST=PC6, UHF_OUT1=PC7
 * 板上无 MCU 天线切换脚 (SIM7500 ANT 在模块上, 天线选择经模块命令), 故无 UHF_ANT。*/
#define UHF_USART               UART4
#define UHF_BAUD                115200U
#define UHF_TX_GPIO_PORT        GPIOC
#define UHF_TX_GPIO_PIN         GPIO_Pin_10
#define UHF_RX_GPIO_PORT        GPIOC
#define UHF_RX_GPIO_PIN         GPIO_Pin_11
#define UHF_EN_GPIO_PORT        GPIOB
#define UHF_EN_GPIO_PIN         GPIO_Pin_12
#define UHF_REG_GPIO_PORT       GPIOB
#define UHF_REG_OUT2_PIN        GPIO_Pin_13
#define UHF_REG_IN1_PIN         GPIO_Pin_14
#define UHF_REG_IN2_PIN         GPIO_Pin_15
#define UHF_NRST_GPIO_PORT      GPIOC
#define UHF_NRST_GPIO_PIN       GPIO_Pin_6
#define UHF_OUT1_GPIO_PORT      GPIOC
#define UHF_OUT1_GPIO_PIN       GPIO_Pin_7
/* 需求: USB_EN=PA6 (LED_BLUE/RGB蓝), 拉高才启用 USB (D+ 上拉)。
 * 前序: 早期 PC10/PC9 均实测过枚举; 现依用户要求迁至 PA6 (RGB 蓝灯路),
 *       PA6 只作 USB 使能, 不再由 RGB 驱动蓝灯位 (见 App_RgbLed_HL.c)。
 * 释放出的 PC9 恢复为下行程开关 KEY_DOWN (见 KEY_DOWN_*)。*/
#define USB_EN_GPIO_PORT   GPIOA
#define USB_EN_GPIO_PIN    GPIO_Pin_6

/* ===================== AM 解码器 -> RS485 (USART1 + SP3485, 见原理图) =====================
 * 接口: USART1 (PA9=TXD / PA10=RXD), 115200-8-N-1 (TTL), 半双工 485。
 * 方向控制: RS485_CTL1=PA8 (SP3485 DE/RE: 发送=1, 接收=0)
 * 协议: 2A A2 帧, 校验 = 命令+包数+包次+包长+数据 & 0xFF
 * 注意: AM 数据经 RS485 收发, 发送前须拉高 PA8 方向, 发完拉低回接收态。*/
#define AM_USART                USART1
#define AM_BAUD                 115200U
#define AM_TX_GPIO_PORT         GPIOA
#define AM_TX_GPIO_PIN          GPIO_Pin_9
#define AM_RX_GPIO_PORT         GPIOA
#define AM_RX_GPIO_PIN          GPIO_Pin_10
#define AM_DIR_GPIO_PORT        GPIOA
#define AM_DIR_GPIO_PIN         GPIO_Pin_8

/* ===================== 新增外设 (见原理图, 驱动骨架) =====================
 * 光电接口 (TLP181): MCU_IR1_DET=PC4 (输入, 检测红外/光电)
 * 行程开关: MCU_KEY_UP=PC8 (输入), 下行程 KEY_DOWN=PC9 已改作 USB_EN (见 USB_EN_GPIO_*)
 * 蜂鸣器:   MCU_BEEP5V0_CTL=PC12 (输出, 高电平响)
 * 调试串口: DEBUG_TX=PC10 (UART4_TX), DEBUG_RX=PC11 (UART4_RX)
 * 注: PC10 已改作 USB_EN; PC9 已恢复为下行程 KEY_DOWN; 调试串口默认关闭 (APP_DEBUG_SERIAL_EN=0)。*/
#define APP_DEBUG_SERIAL_EN     0   /* 1=使能调试串口(UART4/PC10,11), 0=关闭(保留代码)*/
#define IR_DET_GPIO_PORT        GPIOC
#define IR_DET_GPIO_PIN         GPIO_Pin_4
#define KEY_UP_GPIO_PORT        GPIOC
#define KEY_UP_GPIO_PIN         GPIO_Pin_8
/* 下行程 KEY_DOWN=PC9 已恢复 (USB_EN 已迁 PA6, PC9 释放, 恢复为输入) */
#define KEY_DOWN_GPIO_PORT      GPIOC
#define KEY_DOWN_GPIO_PIN       GPIO_Pin_9
#define BEEP_GPIO_PORT          GPIOC
#define BEEP_GPIO_PIN           GPIO_Pin_13
/* 旧调试串口 UART4(PC10/11): 引脚已被 UHF 占用, 定义注释禁用 (保留代码引用) */
/* #define DBG_USART               UART4 */
/* #define DBG_BAUD                115200U */
/* #define DBG_TX_GPIO_PORT        GPIOC */
/* #define DBG_TX_GPIO_PIN         GPIO_Pin_10 */
/* #define DBG_RX_GPIO_PORT        GPIOC */
/* #define DBG_RX_GPIO_PIN         GPIO_Pin_11 */

/* ===================== 外设 RCC 时钟集合 (统一在 System_PeriphClkInit 开启) =====================*/
/* 一次性开启, 替代各 HL 层分散调用 RCC_APBxPeriphClockCmd / RCC_AHBPeriphClockCmd。
 * DISABLE 仍由各 HL 的 DeInit 自行处理 (单外设回收)。
 * 当前仅保留: LED(TIM3_CH1 PWM, PB4) + USB(HID)。 */
#define APP_RCC_APB2_PERIPH    (RCC_APB2Periph_GPIOA  | RCC_APB2Periph_GPIOB | \
                                RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | \
                                RCC_APB2Periph_AFIO   | \
                                RCC_APB2Periph_USART1) /* USART1=RS485(AM) */
/* UART4 已改作 UHF 串口 (UHF_HL_Init 内自行使能 RCC_APB1Periph_UART4);
 * 旧调试串口(UART4)功能已注释禁用, 不再由宏统一开时钟。*/
#define APP_DEBUG_SERIAL_EN     0   /* 旧调试串口 UART4 已禁用(引脚被 UHF 占用) */
#define APP_RCC_APB1_PERIPH    (RCC_APB1Periph_USB | \
                                RCC_APB1Periph_TIM3 | \
                                RCC_APB1Periph_SPI3) /* USB HID; TIM3 LED软PWM时基; SPI3 步进电机*/
                                /* UART4 时钟由 UHF_HL_Init 自行开启 */

/* ===================== Boot Timeout =====================*/
#define BOOT_TIMEOUT_MS     500U

/* ===================== IWDG =====================
 * Boot 启用 IWDG 后, JumpToApp 为直接跳转(非复位), IWDG 持续到 App,
 * 故 App 必须同步启用并在主循环/故障循环喂狗, 否则跳 App 后约 2s 复位死循环.
 * 超时与 Boot 一致 2000ms, 主循环每次迭代喂狗.*/
#define USE_IWDG              0
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
