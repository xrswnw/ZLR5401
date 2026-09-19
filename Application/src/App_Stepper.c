#include "App_Stepper.h"
#include "App_Motor_HL.h"
#include "App_NewPeriph_HL.h"   /* 黑匣子采样需读行程开关原始电平 */
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
#define STEPPER_MICROSTEP_DRV DRV8434S_MICROSTEP_HALF  /* 上电压入 DRV8434S 微步档 (1/2, 400/转); 自检判据经 GetMicrostepCfg 读取 */

/* 高负载(堵转)监测参数: 100ms 采样; TRQ 持续低于阈值 ~600ms 判高负载并自动降速,
 * 降速后仍持续低阈值再累计 ~2.4s (合计 ~3s) 才停机 -> 硬顶挡块 3s 内保护, 避免长时间研磨. */
#define STEPPER_OL_DEFAULT_THRESH   400u   /* TRQ_COUNT 12bit, 越低越接近失速; <此值判高负载 */
#define STEPPER_OL_SAMPLE_MS        100u   /* 采样周期 */
#define STEPPER_OL_SAMPLES          6u     /* 6×100ms=600ms 持续低阈值 -> 高负载(滤瞬时) */
#define STEPPER_OL_DOWNGRADE_HZ     800u   /* 降速档 (微步/s) */
#define STEPPER_OL_DN_SAMPLES       24u    /* 降速后 24×100ms=2.4s 仍未回升 -> 停机 (合计~3s) */
#define STEPPER_RUN_MAX_MS          60000u /* 单次连续运行上限 (防机构长时间卡死通电) */

/* 停止原因 (统计 lastReason) */
#define STEPPER_RSN_NONE     0u
#define STEPPER_RSN_NORMAL   1u
#define STEPPER_RSN_TIMEOUT  2u
#define STEPPER_RSN_OVERLOAD 3u
#define STEPPER_RSN_DRVFAULT 4u

/* 行程开关错误位 (全局错误位, 经 QUERY/HEALTH 上报, CLEAR 清除) */
#define SWERR_UP     0x01u   /* 上行程 KEY_UP 缺失/未触发 */
#define SWERR_DOWN   0x02u   /* 下行程 KEY_DOWN 缺失/未触发 */

/* 实测双腿基准 (Protocol/Files/电机行程测试报告.html):
 * 上腿 4085 微步, 下腿 4324 微步 (1/2 档, 400 步/转). 连续运行超过
 * 基准×裕量仍未触发对应开关 -> 判开关通道缺失/失效. */
#define LEG_UP_STEPS       4085u
#define LEG_DOWN_STEPS     4324u
#define LEG_SW_MARGIN_PCT  15u          /* 裕量 15%, 容机构装配公差 */
#define LEG_UP_LIMIT    (LEG_UP_STEPS * (100u + LEG_SW_MARGIN_PCT) / 100u)
#define LEG_DOWN_LIMIT  (LEG_DOWN_STEPS * (100u + LEG_SW_MARGIN_PCT) / 100u)

static AppStepperState_t s_state = APP_STEPPER_IDLE;
static uint32_t          s_speedHz  = STEPPER_DEFAULT_HZ;
static uint32_t          s_cmdSpeedHz = STEPPER_DEFAULT_HZ;  /* 协议设定速(不随降速改变) */
static uint32_t          s_stepsReq = 0;      /* 本次目标微步数, 0=持续 */
static uint32_t          s_lastTickMs;
static uint8_t           s_fault, s_diag1, s_diag2;
static uint8_t           s_dir;
static uint8_t           s_switchErr = 0u;    /* 行程开关错误位 (SWERR_*) */
static uint32_t          s_legSteps  = 0u;    /* 本腿已走微步 (自启动方向计数) */

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

