#include "stm32f10x.h"
#include "App_Config.h"
#include "App_Param.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"   /* IwdgHl_Init/Feed (Boot 启动 IWDG 持续到 App)*/
#include "App_Led_HL.h"
#include "App_Led.h"
#include "App_RgbLed_HL.h"
#include "App_RgbLed_Pattern.h"
#include "App_Usb.h"
#include "App_Usb_HL.h"
#include "App_Sys_CfgClock.h"
#include "System_PeriphClk.h"
#include "App_Dispatch.h"
#include "App_CustomProtocol.h"
#include "App_Stepper.h"
#include "App_UHF.h"
#include "App_AM.h"
#include "App_Locker.h"
#include "App_NewPeriph_HL.h"
#include "App_MotorTest.h"
#include "App_MotorHoming.h"
#include "App_BootSelfTest.h"

/* TEMP: AM 线路测试模式 —— 主循环每 500ms 发 0x63 总查询探链,
 * RGB 绿=链路通 / 红=无应答; 期间禁用灯语仲裁避免覆盖测试指示.
 * 查询为阻塞: 未接 AM ~200ms 超时, 已接 ~1.2s 取 12 帧 (IWDG 已喂, <2s 预算).
 * 线路排查完成后置 0 恢复正常灯语. */
#define AM_LINE_TEST 0

