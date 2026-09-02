#include "stm32f10x.h"
#include "Boot_Config.h"
#include "Boot_Led_HL.h"
#include "Boot_Led.h"
#include "Boot_SysTick_HL.h"
#include "Boot_Param.h"
#include "Boot_Param_HL.h"
#include "Boot_Usb.h"
#include "Boot_Usb_HL.h"   /* Boot_Usb_HL_DeInit (0: JumpToApp USB 软断开)*/
#include "Boot_Dispatch.h"
#include "Boot_Iwdg_HL.h"
#include "Boot_FaultStorm.h"
#include "System_PeriphClk.h"

static void JumpToApp(void) {
    typedef void (*pFnApp)(void);

    /* 1) MSP向量合法性*/
    if ((*(volatile uint32_t *)APP_FLASH_ORIGIN & 0x2FFC0000) != 0x20000000) {
        return;
    }

    /* 2) Reset向量合法性*/
    uint32_t resetVec = *(volatile uint32_t *)(APP_FLASH_ORIGIN + 4);
    if (resetVec < APP_FLASH_ORIGIN ||
        resetVec >= APP_FLASH_ORIGIN + APP_FLASH_SIZE) {
        return;
    }

    /* 3) APP CRC校验(如果参数区有记录)*/
    if (g_sParam.appCrc32 != 0 && g_sParam.appSize != 0 &&
        g_sParam.appSize <= APP_FLASH_SIZE) {
        uint32_t crc = Crc32Calc((const uint8_t *)APP_FLASH_ORIGIN,
                                  g_sParam.appSize);
        if (crc != g_sParam.appCrc32) {
            return;
        }
    }

    __asm volatile ("cpsid i");
    for (int i = 0; i < 8; i++)
        NVIC->ICER[i] = 0xFFFFFFFF;
    for (int i = 0; i < 8; i++)
        NVIC->ICPR[i] = 0xFFFFFFFF;

    /* 0: USB 软断开 — 关 USB 外设(PDWN+FRES)+关 USB 时钟+禁 USB IRQ,
     * 并拉低 USB_EN(PA8) 断开 D+ 上拉, 让主机看到物理断开.
     * Boot 残留的 USB 状态不带进 App, App 会重新拉高 PA8 重新枚举,
     * 避免过渡期总线错误. (此处 SysTick 已停, 不做延时, 靠电平跳变让主机检测.)*/
    Boot_Usb_HL_DeInit();
    GPIO_ResetBits(GPIOA, GPIO_Pin_8);

    __asm volatile ("dsb");
    SCB->VTOR = APP_FLASH_ORIGIN;
    pFnApp appEntry = (pFnApp)resetVec;
    __asm volatile ("msr msp, %0" : : "r"(*(volatile uint32_t *)APP_FLASH_ORIGIN) : );
    __asm volatile ("dsb; isb");
    BootStorm_Clear();     /* 跳转在即: App 启动会清零计数, 此处先行清 (双重保险) */
    appEntry();
}

void Sys_CfgClock(void)
{
    ErrorStatus HSEStartUpStatus = ERROR;

    RCC_DeInit();
    //Enable HSE
    RCC_HSEConfig(RCC_HSE_ON);

    //Wait till HSE is ready
    HSEStartUpStatus = RCC_WaitForHSEStartUp();

    if(HSEStartUpStatus == SUCCESS)
    {
        //HCLK = SYSCLK = 72M
        RCC_HCLKConfig(RCC_SYSCLK_Div1);

        //PCLK2 = HCLK = 72M
        RCC_PCLK2Config(RCC_HCLK_Div1);

        //PCLK1 = HCLK/2 = 36M
        RCC_PCLK1Config(RCC_HCLK_Div2);

        //ADCCLK = PCLK2/2
        RCC_ADCCLKConfig(RCC_PCLK2_Div2);

        // Select USBCLK source 72 / 1.5 = 48M
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);

        //Flash 2 wait state
        FLASH_SetLatency(FLASH_Latency_2);

        //Enable Prefetch Buffer
        FLASH_PrefetchBufferCmd(FLASH_PrefetchBuffer_Enable);

        //PLLCLK = 12MHz * 6 = 72 MHz
        RCC_PLLConfig(RCC_PLLSource_HSE_Div1, RCC_PLLMul_6);    //PLL‘⁄◊Ó∫Û…Ë÷√

        //Enable PLL
        RCC_PLLCmd(ENABLE);

        //Wait till PLL is ready
        while(RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET)
        {
        }

        //Select PLL as system clock source
        RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);

        //Wait till PLL is used as system clock source
        while(RCC_GetSYSCLKSource() != 0x08)
        {
        }
    }

    /* 同步软件变量 SystemCoreClock 到当前 SYSCLK 来源/分频,
     * 调用方 (main) 无须再手动调 SystemCoreClockUpdate().*/
    SystemCoreClockUpdate();
}