/* =====================================================================
 * 电机黑匣子 (现场堵转排查; RAM 记录, 断电清零, 上位机 MOTOR_CMD_TRACE 导出).
 *  每腿(每次 Move)复位采样环; RUN 中随 TRQ 采样(100ms)记录
 *  {TRQ, 相对起点ms, 累计步数, 行程开关/降速标志}; 每次真实停机(各停机
 *  路径, 仅在本腿确在 RUN 时)把终态快照入历史环(8条, 自然覆盖回零
 *  重试序列: 首驱瞬态/3次重试/换腿)。
 *  判读法 (根因定位):
 *   - 末条历史 reason=3(高负载) 且末段样本步数仍按速度推进:
 *     脉冲在走、TRQ 低于阈值 -> 看 TRQ 绝对值: 接近 0=转子失步真堵转
 *     (机构卡/转矩不足, 开环计数照走), 仅略低于阈值=监测误判;
 *   - reason=4 且 fault 位非零: 器件级 (UVLO=供电跌, OCP=过流,
 *     TF=过温, OL=开路) -> 朝硬件/供电方向查;
 *   - 样本 flags 中 KEY 位中途置位但机构未到触点 -> 行程开关误触发
 *     (回零 hit 判定无防抖, 一次误读即停+换向);
 *   - 历史环各次停机 steps 相近 -> 同一位置机械卡点; 位置随机 -> 转矩不足。
 * ===================================================================== */
#define MTRACE_SAMPLES     48u    /* 48×100ms = 4.8s, 覆盖单腿全程 */
#define MTRACE_HIST        8u    /* 停机历史环 (含回零重试序列) */
#define MTRACE_INTERVAL_MS 100u  /* 采样周期 (与 TRQ 过载采样同拍) */

typedef struct {
    uint16_t trq;       /* 本拍 TRQ_COUNT */
    uint16_t ms;        /* 自本腿起点 ms (低16位截断, 腿长<65s 足够) */
    uint32_t steps;     /* TIM4 累计微步 (开环指令步数) */
    uint8_t  flags;     /* bit0=KEY_UP按下 bit1=KEY_DOWN按下 bit2=降速中 */
} MTraceSmp_t;

typedef struct {
    uint32_t startCnt;  /* 全局启动序号 (对应 STATS startCount) */
    uint32_t steps;     /* 停机时累计步数 */
    uint32_t runMs;     /* 本腿运行时长 */
    uint16_t trqFinal;  /* 停机前最后一次 TRQ_COUNT */
    uint8_t  reason;    /* STEPPER_RSN_* */
    uint8_t  fault;     /* FAULT 寄存器 (FAULT 态冻结快照) */
    uint8_t  diag2;
    uint8_t  olov;      /* 停机时过载监测状态 */
    uint8_t  rsv;
} MTraceStop_t;

static MTraceSmp_t s_mtSmp[MTRACE_SAMPLES];
static MTraceStop_t s_mtHist[MTRACE_HIST];
static uint8_t  s_mtN;         /* 本腿已记样本数 (环满后覆盖最旧) */
static uint8_t  s_mtHead;      /* 采样环写指针 */
static uint8_t  s_mtHistN;     /* 历史环有效条数 */
static uint8_t  s_mtHistHead;  /* 历史环写指针 */
static uint8_t  s_trqPct;      /* 当前转矩档 (%), 终态上报 */

static void mtrace_reset(void)
{
    s_mtN = 0u; s_mtHead = 0u;
}

static void mtrace_sample(uint32_t now)
{
    MTraceSmp_t *s = &s_mtSmp[s_mtHead];
    s->trq   = s_trqCount;
    s->ms    = (uint16_t)(now - s_runStartMs);
    s->steps = s_legSteps;
    s->flags = (uint8_t)(((App_NewPeriph_ReadKeyUp()   == 0u) ? 1u : 0u) |
                          ((App_NewPeriph_ReadKeyDown() == 0u) ? 2u : 0u) |
                          ((s_olov != STEPPER_OL_NONE)         ? 4u : 0u));
    s_mtHead = (uint8_t)((s_mtHead + 1u) % MTRACE_SAMPLES);
    if (s_mtN < MTRACE_SAMPLES) s_mtN++;
}

/* 各停机路径调用; 上层腿失败重试里对已停电机的重复 Stop 不会走到这
 * (调用点均带 s_state==RUN 守卫), 历史环不混入空停机记录。 */
