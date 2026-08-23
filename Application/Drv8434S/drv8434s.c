/*
 * ============================================================================
 *  DRV8434S Stepper Motor Driver - C HAL/SDK Implementation
 * ============================================================================
 *  实现文件。与 drv8434s.h 配套使用。
 *
 *  依赖 HAL: drv8434s_hal_ms_delay / drv8434s_hal_spi_xfer /
 *           drv8434s_hal_read_fault / drv8434s_hal_set_pin
 *
 *  说明:
 *   - SPI 帧: 16bit = [W0][A4..A0][X][D7..D0]  (W0=0写, W0=1读)
 *   - SDO 回读: 前8bit 状态寄存器 S1, 后8bit Report (被访问寄存器内容)
 *   - 回读值低 8 位即所访问寄存器内容
 * ============================================================================
 */
#include "drv8434s.h"

/* 构建 SPI 16-bit 命令字 */
static uint16_t drv8434s_build_frame(uint8_t addr, uint8_t data, uint8_t is_read)
{
    uint16_t frame = 0;
    /* W0 at bit14 */
    if (is_read) frame |= (1u << 14);
    /* 5-bit address at bits 13..9 */
    frame |= ((uint16_t)(addr & 0x1F)) << 9;
    /* bit8 don't care */
    /* 8-bit data at bits 7..0 */
    frame |= (data & 0xFF);
    return frame;
}

/* 单寄存器写: 返回被写寄存器旧值 */
uint8_t drv8434s_write_reg(void *handle, uint8_t addr, uint8_t data)
{
    uint16_t frame = drv8434s_build_frame(addr, data, 0);
    uint16_t resp  = drv8434s_hal_spi_xfer(handle, frame);
    return (uint8_t)(resp & 0xFF);
}

/* 单寄存器读: 返回寄存器内容 */
uint8_t drv8434s_read_reg(void *handle, uint8_t addr)
{
    uint16_t frame = drv8434s_build_frame(addr, 0xFF, 1);
    uint16_t resp  = drv8434s_hal_spi_xfer(handle, frame);
    return (uint8_t)(resp & 0xFF);
}

/* --------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------*/
int drv8434s_init(void *handle, const drv8434s_config_t *cfg)
{
    uint8_t ctrl2 = 0, ctrl4 = 0, ctrl5 = 0, ctrl7 = 0;

    if (!cfg) return DRV8434S_ERR_PARAM;

    /* 确保解锁 (LOCK=011b) */
    uint8_t ctrl4_cur = drv8434s_read_reg(handle, DRV8434S_REG_CTRL4);
    ctrl4_cur &= ~DRV8434S_CTRL4_LOCK_MASK;
    ctrl4_cur |= (DRV8434S_LOCK_UNLOCKED << DRV8434S_CTRL4_LOCK_SHIFT);
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL4, ctrl4_cur);

    /* 清除残留故障 */
    drv8434s_clear_fault(handle);

    /* CTRL1: TRQ_DAC 默认设为 100% (0000b);
       需要精确满量程电流时请在 init 后调用 drv8434s_set_full_scale_current() */
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL1,
                       (uint8_t)(0x0 << DRV8434S_CTRL1_TRQ_DAC_SHIFT));

    /* CTRL2: EN_OUT + TOFF + DECAY */
    ctrl2 = (1u << 7);                       /* EN_OUT = 1 */
    ctrl2 |= ((uint8_t)cfg->toff & 0x03) << DRV8434S_CTRL2_TOFF_SHIFT;
    ctrl2 |= ((uint8_t)cfg->decay & 0x07) << DRV8434S_CTRL2_DECAY_SHIFT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL2, ctrl2);

    /* CTRL3: 使用 SPI 步进/方向, 设置微步模式 */
    {
        uint8_t ctrl3 = 0;
        ctrl3 |= DRV8434S_CTRL3_SPI_STEP;    /* 允许 SPI STEP (写 STEP 位步进) */
        ctrl3 |= DRV8434S_CTRL3_SPI_DIR;     /* 允许 SPI DIR */
        ctrl3 |= ((uint8_t)cfg->microstep & 0x0F) << DRV8434S_CTRL3_MICROSTEP_SHIFT;
        drv8434s_write_reg(handle, DRV8434S_REG_CTRL3, ctrl3);
    }

    /* CTRL4: 保护选项 */
    ctrl4  = (DRV8434S_LOCK_UNLOCKED << DRV8434S_CTRL4_LOCK_SHIFT);
    if (cfg->enable_ol)              ctrl4 |= DRV8434S_CTRL4_EN_OL;
    if (cfg->ocp_retry)              ctrl4 |= DRV8434S_CTRL4_OCP_MODE;
    if (cfg->otsd_auto_recover)      ctrl4 |= DRV8434S_CTRL4_OTSD_MODE;
    if (cfg->tw_report)              ctrl4 |= DRV8434S_CTRL4_TW_REP;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL4, ctrl4);

    /* CTRL5 + CTRL7: 失速检测 */
    ctrl5 = 0;
    if (cfg->enable_stall) {
        ctrl5 |= DRV8434S_CTRL5_EN_STL;
        if (cfg->stall_report) ctrl5 |= DRV8434S_CTRL5_STL_REP;
    }
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL5, ctrl5);

    /* 失速检测要求 smart tune ripple control, 若使能失速则强制该模式 */
    if (cfg->enable_stall) {
        ctrl2 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL2);
        ctrl2 &= ~DRV8434S_CTRL2_DECAY_MASK;
        ctrl2 |= DRV8434S_DECAY_SMART_TUNE_RIPPLE;
        drv8434s_write_reg(handle, DRV8434S_REG_CTRL2, ctrl2);
    }

    /* CTRL6 + CTRL7: STALL_TH (12bit) + 纹波 + 扩频 */
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL6,
                       (uint8_t)(cfg->stall_threshold & 0xFF));

    ctrl7  = ((uint8_t)cfg->ripple & 0x03) << DRV8434S_CTRL7_RC_RIPPLE_SHIFT;
    if (cfg->enable_ssc) ctrl7 |= DRV8434S_CTRL7_EN_SSC;
    ctrl7 |= (uint8_t)((cfg->stall_threshold >> 8) & 0x0F) << DRV8434S_CTRL7_STALL_TH_HI_SHIFT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL7, ctrl7);

    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * 使能/禁止输出
 * --------------------------------------------------------------------------*/