void System_Init(void)
{
    /* 严格按 Sys_Init() 顺序, USB 初始化完成后再开全局中断*/

    /* 1. 时钟树: HSE 12MHz × PLL6 = SYSCLK 72MHz, AHB/PCLK2=72MHz, PCLK1=36MHz
     * main 显式调用, 不依赖 startup -> SystemInit() 隐式路径*/
    App_Sys_CfgClock();
    /* (SystemCoreClockUpdate 已在 App_Sys_CfgClock 末尾同步)*/

    /* 2. 外设 RCC 时钟统一使能 (替代各 HL 分散调用)*/
    System_PeriphClkInit();

    /* 3. 设置 VTOR 指向 APP flash 起始 (App 独立运行向量表)*/
    SCB->VTOR = APP_FLASH_ORIGIN;

    /* 4. 关全局中断 (Sys_DisableInt), 在所有外设初始化完成前不要开*/
    __asm volatile ("cpsid i");

    /* 5. SysTick (1ms 基准, 供 LED 心跳/延时)*/
    SysTickHl_Init();

    /* 5.5 IWDG 尽早重新初始化并喂狗. Boot 经 JumpToApp 跳入 App (非复位),
     * IWDG 仍在运行; 此处重载计数器并开始喂狗节奏, 避免跳转间隙累计超 2s.*/
    IwdgHl_Init();
    IwdgHl_Feed();

    /* 6. LED*/
    LedHl_Init();
    RgbLedHl_Init();   /* RGB 三色灯 G=PA4/R=PA5/B=PA6, 默认全灭 */
    App_RgbLedPat_Init();   /* RGB 灯语仲裁器 (状态指示, 见 App_RgbLed_Pattern) */

    /* 6.5 新增外设: 光电/行程开关/蜂鸣器 GPIO + 调试串口 (骨架)*/
    App_NewPeriph_Init();

    /* 7. USB HID (USB_EN=PA8 使能, AF_PP 配置 PA11/12 + GPIO_SetBits USB_EN)*/
    App_Usb_Init();

    /* 8.4 参数区加载: 主区/影子区 CRC 失败时回落到默认值并写回 flash;
     * 失败事实沉淀到设备级自检错误位 (SELF_ERR_PARAM_CRC, 仅上电置位,
     * 经 FC_SELFTEST_CTRL 0x0F 可读, 只能手动 CLEAR). */
    if (ParamLoad(&g_sParam) != 0) {
        App_SelfTest_SetErrBits(SELF_ERR_PARAM_CRC);
        ParamInit(&g_sParam);
        (void)ParamSave(&g_sParam);
    }
    /* App 启动时计算 deviceUidHash (Bootloader 在 main.c:139 已算)。
     * 若 ParamInit/ParamLoad 后仍为 0 (老参数区未写入 hash), 重新算并写回 flash。*/
    if (g_sParam.deviceUidHash == 0U) {
        g_sParam.deviceUidHash = Crc32CalcUid();
        (void)ParamSave(&g_sParam);
    }
    /* 校验 baudRate 合理性: 0 / 不在常见 baud list 时强制覆盖默认 115200.
     * (修复 flash param 区损坏导致 baudRate=0x1F0001C2 等异常值的问题)*/
    {
        uint32_t b = g_sParam.baudRate;
        int sane = (b == 1200U || b == 2400U || b == 4800U || b == 9600U ||
                    b == 14400U || b == 19200U || b == 38400U || b == 57600U ||
                    b == 115200U || b == 128000U || b == 256000U || b == 921600U);
        if (!sane) {
            g_sParam.baudRate = 115200U;
            (void)ParamSave(&g_sParam);
        }
    }

    /* 8.5 协议分派层初始化: Proto_Init + 注册 AppDispatch 回调
     * AppDispatchInit -> Proto_Init 会 Memset8 清空 g_sChannels,
     * 把 App_Usb_Init 早先注册的 USB transport 清成 NULL。
     * 起 Proto_Poll 经 transport->ops 读环形缓冲, ops==NULL 会
     * 跳过 USB 通道 → EP2_OUT_User 收到的帧永远不被解析 → 握手无响应。
     * 故 USB transport 必须在 Proto_Init 之后(此处)重新注册一次。*/
    AppDispatchInit();
    Proto_RegisterTransport(PROTO_CH_USB, App_Usb_GetTransport());

    /* 8.6 步进电机初始化 (SPI2+GPIO 已由 System_PeriphClkInit 使能时钟;
     * DRV8434S 上电配置 + 停转, IDLE 无输出) */
    App_Stepper_Init();

    /* 8.7 UHF 模块初始化 (USART3 驱动 + 状态机, 不主动上电;
     * 配置由 上位机 SET_CONFIG 下发或在 打开时按持久化配置应用) */
    App_UHF_Init();
    {
        /* 加载上次持久化的 UHF 配置 (功率/天线/校验等) 到状态机,
         * 使断电重启后仍保留上位机最后一次下发值 */
        UHFUserCfg_t pc;
        if (UhfParam_Load(&pc) == 0) {
            AppUHFConfig_t c;
            App_UHF_GetConfig(&c);
            c.powerDbm = pc.powerDbm; c.antenna = pc.antenna;
            c.checksumEn = pc.checksumEn; c.session = pc.session;
            c.target = pc.target; c.q = pc.q; c.band = pc.band;
            (void)App_UHF_SetConfig(&c, 0);
        }
    }

    /* 8.8 AM 消磁器初始化 (RS232/USART1 驱动 + 状态复位, 不下发;
     * 持久化配置复位移至 9.4 —— 见下) */
    App_AM_Init();

    /* 8.9 开锁器业务编排层初始化 (IDLE, 默认锁定)*/
    App_Locker_Init();

    /* 8.10 电机行程测试状态机初始化 (IDLE, 不占用电机)*/
    App_MotorTest_Init();

    /* 9. 开全局中断 (最后一步 Sys_EnableInt, USB 准备就绪后才开)*/
    __asm volatile ("cpsie i");

    /* 9.4 AM 持久化配置复位 (开中断后执行 — 不可提前)
     * App_AM_SetConfig 为阻塞收发: 等回帧依赖 USART1 RX 中断, 500ms
     * 超时依赖 SysTick ms 计数. 此前放在 8.8 (步骤4~9 关中断期),
     * 一旦 AM 解码器不在设备侧 (接上位机/未接), 回帧永不到且超时
     * 永不成立 -> 死循环 -> IWDG 复位循环, 上电看似"不运行"。 */
    {
        AMUserCfg_t pc;
        if (AmParam_Load(&pc) == 0) {
            AppAMConfig_t c;
            c.threshold   = pc.threshold;
            c.hitCount    = pc.hitCount;
            c.freqRange   = pc.freqRange;
            c.recvDelay   = pc.recvDelay;
            c.recvLength  = pc.recvLength;
            c.phaseInvert = pc.phaseInvert;
            c.phaseSync   = pc.phaseSync;
            c.decodeVolt  = pc.decodeVolt;
            c.mode        = pc.mode;
            c.mainsFreq   = pc.mainsFreq;
            IwdgHl_Feed();
            (void)App_AM_SetConfig(&c, 0);
        }
    }

    /* 9.5 上电行程自检/回零: 用行程开关建立绝对位置基准.
     * 必须在开全局中断后调用 —— 回零阻塞驱动依赖 TIM4 中断(推进 STEP)
     * 与 SysTick 中断(ms 计时), 中断关着会卡死; 期间喂狗.
     * 上电首驱存在间歇性 nFAULT/开关漏读 (实测 ~1/3 概率单次失败, 再次
     * 驱动即正常): 失败自动重试至多 3 次并清故障寄存器, 全部失败才置
     * 错误位, 上层 MOVE/TEST 将被禁止以防冲挡块. */
    /* 回零为阻塞循环 (主循环不跑), 无法走 Tick 闪烁 -> 黄常亮示意;
     * 结束保持黄, 衔接 POST (彩灯自检白闪后回到黄, 探测全程黄常亮),
     * 由 POST 末尾统一熄灭, 交主循环灯语仲裁器接管。 */
    RgbLedHl_Set(RGB_BIT_G | RGB_BIT_R);
    for (uint8_t homTry = 0u; homTry < 3u; homTry++) {
        if (App_MotorHoming_Run() == MOTOR_HOMING_OK) break;
        (void)App_Stepper_ClearFault();
        SysTickHl_DelayMs(200u);
        IwdgHl_Feed();
    }

    /* 9.6 上电自检 (POST): 蜂鸣器 500ms + 外设链路监控经调试串口打印.
     * 需在开全局中断后调用 (UHF/AM 帧回依赖 USART 收中断). 阻塞期间喂狗.
     * 呼吸灯由主循环 AppLedProcess 持续运行 (Task 2 常驻). */
    App_BootSelfTest_Run();
}

