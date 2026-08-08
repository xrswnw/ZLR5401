#include "App_Dispatch.h"
#include "App_CustomProtocol.h"
#include "App_Led.h"
#include "App_SysTick_HL.h"
#include "App_Param.h"
#include "stm32f10x.h"
#include "App_Config.h"

DeviceParam_t g_sParam;

void AppDispatchInit(void) {
    Proto_Init();
    Proto_SetDeviceAddr(g_sParam.deviceAddr);
    Proto_RegisterFrameCb(AppDispatch);
    /* 系统仅走 USB HID, 不注册 UART 通道;
     * USB 通道由 App_Usb_Init -> Proto_RegisterTransport(PROTO_CH_USB,...) 注册*/
}

void AppDispatch(ProtoFrame_t *f) {
    uint8_t ch = f->channel;   /* 从哪收就从哪回*/

    switch (f->func) {
    case FC_HANDSHAKE: {
        /* 协议 (App/Boot 一致): 响应 payload (28B), 单包可发 (≤64B EP1 IN)
         * result(1) | ProtoVer(1) | Status(1)
         * | UID(12) | UidHash(4 LE) | Layer(1) | UpgradeCount(4 LE) | BaudRate(4 LE)
         * (移除冗余的 version 字段, ProtoVer 单字段表示协议版本)*/
        uint8_t rsp[1 + 1 + 1 + 12 + 4 + 1 + 4 + 4];
        uint16_t pos = 0;
        rsp[pos++] = RESULT_OK;
        rsp[pos++] = PROTO_VERSION;       /* ProtoVer*/
        rsp[pos++] = g_sParam.deviceStatus;
        const uint8_t *uid = (const uint8_t *)0x1FFFF7E8;
        for (int i = 0; i < 12; i++) rsp[pos++] = uid[i];
        rsp[pos++] = (uint8_t)(g_sParam.deviceUidHash);
        rsp[pos++] = (uint8_t)(g_sParam.deviceUidHash >> 8);
        rsp[pos++] = (uint8_t)(g_sParam.deviceUidHash >> 16);
        rsp[pos++] = (uint8_t)(g_sParam.deviceUidHash >> 24);
        rsp[pos++] = 1;  /* Layer=1=APP*/
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount);
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount >> 8);
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount >> 16);
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount >> 24);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate >> 8);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate >> 16);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate >> 24);
        Proto_TxResponse(ch, FC_HANDSHAKE, rsp, pos);
        break;
    }

    case FC_ENTER_BOOT: {
        /* 预升级指令. 置 UPG 标志 + 持久化 -> 回 OK -> 延时 50ms
         * (等 ACK 经 USB 1ms 帧周期完整推送 + 主机拉走) -> NVIC_SystemReset.
         * 复位后 Boot 读 deviceStatus==UPG -> stayInUpdate=true 自动进入升级等待. */
        uint8_t rsp[1];
        g_sParam.deviceStatus = PARAM_STATUS_UPG;
        if (ParamSave(&g_sParam) != 0) {
            rsp[0] = ENTER_BOOT_PARAM_ERR;
            Proto_TxResponse(ch, FC_ENTER_BOOT, rsp, 1);
            break;
        }
        rsp[0] = RESULT_OK;
        Proto_TxResponse(ch, FC_ENTER_BOOT, rsp, 1);
        SysTickHl_DelayMs(50);
        NVIC_SystemReset();
        break;
    }

    case FC_DEVICE_INFO: {
        /* 协议 (App 层): 响应 payload (34B), 单包可发 (≤64B EP1 IN)
         * result(1) | Addr(1) | hwVersion(16) | Version/swVersion(16)
         * Boot 层返回 bootVersion (字段名 Version, 内容为 boot 版本)*/
        uint8_t rsp[1 + 1 + VER_STR_LEN + VER_STR_LEN];
        Memset8(rsp, 0, sizeof(rsp));
        uint16_t pos = 0;
        rsp[pos++] = RESULT_OK;
        rsp[pos++] = g_sParam.deviceAddr;
        Memcpy(&rsp[pos], g_sParam.hwVersion, VER_STR_LEN); pos += VER_STR_LEN;
        Memcpy(&rsp[pos], g_sParam.swVersion, VER_STR_LEN); pos += VER_STR_LEN;
        Proto_TxResponse(ch, FC_DEVICE_INFO, rsp, pos);
        break;
    }

    case FC_RESET: {
        /* 空参数指令, 回 OK 后 NVIC_SystemReset
         * 时序: 先把 ACK 推入 USB PMA + 启动发送 (UserToPMABufferCopy +
         * SetEPTxValid, 在 App_Usb_HL_Transmit 内完成), 等 20ms 让 USB
         * 全速 1ms 帧周期完整推送 + 主机轮询拉走, 再复位 — 否则 ACK
         * 可能随 NVIC_SystemReset 一起丢, 主机端看不到响应。*/
        uint8_t rsp[1];
        if (f->dataLen != 0) {
            rsp[0] = 1;     /* param len must be 0*/
            Proto_TxResponse(ch, FC_RESET, rsp, 1);
            break;
        }
        rsp[0] = RESULT_OK;
        Proto_TxResponse(ch, FC_RESET, rsp, 1);
        SysTickHl_DelayMs(20);
        NVIC_SystemReset();
        break;
    }

    default:
        break;
    }
}