int drv8434s_set_enable(void *handle, drv8434s_enable_t en)
{
    uint8_t ctrl2 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL2);
    if (en == DRV8434S_ENABLED)
        ctrl2 |= DRV8434S_CTRL2_EN_OUT;
    else
        ctrl2 &= ~DRV8434S_CTRL2_EN_OUT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL2, ctrl2);
    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * 方向
 * --------------------------------------------------------------------------*/
int drv8434s_set_dir(void *handle, drv8434s_dir_t direction)
{
    uint8_t ctrl3 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL3);
    if (direction == DRV8434S_DIR_CCW)
        ctrl3 |= DRV8434S_CTRL3_DIR;
    else
        ctrl3 &= ~DRV8434S_CTRL3_DIR;
    /* 保持 SPI_DIR=1, 使用 SPI 方向; 或按引脚驱动 */
    ctrl3 |= DRV8434S_CTRL3_SPI_DIR;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL3, ctrl3);
    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * SPI 步进 (写 STEP 位自清零)
 * --------------------------------------------------------------------------*/
int drv8434s_spi_step(void *handle)
{
    uint8_t ctrl3 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL3);
    ctrl3 |= DRV8434S_CTRL3_STEP;
    ctrl3 |= DRV8434S_CTRL3_SPI_STEP;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL3, ctrl3);
    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * SPI 引脚单个步进脉冲 (硬件 STEP 引脚; 非阻塞, us 级)
 * 手册 6.7: tWH(STEP)/tWL(STEP) >= 970ns。这里用 ~5us 高低电平, 远大于下限,
 * 且不再阻塞主循环 (旧实现 1ms SysTick 忙等会拖垮步进节拍)。
 * --------------------------------------------------------------------------*/
void drv8434s_pin_step_pulse(void *handle)
{
    drv8434s_hal_set_pin(handle, DRV8434S_PIN_STEP, 1);
    drv8434s_hal_delay_us(5);
    drv8434s_hal_set_pin(handle, DRV8434S_PIN_STEP, 0);
    drv8434s_hal_delay_us(5);
}

/* --------------------------------------------------------------------------
 * 读状态 / 诊断
 * --------------------------------------------------------------------------*/
uint8_t drv8434s_get_fault_status(void *handle)
{
    return drv8434s_read_reg(handle, DRV8434S_REG_FAULT_STATUS);
}
uint8_t drv8434s_get_diag1(void *handle)
{
    return drv8434s_read_reg(handle, DRV8434S_REG_DIAG_STATUS1);
}
uint8_t drv8434s_get_diag2(void *handle)
{
    return drv8434s_read_reg(handle, DRV8434S_REG_DIAG_STATUS2);
}

