#ifndef __APP_MOTOR_HL_H
#define __APP_MOTOR_HL_H

#include <stdint.h>
#include "drv8434s.h"

/* =====================================================================
 * 步进电机 HAL 层 (DRV8434S 底层 SPI/GPIO 移植)
 *   - 自选 SPI2:  SCK=PB13, MISO=PB14, MOSI=PB15, NSS=PB12(手动 CS)
 *   - nFAULT 输入 = PB6 (1=正常, 0=故障)
 *   - nSLEEP 输出 = PB7 (1=运行, 0=睡眠)
 *   - 移植了 DRV8434S 需要的 4 个 HAL 函数 (见 drv8434s.h "HAL 底层接口")
 *   - 配套实现: Application/Drv8434S/{drv8434s.c,drv8434s.h} (SDK 副本, 原样)
 *   用法: Drv8434S_HL_Init() -> drv8434s_init(&g_hMotor, &cfg)
 * ===================================================================== */

/* 本层外设句柄 (对 SDK 而言是透明 void*, 这里具体化为本层端口结构). */
typedef struct {
    uint32_t dummy;   /* 占位: 本实现无跨帧状态, 仅作为稳定取址的句柄载体 */
} drv8434s_port_t;
extern drv8434s_port_t g_hMotor;

/* 初始化 SPI2 + 控制引脚 (nFAULT/nSLEEP/CS) + SPI2 外设 (Mode1: CPOL=0 CPHA=1,
 * 8bit 硬件只发低位优先由 SDK 手动换序, 8192kHz/16)。 */
void     Drv8434S_HL_Init(void);

/* ---- 供 DRV8434S SDK 调用的 4 个 HAL 回调 (实现, 勿直接调) ---- */
uint16_t drv8434s_hal_spi_xfer(void *handle, uint16_t word);
void     drv8434s_hal_delay_ms(uint32_t ms);
uint8_t  drv8434s_hal_read_fault(void *handle);
void     drv8434s_hal_set_pin(void *handle, uint8_t pin_index, uint8_t level);

#endif /* __APP_MOTOR_HL_H */
