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
 * 硬件 GD32F303RCT6 (Cortex-M4, 256K flash @0x08000000~0x08040000, 页 2KB HD)。
 * 分区: BOOT代码(22K) + PARAM(2K预留, 实占512B) + APP(232K)。
 * 0x08000000 BOOT_CODE 22K
 * 0x08005800 PARAM 2K (主256B + 影子256B, 余1.5K空置)
 * 0x08006000 APP 232K
 * 0x08040000 Flash末尾
 * BOOT_FLASH_SIZE = BOOT_CODE_SIZE + PARAM_REGION_SIZE = 24K (APP_ORIGIN据此推导)
 * 注: APP_FLASH_SIZE 曾为 40K(STM32F103 C8T6 旧假设), App 涨到 57KB 后
 * Reset 向量 0x080100f4 越过 0x08010000 范围校验导致 JumpToApp 拒绝 -> 卡 Boot。
 * 已改回 RCT6 真实容量 232K。*/
#define FLASH_PAGE_SIZE     2048U              /* HD=2KB (GD32F303 RCT6/STM32F103 HD)*/
#define BOOT_FLASH_ORIGIN   0x08000000U
#define BOOT_CODE_SIZE      (22U * 1024U)      /* BOOT 代码区(链接脚本 FLASH LENGTH)*/
#define PARAM_FLASH_ORIGIN  (BOOT_FLASH_ORIGIN + BOOT_CODE_SIZE)  /* 0x08005800, 升级不擦除*/
#define PARAM_FLASH_SIZE    512U              /* 实际参数: 主区256B + 影子区256B*/
#define PARAM_REGION_SIZE   (2U * 1024U)      /* 链接脚本预留 PARAM 区(含余量)*/
#define BOOT_FLASH_SIZE     (BOOT_CODE_SIZE + PARAM_REGION_SIZE)  /* 24K = 22K code + 2K param*/
#define APP_FLASH_ORIGIN    (BOOT_FLASH_ORIGIN + BOOT_FLASH_SIZE) /* 0x08006000*/
#define APP_FLASH_SIZE      (232U * 1024U)    /* RCT6 256K - 前置24K: 0x08006000~0x08040000*/
#define APP_FLASH_END       (APP_FLASH_ORIGIN + APP_FLASH_SIZE)  /* 0x08040000 = Flash末尾*/

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
 * RUN=PA2: 呼吸用 TIM2_CH3 硬件 PWM (1kHz, PA2 默认映射无需重映射), AppLedProcess 持续驱动。
 * ERR=PA3: 故障/状态指示, 默认灭; 直接 GPIO 开关。*/
#define LED_RUN_GPIO_PORT    GPIOA
#define LED_RUN_GPIO_PIN     GPIO_Pin_2
#define LED_ERR_GPIO_PORT    GPIOA
#define LED_ERR_GPIO_PIN     GPIO_Pin_3

/* ===================== RGB 三色灯 (G=PA4 / R=PA5 / B=PA6, 高电平点亮) =====================
 * 三色灯直接 GPIO 开关 (无调光), 经 RgbLedHl_Set(掩码) 设置位.
 * 颜色位掩码: G=bit0, R=bit1, B=bit2. 协议 RGB 命令按此位序下发 (见 App_Protocol.h).
 * 注: B=PA6 已由 USB_EN 迁 PA8 后释放, 蓝灯回归 PA6 正常驱动 (PA7 空闲). */
#define RGB_G_GPIO_PORT     GPIOA
#define RGB_G_GPIO_PIN      GPIO_Pin_4
#define RGB_R_GPIO_PORT     GPIOA
#define RGB_R_GPIO_PIN      GPIO_Pin_5
#define RGB_B_GPIO_PORT     GPIOA
#define RGB_B_GPIO_PIN      GPIO_Pin_6
#define RGB_BIT_G           0x01U   /* 绿 */
#define RGB_BIT_R           0x02U   /* 红 */
#define RGB_BIT_B           0x04U   /* 蓝 */

/* ===================== RGB 灯语 (App_RgbLed_Pattern) =====================
 * 灯带为开关型 GPIO (无调光), 图样 = 颜色掩码 + 闪烁周期/亮窗。
 * RGB_HAS_BLUE=1: 蓝灯硬件回归 PA6 (USB_EN 已迁 PA8), 白/青/粉红全量生效。 */
#define RGB_HAS_BLUE            1
#define RGB_PAT_BLINK_MS        1000U  /* 扫描/软标/回零 慢闪周期 */
#define RGB_PAT_FAST_MS         500U   /* 行程测试快闪周期 */
#define RGB_PAT_SWERR_MS        500U   /* 行程开关错误闪烁周期 (沿用旧值) */
#define RGB_MANUAL_HOLD_MS      10000U /* 上位机手动设色保持时长 */

/* ===================== USB_EN =====================*/
/* 需求: USB_EN=PA8, 拉高才启用 USB (D+ 上拉)。
 * 前序: 早期 PC10/PC9 -> PA6(LED_BLUE) -> 现依用户要求迁 PA8
 *       (原 AM RS485 方向控制脚; AM 已改 RS232, PA8 释放)。
 * PA6 已释放回归 RGB 蓝灯 (见 RGB_B_*)。*/
#define USB_EN_GPIO_PORT   GPIOA
#define USB_EN_GPIO_PIN    GPIO_Pin_8