/* --------------------------------------------------------------------------
 * 清除故障
 * --------------------------------------------------------------------------*/
int drv8434s_clear_fault(void *handle)
{
    uint8_t ctrl4 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL4);
    ctrl4 |= DRV8434S_CTRL4_CLR_FLT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL4, ctrl4);
    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * 转矩 DAC 百分比 (6.25% ~ 100%, 每档 -6.25%)
 * --------------------------------------------------------------------------*/
int drv8434s_set_torque_percent(void *handle, float percent)
{
    uint8_t n;
    if (percent > 100.0f) percent = 100.0f;
    if (percent < 6.25f)  percent = 6.25f;

    /* TRQ = 100% - n*6.25%  (0x0..0xF) */
    n = (uint8_t)((100.0f - percent) / 6.25f + 0.5f);
    if (n > 15) n = 15;

    uint8_t ctrl1 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL1);
    ctrl1 &= ~DRV8434S_CTRL1_TRQ_DAC_MASK;
    ctrl1 |= (n << DRV8434S_CTRL1_TRQ_DAC_SHIFT);
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL1, ctrl1);
    return (int)n;
}

/* --------------------------------------------------------------------------
 * 满量程电流设置: IFS = (VREF / KV) * TRQ
 *   -> 所需比例 TRQ = IFS * KV / VREF
 * --------------------------------------------------------------------------*/
int drv8434s_set_full_scale_current(void *handle, float vref_voltage, float current_a)
{
    float ratio, kv = DRV8434S_KV_VPERA;
    uint8_t n;

    if (vref_voltage < 0.05f || vref_voltage > DRV8434S_VREF_MAX_V) return DRV8434S_ERR_PARAM;
    if (current_a <= 0.0f || current_a > DRV8434S_IFS_MAX_A)        return DRV8434S_ERR_PARAM;

    /* TRQ = IFS * KV / VREF   (TRQ 为 100%..6.25% 的标量, 0~1) */
    ratio = current_a * kv / vref_voltage;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0625f) ratio = 0.0625f;

    /* 档位: TRQ = 1 - n*0.0625  -> n = (1 - ratio)/0.0625 */
    n = (uint8_t)((1.0f - ratio) / 0.0625f + 0.5f);
    if (n > 15) n = 15;

    uint8_t ctrl1 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL1);
    ctrl1 &= ~DRV8434S_CTRL1_TRQ_DAC_MASK;
    ctrl1 |= (n << DRV8434S_CTRL1_TRQ_DAC_SHIFT);
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL1, ctrl1);
    return (int)n;
}

/* --------------------------------------------------------------------------
 * 微步模式
 * --------------------------------------------------------------------------*/
int drv8434s_set_microstep(void *handle, drv8434s_microstep_t mode)
{
    uint8_t ctrl3 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL3);
    ctrl3 &= ~DRV8434S_CTRL3_MICROSTEP_MASK;
    ctrl3 |= ((uint8_t)mode & 0x0F) << DRV8434S_CTRL3_MICROSTEP_SHIFT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL3, ctrl3);
    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * 消磁 / 关断时间
 * --------------------------------------------------------------------------*/
int drv8434s_set_decay(void *handle, drv8434s_decay_t decay)
{
    uint8_t ctrl2 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL2);
    ctrl2 &= ~DRV8434S_CTRL2_DECAY_MASK;
    ctrl2 |= ((uint8_t)decay & 0x07) << DRV8434S_CTRL2_DECAY_SHIFT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL2, ctrl2);
    return DRV8434S_OK;
}
int drv8434s_set_toff(void *handle, drv8434s_toff_t toff)
{
    uint8_t ctrl2 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL2);
    ctrl2 &= ~DRV8434S_CTRL2_TOFF_MASK;
    ctrl2 |= ((uint8_t)toff & 0x03) << DRV8434S_CTRL2_TOFF_SHIFT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL2, ctrl2);
    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * 失速检测
 * --------------------------------------------------------------------------*/
