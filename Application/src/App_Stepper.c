#include "App_Stepper.h"
#include "App_Motor_HL.h"
#include "App_SysTick_HL.h"
#include "drv8434s.h"

/* 步进电机应用状态机.
 * 硬件电流档: VREF 按硬件 2.64V 假设, IFS=2A (100%). 如硬件 VREF 不同, 调整此处.
 * 速度: 每微步间隔 = 1/speed; 默认 500 微步/s (Smart Tune Ripple 消磁下可持续). */

#define STEPPER_PCT_DEFAULT  100u   /* 默认转矩 100% (满刻度); 协议 TORQUE 可下调 */
#define STEPPER_DEFAULT_HZ   500u    /* 默认 500 微步/s */
#define STEPPER_MIN_HZ       1u
#define STEPPER_MAX_HZ       2000u   /* 上限, 兼顾 SPI 开销与 IWDG 2s 预算 (每 tick 一次 SPI) */
#define APP_VREF_VOLTS       2.64f   /* 硬件 VREF (仅配置结构数据, 无浮点运算) */

static AppStepperState_t s_state = APP_STEPPER_IDLE;
static uint32_t          s_speedHz  = STEPPER_DEFAULT_HZ;
static uint32_t          s_stepsReq = 0;      /* 本次目标微步数, 0=持续 */
static uint32_t          s_stepsDone = 0;
static uint32_t          s_lastTickMs;
static uint32_t          s_intervalUs;
static uint8_t           s_fault, s_diag1, s_diag2;
static uint8_t           s_dir;
static uint32_t          s_acc;               /* 微步累加器 (1ms 基准) */

static void stepper_disable_output(void)
{
    (void)drv8434s_set_enable(&g_hMotor, DRV8434S_DISABLED);
}

static void stepper_set_trq(uint8_t percent);

void App_Stepper_Init(void)
{
    drv8434s_config_t cfg = DRV8434S_CONFIG_DEFAULT;

    Drv8434S_HL_Init();

    cfg.vref_voltage = APP_VREF_VOLTS;
    cfg.microstep    = DRV8434S_MICROSTEP_FULL_100;
    cfg.decay        = DRV8434S_DECAY_SMART_TUNE_RIPPLE;
    cfg.enable_ol    = 0;
    cfg.ocp_retry    = 0;
    cfg.otsd_auto_recover = 0;
    cfg.enable_stall = 0;

    s_state = APP_STEPPER_IDLE;
    s_speedHz = STEPPER_DEFAULT_HZ;
    s_intervalUs = 1000000u / STEPPER_DEFAULT_HZ;
    s_stepsReq = 0; s_stepsDone = 0; s_acc = 0; s_dir = 0;
    s_fault = 0; s_diag1 = 0; s_diag2 = 0;
    s_lastTickMs = SysTickHl_GetMs();

    /* 上电配置 (内部等唤醒、清故障、应用配置、EN_OUT=1), 然后关断输出 (IDLE 无负载) */
    if (drv8434s_init(&g_hMotor, &cfg) != DRV8434S_OK) {
        s_state = APP_STEPPER_FAULT;
        s_fault = drv8434s_get_fault_status(&g_hMotor);
        return;
    }
    /* 切换为 GPIO STEP/DIR 控制: 清 CTRL3 的 SPI_STEP/SPI_DIR, 保留微步模式,
     * 此后步进/方向走硬件 STEP/DIR 引脚 (SPI 仅做寄存器配置). */
    {
        uint8_t ctrl3 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3);
        ctrl3 &= (uint8_t)~(DRV8434S_CTRL3_SPI_STEP | DRV8434S_CTRL3_SPI_DIR);
        drv8434s_write_reg(&g_hMotor, DRV8434S_REG_CTRL3, ctrl3);
    }
    /* 显式重申微步档位: 某些默认/旧寄存器保留值下 init 的微步写入可能被覆盖,
     * 这里在清除 SPI 位后重新写入并保持 (1/2 步, 0x03). */
    {
        uint8_t ctrl3 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3);
        ctrl3 &= (uint8_t)~(DRV8434S_CTRL3_MICROSTEP_MASK);
        ctrl3 |= ((uint8_t)cfg.microstep & 0x0F);
        drv8434s_write_reg(&g_hMotor, DRV8434S_REG_CTRL3, ctrl3);
    }
    /* 默认转矩 50%: 直接写 CTRL1 TRQ_DAC (整数, 避免链接浮点软库) */
    stepper_set_trq(STEPPER_PCT_DEFAULT);
    stepper_disable_output();
}

/* 写入 TRQ_DAC (保留 CTRL1 其它位): percent 6~100, TRQ 档 n=0..15 (6.25% 步进) */
static void stepper_set_trq(uint8_t percent)
{
    uint8_t ctrl1 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL1);
    /* n = (100 - percent) / 6.25, 整数近似: n = (100-percent)*16/100 */
    uint32_t n = (uint32_t)((uint32_t)(100u - percent) * 16u) / 100u;
    if (n > 15) n = 15;
    ctrl1 &= ~DRV8434S_CTRL1_TRQ_DAC_MASK;
    ctrl1 |= (uint8_t)(n << DRV8434S_CTRL1_TRQ_DAC_SHIFT);
    drv8434s_write_reg(&g_hMotor, DRV8434S_REG_CTRL1, ctrl1);
}

