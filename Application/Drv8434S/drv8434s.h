/*
 * ============================================================================
 *  DRV8434S Stepper Motor Driver - C HAL/SDK Header
 * ============================================================================
 *  Part    : DRV8434S (TI 双极步进电机驱动芯片)
 *  Datasheet: SLOSE70 - DECEMBER 2020
 *
 *  功能概要:
 *    - 集成电流检测 (无需外置采样电阻)
 *    - 1/256 最高微步进 (MICROSTEP_MODE)
 *    - SPI (16-bit, 全双工, 支持菊花链) + STEP/DIR
 *    - Smart Tune / Slow / Mixed 消磁模式
 *    - TRQ_DAC 转矩缩放
 *    - UVLO / CPUV / OCP / OL / 无感失速检测 / OTW / OTSD 保护
 *
 *  本 SDK 提供硬件抽象层 (HAL) 接口, 供 MCU 底层 SPI/GPIO 移植,
 *  以及面向应用的步进电机驱动 API。
 *
 *  移植说明: 实现下方"HAL 底层接口"一节中的 4 个函数即可运行, 无需改算法代码。
 *
 *  NOTE: 电源电压 4.5V~48V, 满量程电流最高 2.5A, RMS 最高 1.8A。
 * ============================================================================
 */
#ifndef __DRV8434S_H__
#define __DRV8434S_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. 寄存器地址定义 (Register Address)
 * ============================================================================
 *  SPI 帧: 16-bit = [W0][A4..A0][X][D7..D0]
 *   W0=0 写, W0=1 读
 * ==========================================================================*/
#define DRV8434S_REG_FAULT_STATUS     0x00   /* R  故障状态 */
#define DRV8434S_REG_DIAG_STATUS1     0x01   /* R  诊断状态1 (OCP 位置) */
#define DRV8434S_REG_DIAG_STATUS2     0x02   /* R  诊断状态2 (过温/失速/开路) */
#define DRV8434S_REG_CTRL1            0x03   /* RW 转矩 DAC + 开路模式 */
#define DRV8434S_REG_CTRL2            0x04   /* RW EN_OUT/TOFF/DECAY */
#define DRV8434S_REG_CTRL3            0x05   /* RW DIR/STEP/SPI_DIR/SPI_STEP/MICROSTEP */
#define DRV8434S_REG_CTRL4            0x06   /* RW CLR_FLT/LOCK/EN_OL/OCP_MODE/OTSD_MODE/TW_REP */
#define DRV8434S_REG_CTRL5            0x07   /* RW STL_LRN/EN_STL/STL_REP */
#define DRV8434S_REG_CTRL6            0x08   /* RW STALL_TH[7:0] */
#define DRV8434S_REG_CTRL7            0x09   /* RW RC_RIPPLE/EN_SSC/TRQ_SCALE/STALL_TH[11:8] */
#define DRV8434S_REG_CTRL8            0x0A   /* R  TRQ_COUNT[7:0] */
#define DRV8434S_REG_CTRL9            0x0B   /* R  REV_ID[3:0]/TRQ_COUNT[11:8] */

/* ============================================================================
 * 2. FAULT Status 寄存器 (0x00) 位定义
 * ==========================================================================*/
#define DRV8434S_FLT_FAULT            (1u << 7)  /* nFAULT=0 时此位=1 */
#define DRV8434S_FLT_SPI_ERROR        (1u << 6)  /* SPI 协议错误 */
#define DRV8434S_FLT_UVLO             (1u << 5)  /* VM 欠压锁存 */
#define DRV8434S_FLT_CPUV             (1u << 4)  /* 电荷泵欠压 */
#define DRV8434S_FLT_OCP              (1u << 3)  /* 过流 */
#define DRV8434S_FLT_STL              (1u << 2)  /* 失速 */
#define DRV8434S_FLT_TF               (1u << 1)  /* 过温警告或关断的逻辑或 */
#define DRV8434S_FLT_OL               (1u << 0)  /* 开路 */

/* ============================================================================
 * 3. DIAG Status 1 寄存器 (0x01) 位定义 - OCP 位置
 * ==========================================================================*/
#define DRV8434S_DIAG1_OCP_LS2_B      (1u << 7)  /* BOUT 半桥2 低边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_HS2_B      (1u << 6)  /* BOUT 半桥2 高边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_LS1_B      (1u << 5)  /* BOUT 半桥1 低边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_HS1_B      (1u << 4)  /* BOUT 半桥1 高边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_LS2_A      (1u << 3)  /* AOUT 半桥2 低边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_HS2_A      (1u << 2)  /* AOUT 半桥2 高边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_LS1_A      (1u << 1)  /* AOUT 半桥1 低边 MOSFET 过流 */
#define DRV8434S_DIAG1_OCP_HS1_A      (1u << 0)  /* AOUT 半桥1 高边 MOSFET 过流 */