int drv8434s_set_stall_detection(void *handle, uint8_t enable, uint8_t report_on_fault, uint16_t threshold)
{
    uint8_t ctrl5 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL5);
    uint8_t ctrl7 = 0, ctrl6 = 0;

    if (enable) {
        ctrl5 |= DRV8434S_CTRL5_EN_STL;
        if (report_on_fault) ctrl5 |= DRV8434S_CTRL5_STL_REP;
    } else {
        ctrl5 &= ~DRV8434S_CTRL5_EN_STL;
    }
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL5, ctrl5);

    /* 失速检测要求 smart tune ripple control */
    if (enable) {
        uint8_t ctrl2 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL2);
        ctrl2 &= ~DRV8434S_CTRL2_DECAY_MASK;
        ctrl2 |= DRV8434S_DECAY_SMART_TUNE_RIPPLE;
        drv8434s_write_reg(handle, DRV8434S_REG_CTRL2, ctrl2);
    }

    /* 阈值涂写 (12bit) */
    if (threshold > 4095) threshold = 4095;
    ctrl6 = (uint8_t)(threshold & 0xFF);
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL6, ctrl6);

    ctrl7 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL7);
    ctrl7 &= ~DRV8434S_CTRL7_STALL_TH_HI_MASK;
    ctrl7 |= (uint8_t)((threshold >> 8) & 0x0F) << DRV8434S_CTRL7_STALL_TH_HI_SHIFT;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL7, ctrl7);

    return DRV8434S_OK;
}

/* --------------------------------------------------------------------------
 * 失速阈值学习 (需电机堵转)
 * --------------------------------------------------------------------------*/
int drv8434s_stall_learn(void *handle)
{
    /* 需先使能失速检测 */
    uint8_t ctrl5 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL5);
    if (!(ctrl5 & DRV8434S_CTRL5_EN_STL)) {
        /* 自动使能 (不设 report) */
        ctrl5 |= DRV8434S_CTRL5_EN_STL;
        drv8434s_write_reg(handle, DRV8434S_REG_CTRL5, ctrl5);
    }
    /* 触发学习 */
    ctrl5 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL5);
    ctrl5 |= DRV8434S_CTRL5_STL_LRN;
    drv8434s_write_reg(handle, DRV8434S_REG_CTRL5, ctrl5);

    /* 等待学习完成 (STL_LRN 自清零 或 STL_LRN_OK 置位) */
    uint32_t timeout = 100; /* 粗略超时计数 */
    while (timeout--) {
        uint8_t diag2 = drv8434s_get_diag2(handle);
        uint8_t c5    = drv8434s_read_reg(handle, DRV8434S_REG_CTRL5);
        if (diag2 & DRV8434S_DIAG2_STL_LRN_OK) return DRV8434S_OK;
        if (!(c5 & DRV8434S_CTRL5_STL_LRN)) {
            /* 学习完成但未成功 */
            return (diag2 & DRV8434S_DIAG2_STL_LRN_OK) ? DRV8434S_OK : DRV8434S_ERR_FAULT;
        }
        drv8434s_hal_delay_ms(1);
    }
    return DRV8434S_ERR_FAULT;
}

/* --------------------------------------------------------------------------
 * 读取转矩计数 12bit
 * --------------------------------------------------------------------------*/
uint16_t drv8434s_get_torque_count(void *handle)
{
    uint8_t lo = drv8434s_read_reg(handle, DRV8434S_REG_CTRL8);
    uint8_t hi = drv8434s_read_reg(handle, DRV8434S_REG_CTRL9);
    uint16_t count = (uint16_t)(((hi & 0x0F) << 8) | lo);
    return count;
}

/* --------------------------------------------------------------------------
 * 版本
 * --------------------------------------------------------------------------*/
uint8_t drv8434s_get_rev_id(void *handle)
{
    uint8_t ctrl9 = drv8434s_read_reg(handle, DRV8434S_REG_CTRL9);
    return (uint8_t)((ctrl9 >> 4) & 0x0F);
}

/* --------------------------------------------------------------------------
 * 睡眠 (通过 nSLEEP 引脚, 或 ENABLE 也可省流)
 * --------------------------------------------------------------------------*/
void drv8434s_sleep(void *handle, uint8_t sleep_enable)
{
    drv8434s_hal_set_pin(handle, DRV8434S_PIN_SLEEP, sleep_enable ? 0 : 1);
    drv8434s_hal_delay_ms(1);
}

/* --------------------------------------------------------------------------
 * 附加: 便捷读取 nFAULT (1=正常, 0=故障)
 * --------------------------------------------------------------------------*/
uint8_t drv8434s_check_fault_pin(void *handle)
{
    return drv8434s_hal_read_fault(handle);
}