static void stepper_read_fault(void)
{
    s_fault = drv8434s_get_fault_status(&g_hMotor);
    s_diag1 = drv8434s_get_diag1(&g_hMotor);
    s_diag2 = drv8434s_get_diag2(&g_hMotor);
}

void App_Stepper_Process(void)
{
    uint32_t now = SysTickHl_GetMs();

    /* 每 10ms 查询一次故障状态 */
    if ((now - s_lastTickMs) >= 10u) {
        s_lastTickMs = now;
        stepper_read_fault();

        if (s_state == APP_STEPPER_RUN) {
            /* 故障 (FAULT 位或 nFAULT 拉低) -> 停转 */
            if ((s_fault & DRV8434S_FLT_FAULT) ||
                (drv8434s_check_fault_pin(&g_hMotor) == 0u)) {
                s_state = APP_STEPPER_FAULT;
                stepper_disable_output();
                return;
            }
        }
    }

    if (s_state != APP_STEPPER_RUN) return;

    /* 步进节拍: 用 1ms 粒度累加器按 s_intervalUs 微步间隔推进.
     * 每 1ms 累计 1000us, 满 intervalUs 则下发一个微步 (SPI). */
    static uint32_t lastMs = 0;
    uint32_t mnow = SysTickHl_GetMs();
    if (mnow == lastMs) return;
    lastMs = mnow;

    s_acc += 1000u;                       /* 1ms 基准 */
    while (s_acc >= s_intervalUs) {
        s_acc -= s_intervalUs;
        /* GPIO 控制: 先设方向 (DIR 引脚), 再发 STEP 微步脉冲 (SPI 仅管寄存器配置) */
        drv8434s_hal_set_pin(&g_hMotor, DRV8434S_PIN_DIR, s_dir);
        drv8434s_pin_step_pulse(&g_hMotor);
        s_stepsDone++;
        /* 限步: 持续运行(stepsReq==0)不受限 */
        if (s_stepsReq != 0u && s_stepsDone >= s_stepsReq) {
            s_state = APP_STEPPER_IDLE;
            stepper_disable_output();
            break;
        }
    }
}

int App_Stepper_Move(AppStepperMove_t *mv)
{
    if (!mv || s_state == APP_STEPPER_FAULT) return -1;
    /* 复位本段: 停止已有运动并清计数 */
    s_stepsDone = 0; s_acc = 0; s_dir = (mv->dir) ? 1u : 0u;
    s_stepsReq = mv->steps;
    drv8434s_hal_set_pin(&g_hMotor, DRV8434S_PIN_DIR, s_dir);   /* GPIO 方向脚 */
    /* 清残留故障再使能输出 */
    if (s_fault & DRV8434S_FLT_FAULT) {
        (void)drv8434s_clear_fault(&g_hMotor);
        s_fault = 0;
    }
    (void)drv8434s_set_enable(&g_hMotor, DRV8434S_ENABLED);
    s_state = APP_STEPPER_RUN;
    return 0;
}

int App_Stepper_Stop(void)
{
    s_stepsReq = 0;
    stepper_disable_output();
    s_state = APP_STEPPER_IDLE;
    return 0;
}

int App_Stepper_SetSpeedHz(uint32_t hz)
{
    if (hz < STEPPER_MIN_HZ) hz = STEPPER_MIN_HZ;
    if (hz > STEPPER_MAX_HZ) hz = STEPPER_MAX_HZ;
    s_speedHz   = hz;
    s_intervalUs = 1000000u / hz;
    return 0;
}

int App_Stepper_SetTorquePercent(uint8_t pct)
{
    if (pct < 6u)  pct = 6u;
    if (pct > 100u) pct = 100u;
    stepper_set_trq(pct);
    return 0;
}

int App_Stepper_ClearFault(void)
{
    (void)drv8434s_clear_fault(&g_hMotor);
    s_fault = 0; s_diag1 = 0; s_diag2 = 0;
    if (s_state == APP_STEPPER_FAULT) {
        s_state = APP_STEPPER_IDLE;
        stepper_disable_output();
    }
    return 0;
}

AppStepperState_t App_Stepper_GetState(void) { return s_state; }
uint8_t  App_Stepper_GetFault(void)     { return s_fault; }
uint8_t  App_Stepper_GetDiag1(void)     { return s_diag1; }
uint8_t  App_Stepper_GetDiag2(void)     { return s_diag2; }
uint8_t  App_Stepper_GetMicrostep(void)   /* 诊断: 实时读 DRV8434S CTRL3 微步位 [3:0] */
{ return (uint8_t)(drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3) & DRV8434S_CTRL3_MICROSTEP_MASK); }
uint32_t App_Stepper_GetStepsDone(void) { return s_stepsDone; }