int main(void) {
    Sys_CfgClock();
    /* 外设 RCC 时钟统一使能 (替代各 HL 分散调用)*/
    System_PeriphClkInit();

    /* 关全局中断, 等所有外设初始化完成 + USB 拉高后再开
     * (避免初始化过程中断打断 USB 枚举)*/
    __asm volatile ("cpsid i");

    SysTickHl_Init();
    BootDispatchInit();
    uint32_t bootStartMs = SysTickHl_GetMs();

    BootLedInit();
    ParamInit(&g_sParam);
    if (ParamLoad(&g_sParam) != 0) {
        Memset8((uint8_t *)&g_sParam, 0, sizeof(g_sParam));
        g_sParam.magic = PARAM_MAGIC;
        g_sParam.version = PARAM_VERSION;
        g_sParam.baudRate = 115200U;
        g_sParam.deviceStatus = PARAM_STATUS_IDLE;
        g_sParam.deviceAddr = PROTO_DEV_ADDR_DEFAULT;
        g_sParam.deviceUidHash = Crc32CalcUid();
        g_sParam.bindVerify = 0;
        g_sParam.appCrc32 = 0;
        g_sParam.appSize = 0;
        g_sParam.upgradeCount = 0;
        VerStrCpy(g_sParam.hwVersion, DEV_HW_VERSION, VER_STR_LEN);
        VerStrCpy(g_sParam.swVersion, DEV_SW_VERSION, VER_STR_LEN);
        VerStrCpy(g_sParam.bootVersion, DEV_BOOT_VERSION, VER_STR_LEN);
        Memset8(g_sParam.userParam, 0, sizeof(g_sParam.userParam));
        if (ParamSave(&g_sParam) != 0) {
        }
    } else {
    }

    /* 版本同步*/
    ParamSyncVersion(&g_sParam);

    /* ParamLoad 之后必须重新同步设备地址到协议层。
     * BootDispatchInit()(line 127) 内调 Proto_SetDeviceAddr(g_sParam.deviceAddr),
     * 但它先于 ParamLoad, 此时 g_sParam.deviceAddr=0 (未加载) -> g_u8DevAddr 被设成 0.
     * 之后 ParamLoad 加载真实 deviceAddr(0x01) 却没再同步 -> 协议层地址过滤
     * (g_sProto.RxFrame.devAddr != g_u8DevAddr) 把所有 devAddr=0x01 的帧丢弃,
     * Boot 收帧但不回调 BootDispatch -> 不响应 (App 顺序正确: AppDispatchInit 在
     * ParamLoad 之后). 这里补一行修正, 与 App 对齐.*/
    Proto_SetDeviceAddr(g_sParam.deviceAddr);

    /* USB HID: 初始化 USB HL + 注册 USB 协议通道 (Boot 走 USB, 与 App 对齐).
     * 必须在开全局中断前完成, 避免初始化过程中断打断 USB 枚举*/
    Boot_Usb_Init();

    IwdgHl_Init();

    /* Round_098 优化 #7: Boot fault 复位风暴保护. 连续 >=3 次 fault
     * (期间无 App 成功启动 / 无升级提交) -> 强制常驻升级循环,
     * 主机可稳定 IAP 救援; 计数在 App 成功启动时被其 .bss 清零
     * 天然复位, 详见 Boot_FaultStorm.c. */
    int stayInUpdate = (g_sParam.deviceStatus == PARAM_STATUS_UPG);
    if (BootStorm_Check() != 0) stayInUpdate = 1;

    /* 所有初始化完成, 开全局中断*/
    __asm volatile ("cpsie i");

    while (stayInUpdate || (SysTickHl_GetMs() - bootStartMs) < BOOT_TIMEOUT_MS) {
        Proto_Poll();
        /* 升级一旦开始(ERASED/DATA_DONE/VERIFY_OK)即持续刷新超时窗口,
         * 直到 EXEC 提交或复位. 原实现 VERIFY_OK 不刷新 -> 2.5s 后超时跳 App,
         * 但此时 App 区已被新固件覆盖而参数区 appCrc32 仍是旧值 (EXEC 未执行),
         * JumpToApp 用旧 CRC 校验新 App 必失败 -> 回落升级, 形成不一致循环.
         * 改为 VERIFY_OK 也刷新 -> 升级中无限等主机发 EXEC, 闭环自洽.*/
        if (g_sIap.State != IAP_IDLE)
            bootStartMs = SysTickHl_GetMs();
        BootLedProcess();
        IwdgHl_Feed();
    }

    SysTickHl_Stop();
    BootLedOff();

    JumpToApp();

    /* JumpToApp returned — app flash is invalid, stay in update loop.
     * Round_098 BUG#5: SysTick 已在上行 Stop, 兜底循环若不重启, ms 计时冻结,
     * 任何协议响应的 WaitEp1Ready 自旋 (50ms 上限依赖 GetMs) 变成无限死等,
     * 设备永久哑死 (JLink 实测: tick 冻结在 2500, PC 卡 WaitEp1Ready).
     * App 无效时 JumpToApp 必然回落到这里, 必须恢复 SysTick. */
    SysTickHl_Init();
    stayInUpdate = 1;
    while (1) {
        Proto_Poll();
        BootLedProcess();
        IwdgHl_Feed();
    }
}