/* ============================================================================
 * 4. DIAG Status 2 寄存器 (0x02) 位定义
 * ==========================================================================*/
#define DRV8434S_DIAG2_OTW            (1u << 6)  /* 过温警告 */
#define DRV8434S_DIAG2_OTS            (1u << 5)  /* 过温关断 */
#define DRV8434S_DIAG2_STL_LRN_OK     (1u << 4)  /* 失速学习成功 */
#define DRV8434S_DIAG2_STALL          (1u << 3)  /* 失速 */
#define DRV8434S_DIAG2_OL_B           (1u << 1)  /* BOUT 开路 */
#define DRV8434S_DIAG2_OL_A           (1u << 0)  /* AOUT 开路 */

/* ============================================================================
 * 5. CTRL1 (0x03) - TRQ_DAC + OL_MODE
 * ==========================================================================*/
#define DRV8434S_CTRL1_TRQ_DAC_SHIFT  4
#define DRV8434S_CTRL1_TRQ_DAC_MASK   (0x0Fu << 4)
#define DRV8434S_CTRL1_OL_MODE        (1u << 1)  /* 1b=开路条件消失立即释放 nFAULT */

/* ============================================================================
 * 6. CTRL2 (0x04) - EN_OUT / TOFF / DECAY
 * ==========================================================================*/
#define DRV8434S_CTRL2_EN_OUT         (1u << 7)  /* 1=使能输出 */
#define DRV8434S_CTRL2_TOFF_SHIFT     3
#define DRV8434S_CTRL2_TOFF_MASK      (0x03u << 3)
#define DRV8434S_CTRL2_DECAY_SHIFT    0
#define DRV8434S_CTRL2_DECAY_MASK     0x07u

/*  PWM 关断时间 TOFF 选项 */
typedef enum {
    DRV8434S_TOFF_7US   = 0x0,
    DRV8434S_TOFF_16US  = 0x1,
    DRV8434S_TOFF_24US  = 0x2,
    DRV8434S_TOFF_32US  = 0x3
} drv8434s_toff_t;

/*  消磁模式 DECAY 选项 */
typedef enum {
    DRV8434S_DECAY_SLOW_SLOW           = 0x0, /* 增:慢消磁, 减:慢消磁 */
    DRV8434S_DECAY_SLOW_MIXED30        = 0x1, /* 增:慢,   减:混合30%快 */
    DRV8434S_DECAY_SLOW_MIXED60        = 0x2, /* 增:慢,   减:混合60%快 */
    DRV8434S_DECAY_SLOW_FAST           = 0x3, /* 增:慢,   减:快 */
    DRV8434S_DECAY_MIXED30_MIXED30     = 0x4, /* 混合30%快, 混合30%快 */
    DRV8434S_DECAY_MIXED60_MIXED60     = 0x5, /* 混合60%快, 混合60%快 */
    DRV8434S_DECAY_SMART_TUNE_DYNAMIC  = 0x6, /* Smart Tune 动态消磁 */
    DRV8434S_DECAY_SMART_TUNE_RIPPLE   = 0x7  /* Smart Tune 纹波控制 (默认) */
} drv8434s_decay_t;

/* ============================================================================
 * 7. CTRL3 (0x05) - DIR / STEP / SPI_DIR / SPI_STEP / MICROSTEP_MODE
 * ==========================================================================*/
#define DRV8434S_CTRL3_DIR             (1u << 7)  /* SPI_DIR=1 时此位决定方向 */
#define DRV8434S_CTRL3_STEP            (1u << 6)  /* SPI_STEP=1 时写1前进一步(自清零) */
#define DRV8434S_CTRL3_SPI_DIR         (1u << 5)  /* 1=方向跟随 SPI DIR */
#define DRV8434S_CTRL3_SPI_STEP        (1u << 4)  /* 1=步进跟随 SPI STEP */
#define DRV8434S_CTRL3_MICROSTEP_SHIFT 0
#define DRV8434S_CTRL3_MICROSTEP_MASK  0x0Fu