static void mtrace_stop(uint8_t reason)
{
    MTraceStop_t *h = &s_mtHist[s_mtHistHead];
    h->startCnt = s_stats.startCount;
    h->steps    = s_legSteps;
    h->runMs    = SysTickHl_GetMs() - s_runStartMs;
    h->trqFinal = s_trqCount;
    h->reason   = reason;
    h->fault    = s_fault;
    h->diag2    = s_diag2;
    h->olov     = (uint8_t)s_olov;
    h->rsv      = 0u;
    s_mtHistHead = (uint8_t)((s_mtHistHead + 1u) % MTRACE_HIST);
    if (s_mtHistN < MTRACE_HIST) s_mtHistN++;
}


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
    cfg.microstep    = STEPPER_MICROSTEP_DRV;
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
    s_switchErr = 0u; s_legSteps = 0u;
    s_lastTickMs = SysTickHl_GetMs();
    s_olov = STEPPER_OL_NONE;
    s_olovThresh = STEPPER_OL_DEFAULT_THRESH;
    s_trqCount = 0; s_olovCnt = 0; s_olovDnCnt = 0;
    s_olovTickMs = 0; s_runStartMs = 0; s_runMsAcc = 0;
    s_stats.runSeconds = 0; s_stats.startCount = 0; s_stats.lastReason = STEPPER_RSN_NONE;
    s_mtN = 0u; s_mtHead = 0u; s_mtHistN = 0u; s_mtHistHead = 0u;
    s_trqPct = STEPPER_PCT_DEFAULT;

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
    /* FAULT 态冻结快照: DRV8434S 状态寄存器读后自清, 停机后若继续轮询,
     * 触发停机那一拍的故障位 (OCP/UVLO/SPI_ERROR 等) 会被下一拍读掉,
     * QUERY 恒见 fault=0x00 无法归因 (历次 nFAULT 停转事故均此现象)。
     * 进入 FAULT 后不再覆盖, 保留停机瞬间的证据; ClearFault 显式清。 */
    if (s_state == APP_STEPPER_FAULT) return;
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
            /* 定步数运动完成: TIM4 走满 stepsReq 自动停脉冲(IsRunning=0),
             * 据此回 IDLE 并断电 (外部 Stop 类路径各自置状态, 不会走到这) */
            if (StepperTim4_IsRunning() == 0u) {
                s_legSteps = StepperTim4_GetStepsDone();
                mtrace_stop(STEPPER_RSN_NORMAL);
                stepper_disable_output();
                s_state = APP_STEPPER_IDLE;
                s_stats.lastReason = STEPPER_RSN_NORMAL;
                return;
            }

            /* 本腿行程计数 (自本次方向起动累加; MOVE 设了 s_legSteps=0) */
            s_legSteps = StepperTim4_GetStepsDone();

            /* 行程开关监控 (硬件保护, 不依赖位置): 本腿已超实测基准×裕量仍未触发
             * 奔向下一个行程开关 -> 判开关缺失/失效, 置错误位并停机, 防止硬顶挡块.
             *     上腿(dir=0, 奔 KEY_UP):  超过 LEG_UP_LIMIT 未触发 -> 上行程错
             *     下腿(dir=1, 奔 KEY_DOWN): 超过 LEG_DOWN_LIMIT 未触发 -> 下行程错 */
            if (s_dir == 0u && s_legSteps >= LEG_UP_LIMIT) {
                mtrace_stop(STEPPER_RSN_OVERLOAD);
                StepperTim4_Stop();
                stepper_disable_output();
                s_state = APP_STEPPER_IDLE;
                s_olov = STEPPER_OL_NONE;
                s_switchErr |= SWERR_UP;
                s_stats.lastReason = STEPPER_RSN_OVERLOAD;
                return;
            }
            if (s_dir == 1u && s_legSteps >= LEG_DOWN_LIMIT) {
                mtrace_stop(STEPPER_RSN_OVERLOAD);
                StepperTim4_Stop();
                stepper_disable_output();
                s_state = APP_STEPPER_IDLE;
                s_olov = STEPPER_OL_NONE;
                s_switchErr |= SWERR_DOWN;
                s_stats.lastReason = STEPPER_RSN_OVERLOAD;
                return;
            }

            /* 故障 (FAULT 位、失速 DIAG2 或 nFAULT 拉低) -> 停转 */
            if ((s_fault & DRV8434S_FLT_FAULT) ||
                (s_diag2 & DRV8434S_DIAG2_STALL) ||
                (drv8434s_check_fault_pin(&g_hMotor) == 0u)) {
                mtrace_stop(STEPPER_RSN_DRVFAULT);
                StepperTim4_Stop();
                s_state = APP_STEPPER_FAULT;
                stepper_disable_output();
                s_stats.lastReason = STEPPER_RSN_DRVFAULT;
                return;
            }

            /* 连续运行时限: 防机构长时间卡死持续通电 */
            if ((now - s_runStartMs) >= STEPPER_RUN_MAX_MS) {
                mtrace_stop(STEPPER_RSN_TIMEOUT);
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
                        mtrace_stop(STEPPER_RSN_OVERLOAD);
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
                /* 黑匣子采样: 与过载判读同拍, flags 中的降速位反映本拍判后状态 */
                mtrace_sample(now);
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
    s_legSteps = 0;
    mtrace_reset();                     /* 黑匣子: 新腿覆盖旧采样 */
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
    if (s_state == APP_STEPPER_RUN) mtrace_stop(STEPPER_RSN_NORMAL);  /* 仅真实停机入历史环 */
    s_stepsReq = 0;
    StepperTim4_Stop();
    stepper_disable_output();
    s_state = APP_STEPPER_IDLE;
    s_olov = STEPPER_OL_NONE;
    s_stats.lastReason = STEPPER_RSN_NORMAL;   /* lastReason=最近停止, 每次覆盖 */
    return 0;
}

