#include "App_BootSelfTest.h"
#include "App_Config.h"
#include "App_SysTick_HL.h"
#include "App_NewPeriph_HL.h"
#include "App_Stepper.h"
#include "App_UHF.h"
#include "App_AM.h"
#include "App_Iwdg_HL.h"
#include "App_Motor_HL.h"
#include "drv8434s.h"
#include "stm32f10x.h"

/* =====================================================================
 * 上电自检 (POST)
 *   1. 蜂鸣器响 500ms 提示上电
 *   2. 逐个探测外设 (步进电机 / UHF / AM) 并读其状态
 *   3. 结果(含通信错误)经调试串口 (UART4) 打印
 *
 * 时序: 在开全局中断后调用一次 (UHF/AM 帧收依赖 USART 收中断)。
 * 阻塞等待模块回帧期间依靠 App_Iwdg_Feed() 喂狗, 避免 IWDG 复位。
 * 自检不改变各外设配置/运行状态 (UHF 用 App_UHF_Query 只读探测,
 * 电机用 SPI 读 Rev_ID 只读探测), 失败也不会把设备至于不可用态。
 * ===================================================================== */

#define SELFTEST_BEEP_MS   500u
#define SELFTEST_QUERY_MS  250u

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
    while (v && i >= 0) { buf[i] = hx[v & 0xFu]; v >>= 4; i--; }
    App_NewPeriph_DebugPutStr(&buf[i + 1]);
}

/* ---- 步进电机自检: SPI 读回配置 + 状态/故障 ----
 * DRV8434S 量产片 REV_ID 默认 = 0000b (数据手册 8.12), 不能以 rev_id 判通信。
 * 改为读回应用在 drv8434s_init() 中写入的非零配置寄存器来回验证通信:
 *   - CTRL3 微步模式 (App_Stepper 固定 1/16 => 低 4 位 = 0x6)
 *   - CTRL7 EN_SSC (init 使能扩频 => bit5=1, 0x20)
 * 读回值与预期一致 => SPI 已通、配置已落进芯片; 再配合 fault 判健康。
 * rev_id 仅打印参考, 不参与判定 (量产片即 0)。 */
static void selftest_motor(void)
{
    uint8_t ctrl3 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL3);
    uint8_t ctrl7 = drv8434s_read_reg(&g_hMotor, DRV8434S_REG_CTRL7);
    uint8_t mic   = (uint8_t)(ctrl3 & DRV8434S_CTRL3_MICROSTEP_MASK);

    putstr("[POST] Motor DRV8434S  ");
    if (App_Stepper_GetState() == APP_STEPPER_FAULT)
        putstr("state=FAULT");
    else
        putstr("state=IDLE");
    putstr(" fault="); puthex(App_Stepper_GetFault());
    putstr(" ctrl3="); puthex(ctrl3);
    putstr(" micro="); puthex(mic);
    putstr(" ctrl7="); puthex(ctrl7);
    putstr(" rev=");   puthex(drv8434s_get_rev_id(&g_hMotor));

    int comm_ok = (mic == DRV8434S_MICROSTEP_1_16) &&
                  ((ctrl7 & DRV8434S_CTRL7_EN_SSC) != 0u);
    int fault   = (App_Stepper_GetFault() & DRV8434S_FLT_FAULT) != 0u;

    if (comm_ok && !fault) putstr(" --MOTOR OK\r\n");
    else                   putstr(" --MOTOR COMM/CFG FAIL\r\n");
}

/* ---- UHF 自检: 上电 + Query 只读探测链路 ----
 * 不进入盘点; 探测后保持上电就绪, 供后续业务直接使用.
 * 未接模块 => 超时, 打印通信错误. */
static void selftest_uhf(void)
{
    putstr("[POST] UHF   ");

    /* 上电 uff 经 App_UHF_Open (电源 + 配置下发); 失败多发生在通信.
     * 调用前已由调用方喂狗, Open 内部最长阻塞(上电100ms+2帧回帧) < IWDG 预算. */
    int r = App_UHF_Open();
    putstr("open="); putdec((uint32_t)r);

    r = App_UHF_Query();
    putstr(" query="); putdec((uint32_t)r);

    AppUHFState_t st = App_UHF_GetState();
    putstr(" state="); putdec((uint32_t)st);
    putstr(" link=");  putdec((uint32_t)App_UHF_GetLinkStatus());
    if (r == 0) putstr(" --UHF OK\r\n");
    else        putstr(" --UHF COMM FAIL\r\n");
}

/* ---- AM 自检: 总查询只读探测链路 ----
 * AM→RS485 经 App_AM_Query (2A A2 查询帧 + 回帧), 校验失败/超时 => 通信错误. */
static void selftest_am(void)
{
    putstr("[POST] AM    ");
    int r = App_AM_Query();
    putstr(" query="); putdec((uint32_t)r);
    putstr(" link=");  putdec((uint32_t)App_AM_GetLinkStatus());
    if (r == 0) putstr(" --AM(RS485) OK\r\n");
    else        putstr(" --AM(RS485) COMM FAIL\r\n");
}

void App_BootSelfTest_Run(void)
{
    /* 1. 蜂鸣器响 500ms (阻塞; 先喂狗, 保证 IWDG 不在此间复位) */
    IwdgHl_Feed();
    App_NewPeriph_Beep(1u);
    SysTickHl_DelayMs(SELFTEST_BEEP_MS);
    App_NewPeriph_Beep(0u);

    /* 2/3. 逐个外设探测 + 串口打印.
     * 每个模块调用可能阻塞在 500ms 回帧超时上 (模块未接时尤其),
     * 故在每个阻塞调用前喂狗一次, 使最长无喂狗间隔 = 单次最长调用
     * (UHF Open 上电+2 帧 ≈1.1s) < IWDG 2s 预算, 避免复位死循环. */
    putstr("\r\n==== ZLR5401 Power-On Self Test ====\r\n");
    putstr("HW "); putstr(DEV_HW_VERSION);
    putstr("  SW "); putstr(DEV_SW_VERSION);
    putstr("\r\n");

    IwdgHl_Feed();
    selftest_motor();
    IwdgHl_Feed();
    selftest_uhf();
    IwdgHl_Feed();
    selftest_am();

    putstr("====================================\r\n");
}