/*  微步进模式选项 */
typedef enum {
    DRV8434S_MICROSTEP_FULL_100  = 0x0, /* 全步进 100% 电流 */
    DRV8434S_MICROSTEP_FULL_71   = 0x1, /* 全步进 71% 电流 */
    DRV8434S_MICROSTEP_HALF_NC   = 0x2, /* 非圆形 1/2 步 */
    DRV8434S_MICROSTEP_HALF      = 0x3, /* 1/2 步 */
    DRV8434S_MICROSTEP_QUARTER   = 0x4, /* 1/4 步 */
    DRV8434S_MICROSTEP_1_8       = 0x5, /* 1/8 步 */
    DRV8434S_MICROSTEP_1_16      = 0x6, /* 1/16 步 (默认) */
    DRV8434S_MICROSTEP_1_32      = 0x7, /* 1/32 步 */
    DRV8434S_MICROSTEP_1_64      = 0x8, /* 1/64 步 */
    DRV8434S_MICROSTEP_1_128     = 0x9, /* 1/128 步 */
    DRV8434S_MICROSTEP_1_256     = 0xA  /* 1/256 步 */
} drv8434s_microstep_t;

/* ============================================================================
 * 8. CTRL4 (0x06) - CLR_FLT / LOCK / EN_OL / OCP_MODE / OTSD_MODE / TW_REP
 * ==========================================================================*/
#define DRV8434S_CTRL4_CLR_FLT        (1u << 7)  /* 写1清除所有锁存故障(自清零) */
#define DRV8434S_CTRL4_LOCK_SHIFT     4
#define DRV8434S_CTRL4_LOCK_MASK      (0x07u << 4)
#define DRV8434S_CTRL4_EN_OL          (1u << 3)  /* 1=使能开路检测 */
#define DRV8434S_CTRL4_OCP_MODE       (1u << 2)  /* 1=过流自动重试, 0=过流锁存 */
#define DRV8434S_CTRL4_OTSD_MODE      (1u << 1)  /* 1=过温自动恢复, 0=过温锁存 */
#define DRV8434S_CTRL4_TW_REP         (1u << 0)  /* 1=过温警告报 nFAULT */

/*  LOCK 关键值 */
#define DRV8434S_LOCK_LOCKED          0x6u   /* 写入以锁定 */
#define DRV8434S_LOCK_UNLOCKED        0x3u   /* 默认, 写入以解锁 */

/* ============================================================================
 * 9. CTRL5 (0x07) - STL_LRN / EN_STL / STL_REP
 * ==========================================================================*/
#define DRV8434S_CTRL5_STL_LRN        (1u << 5)  /* 1=开始失速学习(完成后自动清零) */
#define DRV8434S_CTRL5_EN_STL         (1u << 4)  /* 1=使能失速检测 */
#define DRV8434S_CTRL5_STL_REP        (1u << 3)  /* 1=失速报 nFAULT */

/* ============================================================================
 * 10. CTRL6 (0x08) - STALL_TH[7:0]  &  CTRL7 (0x09) - RC_RIPPLE/EN_SSC/TRQ_SCALE/STALL_TH[11:8]
 * ==========================================================================*/
#define DRV8434S_CTRL7_RC_RIPPLE_SHIFT 6
#define DRV8434S_CTRL7_RC_RIPPLE_MASK  (0x03u << 6)
#define DRV8434S_CTRL7_EN_SSC          (1u << 5)  /* 1=扩频时钟使能(默认) */
#define DRV8434S_CTRL7_TRQ_SCALE       (1u << 4)  /* 1=转矩计数放大8倍 */
#define DRV8434S_CTRL7_STALL_TH_HI_SHIFT 0
#define DRV8434S_CTRL7_STALL_TH_HI_MASK 0x0Fu

/*  RC_RIPPLE 电流纹波选项 (仅 smart tune ripple control) */
typedef enum {
    DRV8434S_RIPPLE_1PCT = 0x0,  /* 19mA + 1% ITRIP */
    DRV8434S_RIPPLE_2PCT = 0x1,  /* 19mA + 2% ITRIP */
    DRV8434S_RIPPLE_4PCT = 0x2,  /* 19mA + 4% ITRIP */
    DRV8434S_RIPPLE_6PCT = 0x3   /* 19mA + 6% ITRIP */
} drv8434s_ripple_t;

/* ============================================================================
 * 11. 常用常量与电气参考值
 * ==========================================================================*/
