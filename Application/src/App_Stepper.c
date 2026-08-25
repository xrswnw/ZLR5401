#include "App_Stepper.h"
#include "App_Motor_HL.h"
#include "App_SysTick_HL.h"
#include "App_Stepper_Tim4.h"
#include "drv8434s.h"

/* 步进电机应用状态机.
 * 硬件电流档: VREF 按硬件 2.64V 假设, IFS=2A (100%). 如硬件 VREF 不同, 调整此处.
 * 速度: 每微步间隔 = 1/speed; 默认 500 微步/s (Smart Tune Ripple 消磁下可持续).
 *
 * STEP 脉冲由 TIM4 硬件定时器(PB6)产生, 微秒级精确均匀 (App_Stepper_Tim4.c).
 * 本层仅管理状态机、方向、使能、故障检测与速度参数. */

#define STEPPER_PCT_DEFAULT  100u   /* 默认转矩 100% (满刻度); 协议 TORQUE 可下调 */
#define STEPPER_DEFAULT_HZ   500u    /* 默认 500 微步/s */
#define STEPPER_MIN_HZ       1u
#define STEPPER_MAX_HZ       8000u   /* 上限, 兼顾 SPI 开销与 IWDG 2s 预算 (每 tick 一次 SPI) */
#define APP_VREF_VOLTS       2.64f   /* 硬件 VREF (仅配置结构数据, 无浮点运算) */

/* 高负载(堵转)监测参数 */
#define STEPPER_OL_DEFAULT_THRESH   400u   /* TRQ_COUNT 12bit, 越低越接近失速; <此值判高负载 */
#define STEPPER_OL_SAMPLE_MS        100u   /* 采样周期 */
#define STEPPER_OL_SAMPLES          6u     /* 6×100ms=600ms 持续低阈值 -> 高负载(滤瞬时) */
#define STEPPER_OL_DOWNGRADE_HZ     800u   /* 降速档 (微步/s) */
#define STEPPER_OL_DN_SAMPLES       4u     /* 降速后 400ms 仍未回升 -> 停机 */
#define STEPPER_RUN_MAX_MS          60000u /* 单次连续运行上限 (防机构长时间卡死通电) */

/* 停止原因 (统计 lastReason) */
#define STEPPER_RSN_NONE     0u
#define STEPPER_RSN_NORMAL   1u
#define STEPPER_RSN_TIMEOUT  2u
#define STEPPER_RSN_OVERLOAD 3u
#define STEPPER_RSN_DRVFAULT 4u

static AppStepperState_t s_state = APP_STEPPER_IDLE;
static uint32_t          s_speedHz  = STEPPER_DEFAULT_HZ;
static uint32_t          s_cmdSpeedHz = STEPPER_DEFAULT_HZ;  /* 协议设定速(不随降速改变) */
static uint32_t          s_stepsReq = 0;      /* 本次目标微步数, 0=持续 */
static uint32_t          s_lastTickMs;
static uint8_t           s_fault, s_diag1, s_diag2;
static uint8_t           s_dir;

