#include "App_Motor_HL.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_spi.h"
/* 步进电机 DRV8434S HAL 移植层 (SPI3 + GPIO, 见原理图).
 * RCC 时钟由 System_PeriphClkInit() 统一开启 (APP_RCC_APB1_PERIPH 含 RCC_APB1Periph_SPI3).
 * GPIOB/GPIOC 时钟已含于 APP_RCC_APB2_PERIPH. */

/* 帧间隔 (nSCS 高电平) 保持时间, 用 SysTick 忙等近似 >= 500ns. */
#define MOTOR_CS_SETTLE_US   2u
/* SPI3 = APB1 = 36MHz, prescaler 8 -> 4.5MHz SCLK (DRV8434S SPI 上限约 10MHz). */
#define MOTOR_SPI_PRESC      SPI_BaudRatePrescaler_8

drv8434s_port_t g_hMotor;

/* ---- 手动片选 (NSCS=PD2) ---- */
static void motor_cs(uint8_t level)
{
    level ? GPIO_SetBits(MOTOR_NSCS_PORT, MOTOR_NSCS_PIN)
          : GPIO_ResetBits(MOTOR_NSCS_PORT, MOTOR_NSCS_PIN);
}

void Drv8434S_HL_Init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef  spi;

    /* 1) 控制引脚 (GPIOB): STEP/DIR/NSLEEP/ENABLE 推挽输出.
     *    NSLEEP 默认高=运行; DIR/STEP 默认低; ENABLE 默认高 (假设高电平使能, 见下). */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Pin   = MOTOR_STEP_PIN | MOTOR_DIR_PIN | MOTOR_NSLEEP_PIN | MOTOR_ENABLE_PIN;
    GPIO_Init(MOTOR_CTRL_GPIO_PORT, &gpio);
    GPIO_ResetBits(MOTOR_CTRL_GPIO_PORT, MOTOR_STEP_PIN | MOTOR_DIR_PIN | MOTOR_ENABLE_PIN);
    GPIO_SetBits(MOTOR_CTRL_GPIO_PORT, MOTOR_NSLEEP_PIN);

    /* 2) 片选 NSCS=PD2: 推挽输出, 空闲高 (低有效) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Pin   = MOTOR_NSCS_PIN;
    GPIO_Init(MOTOR_NSCS_PORT, &gpio);
    GPIO_SetBits(MOTOR_NSCS_PORT, MOTOR_NSCS_PIN);

    /* ENABLE 引脚使能: 高=运行 (假设高电平有效; 若硬件为低有效需反转).
     * 使能输出亦由 DRV8434S EN_OUT 寄存器管理, 此处只是把板上 ENABLE 脚置为运行态. */
    GPIO_SetBits(MOTOR_CTRL_GPIO_PORT, MOTOR_ENABLE_PIN);

    /* 3) SPI3 复用引脚: SCK=PB3, MISO=PB4(接 DRV SDO), MOSI=PB5(接 DRV SDI) (AF_PP) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin   = MOTOR_SPI_SCK_PIN | MOTOR_SPI_MISO_PIN | MOTOR_SPI_MOSI_PIN;
    GPIO_Init(MOTOR_SPI_GPIO_PORT, &gpio);

    /* 4) SPI3 外设: 主机, 8bit, MSB first, CPOL=0/CPHA=1 (Mode1, 下降沿捕获).
     * 帧为 16bit = 高字节(命令) + 低字节(数据), 分两次 8bit 发送 (先高后低, MSB 即帧首). */
    SPI_StructInit(&spi);
    spi.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode              = SPI_Mode_Master;
    spi.SPI_DataSize          = SPI_DataSize_8b;
    spi.SPI_CPOL              = SPI_CPOL_Low;
    spi.SPI_CPHA              = SPI_CPHA_2Edge;   /* Mode 1 */
    spi.SPI_NSS               = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = MOTOR_SPI_PRESC;
    spi.SPI_FirstBit          = SPI_FirstBit_MSB;
    SPI_Init(MOTOR_SPI, &spi);
    SPI_Cmd(MOTOR_SPI, ENABLE);
}

