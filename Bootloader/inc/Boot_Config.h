#ifndef __CONFIG_H
#define __CONFIG_H

/* 必须先 include stm32f10x.h*/
#include "stm32f10x.h"

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

/* ===================== LED (单灯: 绿灯 PB4, 高电平点亮) =====================*/
#define LED_GPIO_PORT    GPIOB
#define LED_GPIO_PIN    GPIO_Pin_4

/* ===================== 外设 RCC 时钟集合 (统一在 System_PeriphClkInit 开启) =====================*/
/* 一次性开启, 替代各 HL 层分散调用 RCC_APBxPeriphClockCmd / RCC_AHBPeriphClockCmd。
 * DISABLE 仍由各 HL 的 DeInit 自行处理 (单外设回收)。
 * 当前仅保留: LED(GPIOB) + USB(HID, GPIOA/GPIOB/AFIO)。 */
#define BOOT_RCC_APB2_PERIPH   (RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | \
                                RCC_APB2Periph_AFIO)
#define BOOT_RCC_APB1_PERIPH   (RCC_APB1Periph_USB)

/* ===================== Boot Timeout =====================
 * 500 -> 2500ms. 仅影响"无 UPG 标志的冷启动"路径 (主机来不及
 * 枚举就被跳走). 已置 PARAM_STATUS_UPG 的 IAP 流程仍为无限等待, 不受影响.
 * 2500ms 覆盖 Mac/Win 1~3s 的 USB 枚举时延余量.*/
#define BOOT_TIMEOUT_MS     2500U

/* ===================== IWDG =====================
 * 升级态开启独立看门狗. Boot 一旦启动 IWDG 无法停止(仅复位清除),
 * JumpToApp 为直接跳转(非复位) -> IWDG 持续到 App, 故 App 必须同步启用并喂狗
 * (见 App_Config.h / Application/src/main.c). 超时 2000ms 覆盖:
 * - Boot 长擦除: 每页擦后 IwdgHl_Feed() (Boot_Dispatch.c), 单页 ~30~60ms << 2s;
 * - ParamSave 重序列(cpsid i): ~35ms << 2s.*/
#define USE_IWDG              1
#define IWDG_TIMEOUT_MS       2000U
#define IWDG_COUNTER_RATE     40000U
#define IWDG_RELOAD_DIV       64U

/* ===================== Device Version Info =====================*/
//注意、注意这部分参数不要动，给我自行修改
#define DEV_HW_VERSION      "C8T6_V1.0"           /* 硬件版本,16B补零*/
#define DEV_SW_VERSION      "ZLR5401_V1.0"               /* 软件版本,16B补零*/
#define DEV_BOOT_VERSION    "BOOT_V1.0"               /* BOOT版本,16B补零*/
#define VER_STR_LEN         16U                  /* 版本字符串长度*/

/* ===================== Debug =====================*/
/* #define USE_FULL_ASSERT 1*/

#endif /* __CONFIG_H*/