#define DRV8434S_IFS_MAX_A     2.5f   /* 满量程电流上限 A */
#define DRV8434S_IRMS_MAX_A    1.8f   /* RMS 电流上限 A */
#define DRV8434S_KV_VPERA      1.32f  /* 跨导增益 V/A (典型) */
#define DRV8434S_VREF_MAX_V    3.3f   /* VREF 电压上限 */
#define DRV8434S_VM_MIN_V      4.5f   /* 供电下限 */
#define DRV8434S_VM_MAX_V      48.0f  /* 供电上限 */
#define DRV8434S_TWAKE_MS      1.2f   /* 唤醒时间 ms (典型最大) */
#define DRV8434S_TON_MS        1.2f   /* 上电时间 ms */
#define DRV8434S_TREADY_MS     1.0f   /* SPI 就绪时间 ms */

/* ============================================================================
 * 12. 驱动状态 / 方向
 * ==========================================================================*/
typedef enum {
    DRV8434S_DISABLED = 0,
    DRV8434S_ENABLED  = 1
} drv8434s_enable_t;

typedef enum {
    DRV8434S_DIR_CW  = 0,   /* 方向: 顺时针 (DIR 位=0) */
    DRV8434S_DIR_CCW = 1    /* 方向: 逆时针 (DIR 位=1) */
} drv8434s_dir_t;

/* ============================================================================
 * 13. 配置结构体 - 用于一键初始化
 * ==========================================================================*/
typedef struct {
    /* 电流与转矩 */
    float                vref_voltage;      /* VREF 引脚电压 V (0.05~3.3V) */
    drv8434s_toff_t      toff;              /* PWM 关断时间 */
    drv8434s_decay_t     decay;             /* 消磁模式 */
    drv8434s_microstep_t microstep;         /* 微步进模式 */
    /* 保护选项 */
    uint8_t              enable_ol;         /* 1=使能开路检测 */
    uint8_t              ocp_retry;         /* 1=过流自动重试, 0=锁存 */
    uint8_t              otsd_auto_recover; /* 1=过温自动恢复, 0=锁存 */
    uint8_t              tw_report;         /* 1=过温报警报 nFAULT */
    /* 失速检测 (仅 smart tune ripple 模式有效) */
    uint8_t              enable_stall;      /* 1=使能失速检测 */
    uint8_t              stall_report;      /* 1=失速报 nFAULT */
    uint8_t              stall_threshold;   /* 12-bit 失速阈值 (0~4095) */
    drv8434s_ripple_t    ripple;            /* smart tune 纹波 */
    uint8_t              enable_ssc;        /* 1=扩频时钟使能 */
} drv8434s_config_t;

/* ============================================================================
 * 14. 返回码
 * ==========================================================================*/
typedef enum {
    DRV8434S_OK            = 0,
    DRV8434S_ERR_PARAM     = -1,   /* 参数错误 */
    DRV8434S_ERR_FAULT     = -2,   /* 器件处于故障状态 */
    DRV8434S_ERR_NULL      = -3    /* 空指针 */
} drv8434s_err_t;

/* 桩 / 默认配置宏 (VREF=2.64V => IFS=2A, 需按实际硬件填写) */
#define DRV8434S_CONFIG_DEFAULT { \
        .vref_voltage = 2.64f, \
        .toff = DRV8434S_TOFF_16US, \
        .decay = DRV8434S_DECAY_SMART_TUNE_RIPPLE, \
        .microstep = DRV8434S_MICROSTEP_1_16, \
        .enable_ol = 0, \
        .ocp_retry = 0, \
        .otsd_auto_recover = 0, \
        .tw_report = 0, \
        .enable_stall = 0, \
        .stall_report = 1, \
        .stall_threshold = 3, \
        .ripple = DRV8434S_RIPPLE_1PCT, \
        .enable_ssc = 1 }

/* ============================================================================
 * 15. HAL 底层接口 (需在移植层实现)
 * ----------------------------------------------------------------------------
 * 用户只需实现以下 4 个函数, 即可使用本 SDK。
 * ==========================================================================*/

/*
 * 延迟毫秒。可调用 MCU 的 delay_ms();
 */
void drv8434s_hal_delay_ms(uint32_t ms);

/*
 * 单次 SPI 16-bit 全双工传输。
 * 返回从 SDO 读到的 16-bit 数据。
 * 注意: nSCS 片选已在内部处理 (低电平有效, 帧间高电平 >= 500ns)。
 * 建议 CPOL=0, CPHA=1 (数据在下降沿捕获)。@param[in] handle 设备句柄(如 SPI 外设指针)
 */
uint16_t drv8434s_hal_spi_xfer(void *handle, uint16_t word);

/*
 * 读写 nFAULT 引脚电平。返回当前 nFAULT 逻辑电平 (1=正常, 0=故障)。
 */
uint8_t drv8434s_hal_read_fault(void *handle);