/* DC 磁制动停止: Stop 断电前短暂维持磁场(EN_OUT=1)抵抗惯性滑行, 再关断输出.
 * 用于触点接触的停位事件(如 KEY_UP 警戒线硬停), 减少"到了还在沿旧趋势走"的超程.
 * 罕见事件上的短暂阻塞在单任务 1ms 主循环下可接受. */
#define STEPPER_DC_BRAKE_MS  8u
int App_Stepper_DcBrakeStop(void)
{
    if (s_state == APP_STEPPER_RUN) mtrace_stop(STEPPER_RSN_NORMAL);
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
    s_trqPct = pct;                     /* 黑匣子终态上报用 */
    stepper_set_trq(pct);
    return 0;
}

int App_Stepper_ClearFault(void)
{
    (void)drv8434s_clear_fault(&g_hMotor);
    s_fault = 0; s_diag1 = 0; s_diag2 = 0;
    s_switchErr = 0u;                       /* 一并清行程开关错误位 (运行/回零时检测) */
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
uint8_t  App_Stepper_GetSwitchErr(void) { return s_switchErr; }
void     App_Stepper_SetSwitchErr(uint8_t bit) { s_switchErr |= (uint8_t)(bit & (SWERR_UP | SWERR_DOWN)); }
uint8_t  App_Stepper_GetMicrostep(void)   /* 诊断: 实时读 DRV8434S CTRL3 微步位 [3:0] */
{ return (uint8_t)(drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3) & DRV8434S_CTRL3_MICROSTEP_MASK); }
uint8_t  App_Stepper_GetMicrostepCfg(void)  /* 自检判据: 上电压入 DRV8434S 的微步档期望值 */
{ return (uint8_t)STEPPER_MICROSTEP_DRV; }
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

/* 黑匣子导出 (MOTOR_CMD_TRACE 0x20/0x0A). 全 LE, 布局见 App_CustomProtocol.h:
 * 头 20B(实时态) + 停机历史环(最新->最旧, 各 19B) + 采样环(最新->最旧, 各 9B)。
 * 历史环最新一条即最近一次停机终态 (含 fault 冻结快照); 采样环为最近一腿
 * (每次 Move 覆盖) 的 TRQ/步数/开关轨迹, 断电清零。 */
