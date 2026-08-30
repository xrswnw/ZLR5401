#include "App_BootSelfTest.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "App_NewPeriph_HL.h"
#include "App_Stepper.h"
#include "App_UHF.h"
#include "App_AM.h"
#include "App_Iwdg_HL.h"
#include "App_Motor_HL.h"
#include "App_RgbLed_HL.h"
#include "drv8434s.h"
#include "stm32f10x.h"

/* =====================================================================
 * 上电自检 (POST) + 设备级自检错误位 (锁存位图)
 *   1. RGB 彩灯自检 (白常亮 200ms): 三色硬件已全接, 同亮即白, 一次验证三通道
 *   2. 逐个探测外设 (步进电机 / UHF / AM), 结果沉淀为 16bit 锁存错误位
 *      (App_SelfTest_*), 并经调试串口打印
 *   3. 探测结果经 FC_SELFTEST_CTRL (0x0F) 供上位机 读/重探/清
 *   4. 引导期灯语: 回零黄常亮, 探测期黄慢闪 (窗口节拍), 探测结束统一熄灭
 *      交主循环仲裁
 *
 * 时序: 在开全局中断后调用一次 (UHF/AM 帧收依赖 USART 收中断)。
 * 阻塞等待模块回帧期间依靠 App_Iwdg_Feed() 喂狗, 避免 IWDG 复位。
 * 自检不改变各外设配置/运行状态 (UHF 用 App_UHF_Open/Query 只读探测,
 * 电机用 SPI 读回配置只读探测), 失败也不会把设备置于不可用态。
 * ===================================================================== */

#define SELFTEST_QUERY_MS  250u

/* ---- 锁存状态 ---- */
static uint16_t s_errBits;    /* 锁存错误位图 (POST 置位, RERUN 刷新外设位, CLEAR 清) */
static uint16_t s_probeLast;  /* 最近一次外设探测结果位图 (原始, 不含 PARAM_CRC) */

/* ---- 微型无依赖打印 (避免链接浮点/softlib) ---- */
static void putstr(const char *s)
{
    App_NewPeriph_DebugPutStr(s);
}

static void putdec(uint32_t v)
{
    char buf[12];
    int  i = 11;
    buf[i--] = '\0';
    if (v == 0) buf[i--] = '0';
    while (v && i >= 0) { buf[i--] = (char)('0' + (v % 10u)); v /= 10u; }
    App_NewPeriph_DebugPutStr(&buf[i + 1]);
}

static void puthex(uint32_t v)
{
    char buf[12];
    const char hx[] = "0123456789ABCDEF";
    int  i = 10;
    buf[11] = '\0';
    if (v == 0) buf[i--] = '0';
    while (v && i >= 0) { buf[i--] = hx[v & 0xFu]; v >>= 4; i--; }
    App_NewPeriph_DebugPutStr(&buf[i + 1]);
}

/* ---- 锁存错误位 API ---- */
uint16_t App_SelfTest_GetErrBits(void) { return s_errBits; }

void App_SelfTest_SetErrBits(uint16_t mask)
{
    s_errBits |= (uint16_t)(mask & SELF_ERR_KNOWN_MASK);
}

void App_SelfTest_ClearErrBits(uint16_t mask)
{
    s_errBits &= (uint16_t)~(uint16_t)(mask & SELF_ERR_KNOWN_MASK);
}

/* ---- 步进电机探测: SPI 读回配置 + 状态/故障, 返回错误位 ----
 * DRV8434S 量产片 REV_ID 默认 = 0000b (数据手册 8.12), 不能以 rev_id 判通信。
 * 改为读回应用在 drv8434s_init() 中写入的非零配置寄存器来回验证通信:
 *   - CTRL3 微步档 = App_Stepper 上电压入值 (App_Stepper_GetMicrostepCfg)
 *   - CTRL7 EN_SSC (init 使能扩频 => bit5=1)
 * 读回值与预期一致 => SPI 已通、配置已落进芯片; 再配合 fault 判健康。
 * rev_id 仅打印参考, 不参与判定 (量产片即 0)。 */
