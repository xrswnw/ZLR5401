#include "stm32f10x.h"
#include "App_Config.h"
#include "App_Param.h"
#include "App_SysTick_HL.h"
#include "App_Iwdg_HL.h"   /* IwdgHl_Init/Feed (Boot 启动 IWDG 持续到 App)*/
#include "App_Led_HL.h"
#include "App_Led.h"
#include "App_Usb.h"
#include "App_Usb_HL.h"
#include "App_Sys_CfgClock.h"
#include "System_PeriphClk.h"
#include "App_Dispatch.h"
#include "App_CustomProtocol.h"

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

    /* 7. USB HID (USB_EN=PA1 使能, AF_PP 配置 PA11/12 + GPIO_SetBits USB_EN)*/
    App_Usb_Init();

    /* 8.4 参数区加载: 主区/影子区 CRC 失败时回落到默认值并写回 flash*/
    if (ParamLoad(&g_sParam) != 0) {
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

    /* 9. 开全局中断 (最后一步 Sys_EnableInt, USB 准备就绪后才开)*/
    __asm volatile ("cpsie i");
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

        /* USB HL 轮询 (当前中断驱动, 保留)*/
        App_Usb_Poll();

        /* 绿灯心跳 (500ms)*/
        AppLedProcess();
    }
}