/* 高负载(堵转)监测 + 运行统计 */
static AppStepperOlovState_t s_olov = STEPPER_OL_NONE;
static uint16_t  s_olovThresh = STEPPER_OL_DEFAULT_THRESH;
static uint16_t  s_trqCount  = 0;
static uint8_t   s_olovCnt   = 0;      /* 连续低阈值采样计数 */
static uint8_t   s_olovDnCnt = 0;      /* 降速后连续低阈值计数 */
static uint32_t  s_olovTickMs = 0;     /* TRQ 采样节拍基准 */
static uint32_t  s_runStartMs = 0;
static uint32_t  s_runMsAcc   = 0;     /* 运行 ms 累计 (用于 runSeconds) */
static AppStepperStats_t s_stats = {0,0,STEPPER_RSN_NONE};

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
    cfg.microstep    = DRV8434S_MICROSTEP_HALF;
    cfg.decay        = DRV8434S_DECAY_SMART_TUNE_RIPPLE;
    cfg.enable_ol    = 0;
    cfg.ocp_retry    = 0;
    cfg.otsd_auto_recover = 0;
    cfg.enable_stall = 0;           /* 失速检测关闭: 本硬件600RPM下使能EN_STL会误置SPI_ERROR/FAULT(见验证) */
    cfg.stall_report = 1;

    s_state = APP_STEPPER_IDLE;
    s_speedHz = STEPPER_DEFAULT_HZ;
    s_cmdSpeedHz = STEPPER_DEFAULT_HZ;
    s_stepsReq = 0; s_dir = 0;
    s_fault = 0; s_diag1 = 0; s_diag2 = 0;
    s_lastTickMs = SysTickHl_GetMs();
    s_olov = STEPPER_OL_NONE;
    s_olovThresh = STEPPER_OL_DEFAULT_THRESH;
    s_trqCount = 0; s_olovCnt = 0; s_olovDnCnt = 0;
    s_olovTickMs = 0; s_runStartMs = 0; s_runMsAcc = 0;
    s_stats.runSeconds = 0; s_stats.startCount = 0; s_stats.lastReason = STEPPER_RSN_NONE;

    /* 硬件 STEP 定时器: PB6 重配为 TIM4_CH1, 产生微秒级均匀 STEP 脉冲 */
    StepperTim4_Init();

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
            /* 故障 (FAULT 位、失速 DIAG2 或 nFAULT 拉低) -> 停转 */
            if ((s_fault & DRV8434S_FLT_FAULT) ||
                (s_diag2 & DRV8434S_DIAG2_STALL) ||
                (drv8434s_check_fault_pin(&g_hMotor) == 0u)) {
                StepperTim4_Stop();
                s_state = APP_STEPPER_FAULT;
                stepper_disable_output();
                s_stats.lastReason = STEPPER_RSN_DRVFAULT;
                return;
            }

            /* 连续运行时限: 防机构长时间卡死持续通电 */
            if ((now - s_runStartMs) >= STEPPER_RUN_MAX_MS) {
                StepperTim4_Stop();
                stepper_disable_output();
                s_state = APP_STEPPER_IDLE;
                s_olov = STEPPER_OL_NONE;
                s_stats.lastReason = STEPPER_RSN_TIMEOUT;
                return;
            }

            /* TRQ_COUNT 高负载(堵转)监测: 每 100ms 采样 */
            if ((now - s_olovTickMs) >= STEPPER_OL_SAMPLE_MS) {
                s_olovTickMs = now;
                s_trqCount = drv8434s_get_torque_count(&g_hMotor);
                s_runMsAcc += STEPPER_OL_SAMPLE_MS;
                if (s_runMsAcc >= 1000u) { s_runMsAcc -= 1000u; s_stats.runSeconds++; }
                if (s_trqCount < s_olovThresh) {
                    if (s_olov == STEPPER_OL_OVERLOAD) {
                        /* 已降速仍高负载 -> 停机保护 */
                        if (++s_olovDnCnt >= STEPPER_OL_DN_SAMPLES) {
                            StepperTim4_Stop();
                            stepper_disable_output();
                            s_state = APP_STEPPER_IDLE;
                            s_olov = STEPPER_OL_FAULT;
                            s_stats.lastReason = STEPPER_RSN_OVERLOAD;
                            return;
                        }
                    } else if (++s_olovCnt >= STEPPER_OL_SAMPLES) {
                        /* 持续低阈值 -> 判高负载并自动降速 */
                        s_olovDnCnt = 0;
                        s_olov = STEPPER_OL_OVERLOAD;
                        s_speedHz = STEPPER_OL_DOWNGRADE_HZ; /* TIM4 斜坡自然减速 */
                    }
                } else {
                    /* 负载恢复: 复位监测并恢复协议设定速 */
                    s_olovCnt = 0; s_olovDnCnt = 0;
                    if (s_olov == STEPPER_OL_OVERLOAD)
                        s_speedHz = s_cmdSpeedHz;
                    s_olov = STEPPER_OL_NONE;
                }
            }
        }
    }
}