static uint16_t probe_motor(int verbose)
{
    uint8_t ctrl3 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3);
    uint8_t ctrl7 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL7);
    uint8_t mic   = (uint8_t)(ctrl3 & DRV8434S_CTRL3_MICROSTEP_MASK);
    uint8_t fault = App_Stepper_GetFault();
    uint16_t bits = 0u;

    int comm_ok = (mic == App_Stepper_GetMicrostepCfg()) &&
                  ((ctrl7 & DRV8434S_CTRL7_EN_SSC) != 0u);
    if (!comm_ok)                        bits |= SELF_ERR_MOTOR_SPI;
    if ((fault & DRV8434S_FLT_FAULT) != 0u) bits |= SELF_ERR_MOTOR_FAULT;

    if (verbose) {
        putstr("[POST] Motor DRV8434S  ");
        if (App_Stepper_GetState() == APP_STEPPER_FAULT)
            putstr("state=FAULT");
        else
            putstr("state=IDLE");
        putstr(" fault="); puthex(fault);
        putstr(" ctrl3="); puthex(ctrl3);
        putstr(" micro="); puthex(mic);
        putstr(" ctrl7="); puthex(ctrl7);
        putstr(" rev=");   puthex(drv8434s_get_rev_id(&g_hMotor));
        if (bits == 0u) putstr(" --MOTOR OK\r\n");
        else            putstr(" --MOTOR COMM/CFG FAIL\r\n");
    }
    return bits;
}

/* ---- UHF 探测: 上电 + Query 只读探测链路, 返回错误位 ----
 * 不进入盘点; 探测后保持上电就绪, 供后续业务直接使用.
 * 未接模块 => 超时, 置 UHF_COMM. */
static uint16_t probe_uhf(int verbose)
{
    uint16_t bits = 0u;

    if (verbose) putstr("[POST] UHF   ");

    /* 上电经 App_UHF_Open (电源 + 配置下发); 失败多发生在通信.
     * 调用前已由调用方喂狗, Open 内部最长阻塞(上电100ms+2帧回帧) < IWDG 预算. */
    int r = App_UHF_Open();
    if (verbose) { putstr(" open="); putdec((uint32_t)r); }

    r = App_UHF_Query();
    if (verbose) {
        putstr(" query="); putdec((uint32_t)r);
        putstr(" state="); putdec((uint32_t)App_UHF_GetState());
        putstr(" link=");  putdec((uint32_t)App_UHF_GetLinkStatus());
    }
    if (r != 0) {
        bits |= SELF_ERR_UHF_COMM;
        if (verbose) putstr(" --UHF COMM FAIL\r\n");
    } else if (verbose) {
        putstr(" --UHF OK\r\n");
    }
    return bits;
}

/* ---- AM 探测: 总查询只读探测链路, 返回错误位 ----
 * AM→RS232 经 App_AM_Query (2A A2 查询帧 + 回帧), 校验失败/超时 => AM_COMM.
 * 解码器对半截帧会失步 (如前次复位停在 TX 半途), 首查无应答时重试一次. */
static uint16_t probe_am(int verbose)
{
    uint16_t bits = 0u;

    if (verbose) putstr("[POST] AM    ");
    int r = App_AM_Query();
    if (r != 0) {
        IwdgHl_Feed();               /* 首查最坏耗尽 1.2s 窗口, 先喂狗再重试 */
        SysTickHl_DelayMs(100u);
        IwdgHl_Feed();
        r = App_AM_Query();          /* 失步自愈重试 */
    }
    if (verbose) {
        putstr(" query="); putdec((uint32_t)r);
        putstr(" link=");  putdec((uint32_t)App_AM_GetLinkStatus());
    }
    if (r != 0) {
        bits |= SELF_ERR_AM_COMM;
        if (verbose) putstr(" --AM(RS232) COMM FAIL\r\n");
    } else if (verbose) {
        putstr(" --AM(RS232) OK\r\n");
    }
    return bits;
}

/* ---- 外设探测汇总: 电机 + UHF + AM + 行程开关, 返回本次结果位图 ----
 * 每个模块调用可能阻塞在 500ms 回帧超时上 (模块未接时尤其),
 * 故每个阻塞调用前喂狗一次, 使最长无喂狗间隔 = 单次最长调用
 * (UHF Open 上电+2 帧 ≈1.1s) < IWDG 2s 预算, 避免复位死循环。 */