int main(void)
{
    System_Init();

    while (1)
    {
        /* 协议层轮询: 把 USB(HID 中断已收)字节喂给解析器 → 分派 → 响应经 EP1 IN 发回*/
        Proto_Poll();

        /* 主循环喂狗.*/
        IwdgHl_Feed();

        /* 蜂鸣脉冲到期自动停: BeepPulse(50ms/300ms) 只开不关,
         * IR 蜂鸣流程删除时原轮询者一并丢失, 曾导致一次触发即长响. */
        (void)App_NewPeriph_BeepPulseActive();

        /* USB HL 轮询 (当前中断驱动, 保留)*/
        App_Usb_Poll();

        /* 步进电机状态机推进 (步进 + 故障监测)*/
        App_Stepper_Process();

        /* 开锁器业务状态机推进 (硬标签EPC比对/软标解码编排)*/
        App_Locker_Process();

        /* 电机行程测试状态机推进 (仅 TEST 指令触发时占用电机)*/
        App_MotorTest_Process();

        /* UHF 状态机推进 (标签流采集 + 空闲结束检测)*/
        App_UHF_Process();

        /* AM 解码器状态机推进 (链路/心跳监测)*/
        App_AM_Process();

#if AM_LINE_TEST
        /* TEMP AM 线路测试: 每 500ms 一问, RGB 直接指示结果 */
        {
            static uint32_t s_amTestMs = 0u;
            uint32_t now = SysTickHl_GetMs();
            if (s_amTestMs == 0u || (now - s_amTestMs) >= 500u) {
                s_amTestMs = now;
                IwdgHl_Feed();
                int ok = (App_AM_Query() == 0);
                RgbLedHl_Set(ok ? RGB_BIT_G : RGB_BIT_R);
            }
        }
#endif

        /* 绿灯心跳 (500ms)*/
        AppLedProcess();

        /* 按键诊断闪烁 (行程开关低电平驱动 ERR, 独立于业务)*/
        AppLed_KeyBlinkProcess();

        /* RGB 灯带状态指示仲裁 (业务灯语/行程错误/闪显/手动回收) */
#if !AM_LINE_TEST
        App_RgbLedPat_Tick();
#endif
    }
}