/*
 * 控制 STEP/DIR/ENABLE/nSLEEP 引脚。
 * 若设计使用引脚控制而非 SPI 控制, 由本函数实现。
 */
void drv8434s_hal_set_pin(void *handle, uint8_t pin_index, uint8_t level);

/*  引脚索引定义 (便于 hal_set_pin 使用) */
#define DRV8434S_PIN_STEP    0
#define DRV8434S_PIN_DIR     1
#define DRV8434S_PIN_ENABLE  2
#define DRV8434S_PIN_SLEEP   3

/* ============================================================================
 * 16. 公共 API
 * ==========================================================================*/

/*
 * 读取一个寄存器 (返回寄存内容)。
 */
uint8_t drv8434s_read_reg(void *handle, uint8_t addr);

/*
 * 写一个寄存器 (读取旧值返回)。
 */
uint8_t drv8434s_write_reg(void *handle, uint8_t addr, uint8_t data);

/*
 * 初始化: 上电等待, 清除故障, 应用配置, 使能输出。
 * 返回 0 成功, 负数为错误码。
 */
int drv8434s_init(void *handle, const drv8434s_config_t *cfg);

/*
 * 使能 / 禁止输出 (EN_OUT)。
 */
int drv8434s_set_enable(void *handle, drv8434s_enable_t en);

/*
 * 设置方向。direction: DRV8434S_DIR_CW / DRV8434S_DIR_CCW。
 */
int drv8434s_set_dir(void *handle, drv8434s_dir_t direction);

/*
 * 通过 SPI 步进一步 (需 SPI_STEP=1)。每次写 STEP 位=1 前进一个微步。
 */
int drv8434s_spi_step(void *handle);

/*
 * 读取故障状态寄存器 (0x00)。返回原始值。
 */
uint8_t drv8434s_get_fault_status(void *handle);

/*
 * 读取诊断状态寄存器 1/2。
 */
uint8_t drv8434s_get_diag1(void *handle);
uint8_t drv8434s_get_diag2(void *handle);

/*
 * 清除所有锁存故障 (写 CLR_FLT=1)。
 */
int drv8434s_clear_fault(void *handle);

/*
 * 设置 TRQ_DAC 转矩缩放 (0x0~0xF), 具体电流占比见数据手册 6.3。
 * full_scale_percent 传入 6.25~100 (取最近合法档位)。
 */
int drv8434s_set_torque_percent(void *handle, float full_scale_percent);

/*
 * 设置满量程电流 (A)。按 VREF 电压自动计算所需 TRQ_DAC。
 * 返回所选 TRQ_DAC 档位 (0~15)。
 */
int drv8434s_set_full_scale_current(void *handle, float vref_voltage, float current_a);

/*
 * 设置微步进模式。
 */
int drv8434s_set_microstep(void *handle, drv8434s_microstep_t mode);

/*
 * 设置消磁模式与 PWM 关断时间。
 */
int drv8434s_set_decay(void *handle, drv8434s_decay_t decay);
int drv8434s_set_toff(void *handle, drv8434s_toff_t toff);

/*
 * 使能 / 禁止失速检测 (将消磁模式自动切到 Smart Tune Ripple Control)。
 */
int drv8434s_set_stall_detection(void *handle, uint8_t enable, uint8_t report_on_fault, uint16_t threshold);

/*
 * 启动失速阈值学习。返回学习是否成功 (STL_LRN_OK)。
 * NOTE: 学习时电机需短暂堵转。
 */
int drv8434s_stall_learn(void *handle);

/*
 * 读取当前转矩计数 (12-bit TRQ_COUNT)。
 * 数值越低越接近失速。
 */
uint16_t drv8434s_get_torque_count(void *handle);

/*
 * 读取芯片版本 (REV_ID)。
 */
uint8_t drv8434s_get_rev_id(void *handle);

/*
 * 进入 / 退出低功耗睡眠模式。 (也可用 nSLEEP 引脚)
 * NOTE: nSLEEP 在硬件上不可直接接 DVDD。
 */
void drv8434s_sleep(void *handle, uint8_t sleep_enable);

/*
 * 便捷: 单个 STEP 脉冲 (通过引脚)。电平翻转间隔建议 >= 1us。
 * 普通 SPI 步进请用 drv8434s_spi_step()。
 */
void drv8434s_pin_step_pulse(void *handle);

/*
 * 直接读取 nFAULT 引脚电平 (1=正常, 0=故障)。
 */
uint8_t drv8434s_check_fault_pin(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* __DRV8434S_H__ */