/* ---- 16-bit 全双工传输 (nSCS 自动拉低/拉高包围) ----
 * SDK 帧: 16bit = [W0][A4..A0][X][D7..D0], 高字节在前.
 * 两字节拆发后, RX 高字节 = (W0|A4..0|X), RX 低字节 = 回读寄存器内容 (8bit).
 * 回读只取低字节 (DRV8434S SDO 有效内容). */
uint16_t drv8434s_hal_spi_xfer(void *handle, uint16_t word)
{
    uint8_t txh = (uint8_t)(word >> 8);
    uint8_t txl = (uint8_t)(word & 0xFF);
    uint8_t rxh = 0, rxl = 0;
    volatile uint32_t i;
    (void)handle;

    motor_cs(0);
    /* 帧起始: 低有效建立 (器件在 nSCS 下降沿后采样) */
    for (i = 0; i < MOTOR_CS_SETTLE_US; i++) __NOP();

    while (SPI_I2S_GetFlagStatus(MOTOR_SPI, SPI_I2S_FLAG_TXE) == RESET) {}
    SPI_I2S_SendData(MOTOR_SPI, txh);
    while (SPI_I2S_GetFlagStatus(MOTOR_SPI, SPI_I2S_FLAG_RXNE) == RESET) {}
    rxh = (uint8_t)SPI_I2S_ReceiveData(MOTOR_SPI);

    while (SPI_I2S_GetFlagStatus(MOTOR_SPI, SPI_I2S_FLAG_TXE) == RESET) {}
    SPI_I2S_SendData(MOTOR_SPI, txl);
    while (SPI_I2S_GetFlagStatus(MOTOR_SPI, SPI_I2S_FLAG_RXNE) == RESET) {}
    rxl = (uint8_t)SPI_I2S_ReceiveData(MOTOR_SPI);

    while (SPI_I2S_GetFlagStatus(MOTOR_SPI, SPI_I2S_FLAG_BSY) == SET) {}

    for (i = 0; i < MOTOR_CS_SETTLE_US; i++) __NOP();
    motor_cs(1);

    return (uint16_t)((uint16_t)rxh << 8) | rxl;
}

void drv8434s_hal_delay_ms(uint32_t ms)
{
    SysTickHl_DelayMs(ms);
}

void drv8434s_hal_delay_us(uint32_t us)
{
    /* 72MHz 下约 72 周期/us; 每条 __NOP 约 1~2 周期, 用 ~0.03us/步近似.
     * 只需满足 STEP 时序 >=970ns, 精度要求宽, 无需精确计时. */
    volatile uint32_t i;
    uint32_t n = us * (uint32_t)40u;   /* 经验系数, ~覆盖 1us 量级 */
    for (i = 0; i < n; i++) __NOP();
}

uint8_t drv8434s_hal_read_fault(void *handle)
{
    /* 原理图上 DRV8434S nFAULT 接板载故障指示灯, 未连 MCU GPIO.
     * 故障经 SPI 寄存器读取 (drv8434s_get_fault_status), 此处恒报正常(1). */
    (void)handle;
    return 1u;
}

void drv8434s_hal_set_pin(void *handle, uint8_t pin_index, uint8_t level)
{
    (void)handle;
    uint16_t pin;
    switch (pin_index) {
    case DRV8434S_PIN_STEP:    pin = MOTOR_STEP_PIN;    break;
    case DRV8434S_PIN_DIR:     pin = MOTOR_DIR_PIN;     break;
    case DRV8434S_PIN_ENABLE:  pin = MOTOR_ENABLE_PIN;  break;
    case DRV8434S_PIN_SLEEP:   pin = MOTOR_NSLEEP_PIN;  break;
    default:                   return;
    }
    level ? GPIO_SetBits(MOTOR_CTRL_GPIO_PORT, pin)
          : GPIO_ResetBits(MOTOR_CTRL_GPIO_PORT, pin);
}
