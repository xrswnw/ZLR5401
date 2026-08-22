#ifndef __APP_MOTOR_HL_H
#define __APP_MOTOR_HL_H

#include <stdint.h>
#include "drv8434s.h"

/* =====================================================================
 * 步进电机 HAL 层 (DRV8434S 底层 SPI/GPIO 移植)
 *   - SPI3:  SCK=PB3, MISO=PB4(SDO), MOSI=PB5(SDI), NSS=PC0(手动 CS)
 *   - GPIO 控制: STEP=PB6, DIR=PB7, NSLEEP=PB8(1=运行), ENABLE=PB9(1=使能)
 *   - nFAULT 未连 MCU (板上接故障指示灯), 故障状态经 SPI 寄存器读取
 *   - 移植了 DRV8434S 需要的 4 个 HAL 函数 (见 drv8434s.h "HAL 底层接口")
 *   - 配套实现: Application/Drv8434S/{drv8434s.c,drv8434s.h} (SDK 副本, 原样)
 *   用法: Drv8434S_HL_Init() -> drv8434s_init(&g_hMotor, &cfg)
 * ===================================================================== */

/* 本层外设句柄 (对 SDK 而言是透明 void*, 这里具体化为本层端口结构). */
typedef struct {
    uint32_t dummy;   /* 占位: 本实现无跨帧状态, 仅作为稳定取址的句柄载体 */
} drv8434s_port_t;
extern drv8434s_port_t g_hMotor;

/* 初始化 SPI3 + 控制引脚 (NSCS/STEP/DIR/NSLEEP/ENABLE) + SPI3 外设
 * (Mode1: CPOL=0 CPHA=1, 8bit, 4.5MHz SCLK)。 */
void     Drv8434S_HL_Init(void);

/* ---- 供 DRV8434S SDK 调用的 4 个 HAL 回调 (实现, 勿直接调) ---- */
uint16_t drv8434s_hal_spi_xfer(void *handle, uint16_t word);
void     drv8434s_hal_delay_ms(uint32_t ms);
uint8_t  drv8434s_hal_read_fault(void *handle);
void     drv8434s_hal_set_pin(void *handle, uint8_t pin_index, uint8_t level);

#endif /* __APP_MOTOR_HL_H */