static uint16_t probe_all(int verbose)
{
    uint16_t bits = 0u;

    if (verbose) {
        putstr("\r\n==== ZLR5401 Power-On Self Test ====\r\n");
        putstr("HW "); putstr(DEV_HW_VERSION);
        putstr("  SW "); putstr(DEV_SW_VERSION);
        putstr("\r\n");
    }

    /* 探测期黄慢闪: 以模块探测窗口为节拍 —— 亮≈电机+UHF 窗口, 灭 300ms 间隙,
     * 再亮=AM 窗口, 周期 ~1.4s (与业务回零黄慢闪 HOMING_SLOW 同量级).
     * 开机语境: 回零黄常亮 -> 白闪自检 -> 黄慢闪(探测) -> 灭, 阶段推进可辨. */
    RgbLedHl_Set(RGB_BIT_G | RGB_BIT_R);
    bits |= probe_motor(verbose);
    IwdgHl_Feed();
    bits |= probe_uhf(verbose);

    RgbLedHl_Set(0u);
    SysTickHl_DelayMs(300u);               /* 灭半拍: 分隔探测窗口 */
    IwdgHl_Feed();

    RgbLedHl_Set(RGB_BIT_G | RGB_BIT_R);
    bits |= probe_am(verbose);
    RgbLedHl_Set(0u);

    /* 行程开关: 回零失败/运行监控置位时并入 (bit0=上 bit1=下) */
    if (App_Stepper_GetSwitchErr() != 0u) bits |= SELF_ERR_TRAVEL_SW;

    return bits;
}

/* 锁存外设/行程位刷新 (PARAM_CRC 为引导期事实, 不随探测清) */
static void latch_refresh(uint16_t probeBits)
{
    s_errBits &= (uint16_t)~(SELF_ERR_MOTOR_SPI | SELF_ERR_MOTOR_FAULT |
                             SELF_ERR_UHF_COMM  | SELF_ERR_AM_COMM   |
                             SELF_ERR_TRAVEL_SW);
    s_errBits |= probeBits;
    s_probeLast = probeBits;
}

uint16_t App_SelfTest_ProbePeripherals(void)
{
    uint16_t bits = probe_all(0);
    latch_refresh(bits);
    return bits;
}

void App_SelfTest_ReadLive(SelfTestLive_t *out)
{
    if (out == 0) return;
    uint8_t ctrl3 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3);
    uint8_t ctrl7 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL7);

    out->motorCommOk = (((uint8_t)(ctrl3 & DRV8434S_CTRL3_MICROSTEP_MASK)
                         == App_Stepper_GetMicrostepCfg()) &&
                        ((ctrl7 & DRV8434S_CTRL7_EN_SSC) != 0u)) ? 1u : 0u;
    out->motorFaultReg = App_Stepper_GetFault();
    out->uhfLink   = (uint8_t)App_UHF_GetLinkStatus();
    out->amLink    = (uint8_t)App_AM_GetLinkStatus();
    out->switchErr = App_Stepper_GetSwitchErr();
}

void App_BootSelfTest_Run(void)
{
    /* 0. RGB 灯带自检: 白常亮 200ms. 三色硬件已全接 (RGB_HAS_BLUE=1),
     * 同亮即白, 一次点亮即验证 R/G/B 三通道. 阻塞期间喂狗. */
    IwdgHl_Feed();
    RgbLedHl_Set(RGB_BIT_R | RGB_BIT_G | RGB_BIT_B);
    SysTickHl_DelayMs(200u);
    IwdgHl_Feed();
    RgbLedHl_Set(0u);   /* 熄灭半拍再进探测黄慢闪 (probe_all 内以窗口为节拍) */

    /* 1. 蜂鸣器提示已移除 (默认上电不响);
     * 成功提示移至 POST 末尾 (见下), 判据 = 电机 + UHF + AM 通信全通过. */

    /* 2/3. 逐个外设探测 + 错误位沉淀 + 串口打印 (黄慢闪, probe_all 内节拍). */
    uint16_t bits = probe_all(1);
    latch_refresh(bits);

    /* 4. 成功蜂鸣 1s: 电机自检 (SPI 回读/器件故障/行程开关) + UHF 通信
     * + AM(RS232) 通信 全部通过才响; 任一失败则静默, 由错误灯语表达. */
    if ((bits & (uint16_t)(SELF_ERR_MOTOR_SPI | SELF_ERR_MOTOR_FAULT |
                          SELF_ERR_TRAVEL_SW | SELF_ERR_UHF_COMM |
                          SELF_ERR_AM_COMM)) == 0u) {
        IwdgHl_Feed();
        App_NewPeriph_Beep(1u);
        SysTickHl_DelayMs(1000u);
        IwdgHl_Feed();
        App_NewPeriph_Beep(0u);
    }

    /* 5. 引导期结束: 熄灭 RGB, 交主循环灯语仲裁器接管. */
    RgbLedHl_Set(0u);

    putstr("====================================\r\n");
}