const uint8_t *App_Stepper_TraceDump(uint16_t *len)
{
    static uint8_t buf[20u + MTRACE_HIST * 19u + MTRACE_SAMPLES * 9u];
    uint16_t p = 0u;
    uint8_t i;

    buf[p++] = 0x0Au;                     /* cmd 回显 */
    buf[p++] = 0u;                        /* err=OK */
    buf[p++] = (uint8_t)s_state;          /* 实时: 步进态 */
    buf[p++] = s_fault;
    buf[p++] = s_diag1;
    buf[p++] = s_diag2;
    buf[p++] = (uint8_t)s_olov;
    buf[p++] = (uint8_t)(s_olovThresh & 0xFFu);       /* 7..8: 过载阈值 */
    buf[p++] = (uint8_t)(s_olovThresh >> 8);
    buf[p++] = s_trqPct;                  /* 9: 当前转矩档 % */
    buf[p++] = s_dir;                     /* 10 */
    buf[p++] = s_switchErr;               /* 11 */
    buf[p++] = (uint8_t)(s_speedHz & 0xFFu);          /* 12..15: 速度 */
    buf[p++] = (uint8_t)((s_speedHz >> 8) & 0xFFu);
    buf[p++] = (uint8_t)((s_speedHz >> 16) & 0xFFu);
    buf[p++] = (uint8_t)((s_speedHz >> 24) & 0xFFu);
    buf[p++] = s_mtN;                     /* 16: 样本数 */
    buf[p++] = s_mtHistN;                 /* 17: 历史条数 */
    buf[p++] = (uint8_t)MTRACE_INTERVAL_MS;          /* 18: 采样间隔 ms */
    buf[p++] = 0u;                        /* 19: 预留 */

    for (i = 0u; i < s_mtHistN; i++) {
        const MTraceStop_t *h = &s_mtHist[(uint8_t)((s_mtHistHead + MTRACE_HIST - 1u - i) % MTRACE_HIST)];
        buf[p++] = (uint8_t)(h->startCnt & 0xFFu);
        buf[p++] = (uint8_t)((h->startCnt >> 8) & 0xFFu);
        buf[p++] = (uint8_t)((h->startCnt >> 16) & 0xFFu);
        buf[p++] = (uint8_t)((h->startCnt >> 24) & 0xFFu);
        buf[p++] = (uint8_t)(h->steps & 0xFFu);
        buf[p++] = (uint8_t)((h->steps >> 8) & 0xFFu);
        buf[p++] = (uint8_t)((h->steps >> 16) & 0xFFu);
        buf[p++] = (uint8_t)((h->steps >> 24) & 0xFFu);
        buf[p++] = (uint8_t)(h->runMs & 0xFFu);
        buf[p++] = (uint8_t)((h->runMs >> 8) & 0xFFu);
        buf[p++] = (uint8_t)((h->runMs >> 16) & 0xFFu);
        buf[p++] = (uint8_t)((h->runMs >> 24) & 0xFFu);
        buf[p++] = (uint8_t)(h->trqFinal & 0xFFu);
        buf[p++] = (uint8_t)(h->trqFinal >> 8);
        buf[p++] = h->reason;
        buf[p++] = h->fault;
        buf[p++] = h->diag2;
        buf[p++] = h->olov;
        buf[p++] = h->rsv;
    }

    for (i = 0u; i < s_mtN; i++) {
        const MTraceSmp_t *s = &s_mtSmp[(uint8_t)((s_mtHead + MTRACE_SAMPLES - 1u - i) % MTRACE_SAMPLES)];
        buf[p++] = (uint8_t)(s->trq & 0xFFu);
        buf[p++] = (uint8_t)(s->trq >> 8);
        buf[p++] = (uint8_t)(s->ms & 0xFFu);
        buf[p++] = (uint8_t)(s->ms >> 8);
        buf[p++] = (uint8_t)(s->steps & 0xFFu);
        buf[p++] = (uint8_t)((s->steps >> 8) & 0xFFu);
        buf[p++] = (uint8_t)((s->steps >> 16) & 0xFFu);
        buf[p++] = (uint8_t)((s->steps >> 24) & 0xFFu);
        buf[p++] = s->flags;
    }

    if (len) *len = p;
    return buf;
}