int App_Stepper_Move(AppStepperMove_t *mv)
{
    if (!mv || s_state == APP_STEPPER_FAULT) return -1;
    /* 停止已有运动 */
    StepperTim4_Stop();
    s_dir = (mv->dir) ? 1u : 0u;
    s_stepsReq = mv->steps;
    drv8434s_hal_set_pin(&g_hMotor, DRV8434S_PIN_DIR, s_dir);   /* GPIO 方向脚 */
    /* 清残留故障再使能输出 */
    if (s_fault & DRV8434S_FLT_FAULT) {
        (void)drv8434s_clear_fault(&g_hMotor);
        s_fault = 0;
    }
    (void)drv8434s_set_enable(&g_hMotor, DRV8434S_ENABLED);
    /* 新运动: 复位高负载状态、恢复协议设定速、记录统计与起始时刻 */
    s_olov = STEPPER_OL_NONE;
    s_olovCnt = 0; s_olovDnCnt = 0;
    s_speedHz = s_cmdSpeedHz;
    s_runStartMs = SysTickHl_GetMs();
    s_olovTickMs = s_runStartMs;
    s_stats.startCount++;
    /* TIM4 硬件 STEP: 斜坡起速升到 s_speedHz, stepsReq 步后自动停 */
    StepperTim4_Start(mv->steps, s_speedHz);
    s_state = APP_STEPPER_RUN;
    return 0;
}

int App_Stepper_Stop(void)
{
    s_stepsReq = 0;
    StepperTim4_Stop();
    stepper_disable_output();
    s_state = APP_STEPPER_IDLE;
    s_olov = STEPPER_OL_NONE;
    if (s_stats.lastReason == STEPPER_RSN_NONE)
        s_stats.lastReason = STEPPER_RSN_NORMAL;
    return 0;
}

/* DC 磁制动停止: Stop 断电前短暂维持磁场(EN_OUT=1)抵抗惯性滑行, 再关断输出.
 * 用于触点接触的停位事件(如 KEY_UP 警戒线硬停), 减少"到了还在沿旧趋势走"的超程.
 * 罕见事件上的短暂阻塞在单任务 1ms 主循环下可接受. */
#define STEPPER_DC_BRAKE_MS  8u
int App_Stepper_DcBrakeStop(void)
{
    s_stepsReq = 0;
    StepperTim4_Stop();
    uint32_t t0 = SysTickHl_GetMs();
    while ((SysTickHl_GetMs() - t0) < STEPPER_DC_BRAKE_MS) { }
    stepper_disable_output();
    s_state = APP_STEPPER_IDLE;
    s_olov = STEPPER_OL_NONE;
    return 0;
}

int App_Stepper_SetSpeedHz(uint32_t hz)
{
    if (hz < STEPPER_MIN_HZ) hz = STEPPER_MIN_HZ;
    if (hz > STEPPER_MAX_HZ) hz = STEPPER_MAX_HZ;
    s_speedHz   = hz;
    s_cmdSpeedHz = hz;              /* 协议设定速; 自动降速不覆盖此值 */
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
uint32_t App_Stepper_GetStepsDone(void) { return StepperTim4_GetStepsDone(); }

void App_Stepper_SetOlovThreshold(uint16_t t)
{
    if (t == 0u) t = 1u;
    s_olovThresh = t;
    s_olovCnt = 0; s_olovDnCnt = 0;
}

uint16_t App_Stepper_GetOlovThreshold(void) { return s_olovThresh; }

AppStepperOlovState_t App_Stepper_GetOlovState(void) { return s_olov; }
uint16_t App_Stepper_GetTorqueCount(void)  { return s_trqCount; }

AppStepperStats_t App_Stepper_GetStats(void) { return s_stats; }