/* ===================== UHF 模块 SIM7500 (USART3, 见原理图) =====================
 * 接口: USART3 (PB10=TX / PB11=RX), 115200-8-N-1 (TTL)
 * 注: UHF 已从 UART4(PC10/11) 回归 USART3(PB10/11); PC10/PC11 释放。
 * 引脚(见原理图): USART3_TX=PB10, USART3_RX=PB11  (AF_PP)
 *   UHF_EN=PB12 (高电平上电), UHF_OUT2=PB13, UHF_IN1=PB14, UHF_IN2=PB15,
 *   UHF_NRST=PC6, UHF_OUT1=PC7
 * 板上无 MCU 天线切换脚 (SIM7500 ANT 在模块上, 天线选择经模块命令), 故无 UHF_ANT。*/
#define UHF_USART               USART3
#define UHF_BAUD                115200U
#define UHF_TX_GPIO_PORT        GPIOB
#define UHF_TX_GPIO_PIN         GPIO_Pin_10
#define UHF_RX_GPIO_PORT        GPIOB
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

/* ===================== AM 消磁器/解码器 -> RS232 (USART1, 见原理图) =====================
 * 接口: USART1 (PA9=TXD / PA10=RXD), 115200-8-N-1, 全双工 RS232 (TTL 电平)。
 * 协议: 2A A2 帧, 校验 = 命令+包数+包次+包长+数据 & 0xFF
 * 注: AM 已由 RS485(半双工+PA8方向控制) 改为 RS232, 无方向控制脚;
 *     PA8 已释放改作 USB_EN (见 USB_EN_GPIO_*)。*/
#define AM_USART                USART1
#define AM_BAUD                 115200U
#define AM_TX_GPIO_PORT         GPIOA
#define AM_TX_GPIO_PIN          GPIO_Pin_9
#define AM_RX_GPIO_PORT         GPIOA
#define AM_RX_GPIO_PIN          GPIO_Pin_10

/* ===================== 新增外设 (见原理图, 驱动骨架) =====================
 * 光电接口 (TLP181): MCU_IR1_DET=PC4 (输入, 检测红外/光电)
 * 行程开关: MCU_KEY_UP=PC8 (输入), 下行程 KEY_DOWN=PC9 已改作 USB_EN (见 USB_EN_GPIO_*)
 * 蜂鸣器:   MCU_BEEP5V0_CTL=PC12 (输出, 高电平响)
 * 调试串口: DEBUG_TX=PC10 (UART4_TX), DEBUG_RX=PC11 (UART4_RX)
 * 注: UHF 已回归 USART3(PB10/11), PC10/PC11 释放可作调试串口; 默认关闭 (APP_DEBUG_SERIAL_EN=0)。*/
#define APP_DEBUG_SERIAL_EN     0   /* 1=使能调试串口(UART4/PC10,11), 0=关闭(保留代码)*/
#define IR_DET_GPIO_PORT        GPIOC
#define IR_DET_GPIO_PIN         GPIO_Pin_4
#define KEY_UP_GPIO_PORT        GPIOC
#define KEY_UP_GPIO_PIN         GPIO_Pin_8
/* 下行程 KEY_DOWN=PC9 已恢复 (USB_EN 已迁 PA8, PC9 释放, 恢复为输入) */
#define KEY_DOWN_GPIO_PORT      GPIOC
#define KEY_DOWN_GPIO_PIN       GPIO_Pin_9
#define BEEP_GPIO_PORT          GPIOC
#define BEEP_GPIO_PIN           GPIO_Pin_13
/* 旧调试串口 UART4(PC10/11): UHF 已回归 USART3, 引脚释放; 定义仍注释禁用 (保留代码引用) */
/* #define DBG_USART               UART4 */
/* #define DBG_BAUD                115200U */
/* #define DBG_TX_GPIO_PORT        GPIOC */
/* #define DBG_TX_GPIO_PIN         GPIO_Pin_10 */
/* #define DBG_RX_GPIO_PORT        GPIOC */
/* #define DBG_RX_GPIO_PIN         GPIO_Pin_11 */

/* ===================== 外设 RCC 时钟集合 (统一在 System_PeriphClkInit 开启) =====================*/
/* 一次性开启, 替代各 HL 层分散调用 RCC_APBxPeriphClockCmd / RCC_AHBPeriphClockCmd。
 * DISABLE 仍由各 HL 的 DeInit 自行处理 (单外设回收)。
 * 当前仅保留: LED(TIM2_CH3 PWM, PA2) + USB(HID)。 */
#define APP_RCC_APB2_PERIPH    (RCC_APB2Periph_GPIOA  | RCC_APB2Periph_GPIOB | \
                                RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | \
                                RCC_APB2Periph_AFIO   | \
                                RCC_APB2Periph_USART1) /* USART1=RS232(AM消磁器) */
/* UHF 已回归 USART3 (UHF_HL_Init 内自行使能 RCC_APB1Periph_USART3);
 * 旧调试串口(UART4)功能仍注释禁用, 不由宏统一开时钟。*/
#define APP_DEBUG_SERIAL_EN     0   /* 旧调试串口 UART4 已禁用 (PC10/11 已释放) */
#define APP_RCC_APB1_PERIPH    (RCC_APB1Periph_USB | \
                                RCC_APB1Periph_TIM2 | \
                                RCC_APB1Periph_TIM4 | \
                                RCC_APB1Periph_SPI3) /* USB HID; TIM2 CH3=PA2 RUN灯硬件PWM; TIM4 STEP硬件脉冲; SPI3 步进电机*/
                                /* USART3 时钟由 UHF_HL_Init 自行开启 */

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
