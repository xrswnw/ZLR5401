#include "App_Dispatch.h"
#include "App_CustomProtocol.h"
#include "App_Led.h"
#include "App_SysTick_HL.h"
#include "App_Param.h"
#include "App_Stepper.h"
#include "App_UHF.h"
#include "App_AM.h"
#include "App_AM_HL.h"
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

    case FC_MOTOR_CTRL: {
        /* 步进电机控制. data[0]=cmd. 响应 payload: [cmd, err, ...]. */
        uint8_t rsp[8];
        uint8_t err = MOTOR_ERR_PARAM;
        uint16_t pos = 0; uint8_t cmd = 0;

        if (f->dataLen >= 1) cmd = f->data[0];

        switch (cmd) {
        case MOTOR_CMD_MOVE: {
            if (f->dataLen >= 5 && f->data[1] <= 1u) {
                AppStepperMove_t mv;
                mv.dir = f->data[1];
                mv.steps = (uint32_t)f->data[2] | ((uint32_t)f->data[3] << 8) | ((uint32_t)f->data[4] << 16);
                err = (App_Stepper_Move(&mv) == 0) ? MOTOR_ERR_OK : MOTOR_ERR_FAULT;
            }
            break;
        }
        case MOTOR_CMD_STOP:
            App_Stepper_Stop();
            err = MOTOR_ERR_OK;
            break;
        case MOTOR_CMD_SPEED:
            if (f->dataLen >= 3) {
                uint32_t hz = (uint32_t)f->data[1] | ((uint32_t)f->data[2] << 8);
                App_Stepper_SetSpeedHz(hz);
                err = MOTOR_ERR_OK;
            }
            break;
        case MOTOR_CMD_TORQUE:
            if (f->dataLen >= 2) {
                App_Stepper_SetTorquePercent(f->data[1]);
                err = MOTOR_ERR_OK;
            }
            break;
        case MOTOR_CMD_QUERY: {
            /* Query 响应: [cmd, err, state, fault, diag1, diag2, stepsDoneL, stepsDoneH] */
            uint8_t qrsp[8];
            qrsp[0] = cmd;
            qrsp[1] = MOTOR_ERR_OK;
            qrsp[2] = (uint8_t)App_Stepper_GetState();
            qrsp[3] = App_Stepper_GetFault();
            qrsp[4] = App_Stepper_GetDiag1();
            qrsp[5] = App_Stepper_GetDiag2();
            qrsp[6] = (uint8_t)(App_Stepper_GetStepsDone() & 0xFF);
            qrsp[7] = (uint8_t)((App_Stepper_GetStepsDone() >> 8) & 0xFF);
            Proto_TxResponse(ch, FC_MOTOR_CTRL, qrsp, sizeof(qrsp));
            break;
        }
        case MOTOR_CMD_CLEAR:
            App_Stepper_ClearFault();
            err = MOTOR_ERR_OK;
            break;
        default:
            break;
        }

        if (cmd != MOTOR_CMD_QUERY) {
            rsp[pos++] = cmd;
            rsp[pos++] = err;
            Proto_TxResponse(ch, FC_MOTOR_CTRL, rsp, pos);
        }
        break;
    }

    case FC_UHF_CTRL: {
        /* UHF 模块. data[0]=子命令. 响应 data[0]=cmd, data[1]=err, 其余随 cmd. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        switch (sub) {
        case UHF_SUB_OPEN: {
            uint8_t r[2] = { sub, APP_UHF_ERR_OK };
            int e = App_UHF_Open();
            if (e != APP_UHF_ERR_OK) { r[1] = UHF_ERR_LINK; r[0] = sub; }
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_CLOSE: {
            App_UHF_Close();
            uint8_t r[2] = { sub, UHF_ERR_OK };
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_INVENTORY: {
            if (App_UHF_IsBusy()) {
                uint8_t r[2] = { sub, UHF_ERR_BUSY };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            int e = App_UHF_Inventory();
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_BUSY) r[1] = UHF_ERR_BUSY;
            else if (e == APP_UHF_ERR_LINK) r[1] = UHF_ERR_LINK;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_READ_TAG: {
            /* [sub, epcLen, epc.., bank, addr, cnt] */
            if (f->dataLen < 6u) {
                uint8_t r[2] = { sub, UHF_ERR_PARAM };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            uint8_t epcLen = f->data[1];
            if ((uint16_t)2u + epcLen + 3u > f->dataLen) {
                uint8_t r[2] = { sub, UHF_ERR_PARAM };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            if (App_UHF_IsBusy()) {
                uint8_t r[2] = { sub, UHF_ERR_BUSY };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            int e = App_UHF_ReadTag(&f->data[2], epcLen, f->data[2 + epcLen],
                                    f->data[2 + epcLen + 1], f->data[2 + epcLen + 2]);
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_BUSY) r[1] = UHF_ERR_BUSY;
            else if (e == APP_UHF_ERR_LINK) r[1] = UHF_ERR_LINK;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_WRITE_TAG: {
            /* [sub, epcLen, epc.., bank, addr, len, data..] */
            if (f->dataLen < 7u) {
                uint8_t r[2] = { sub, UHF_ERR_PARAM };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            uint8_t epcLen = f->data[1];
            uint8_t dlen = f->data[2 + epcLen + 2];
            if ((uint16_t)2u + epcLen + 3u + dlen > f->dataLen) {
                uint8_t r[2] = { sub, UHF_ERR_PARAM };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            if (App_UHF_IsBusy()) {
                uint8_t r[2] = { sub, UHF_ERR_BUSY };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            int e = App_UHF_WriteTag(&f->data[2], epcLen, f->data[2 + epcLen],
                                     f->data[2 + epcLen + 1],
                                     &f->data[2 + epcLen + 3], dlen);
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_BUSY) r[1] = UHF_ERR_BUSY;
            else if (e == APP_UHF_ERR_LINK) r[1] = UHF_ERR_LINK;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_STOP:
            App_UHF_Stop();
            {
                uint8_t r[2] = { sub, UHF_ERR_OK };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            }
            break;
        case UHF_SUB_QUERY: {
            int e = App_UHF_Query();
            uint8_t r[1 + 1 + 3] = { sub, UHF_ERR_OK,
                (uint8_t)App_UHF_GetState(),
                (uint8_t)App_UHF_GetLinkStatus(),
                (uint8_t)(App_UHF_GetTotalTags() > 255u ? 255u : App_UHF_GetTotalTags()) };
            if (e != APP_UHF_ERR_OK) r[1] = UHF_ERR_LINK;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, sizeof(r));
            break;
        }
        case UHF_SUB_GET_CONFIG: {
            AppUHFConfig_t c;
            App_UHF_GetConfig(&c);
            uint8_t r[1 + 1 + 6] = { sub, UHF_ERR_OK, c.powerDbm, c.antenna,
                c.checksumEn, c.session, c.target, c.q };
            Proto_TxResponse(ch, FC_UHF_CTRL, r, sizeof(r));
            break;
        }
        case UHF_SUB_SET_CONFIG: {
            /* [sub, powerDbm, antenna, checksumEn, session, target, q] */
            if (f->dataLen < 7u) {
                uint8_t r[2] = { sub, UHF_ERR_PARAM };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            AppUHFConfig_t c;
            c.powerDbm   = f->data[1];
            c.antenna    = f->data[2];
            c.checksumEn = f->data[3];
            c.session    = f->data[4];
            c.target     = f->data[5];
            c.q          = f->data[6];
            int e = App_UHF_SetConfig(&c, 0);
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_PARAM) r[1] = UHF_ERR_PARAM;
            else if (e == APP_UHF_ERR_LINK) r[1] = UHF_ERR_LINK;
            else {
                /* 持久化 */
                UHFUserCfg_t pc = { c.powerDbm, c.antenna, c.checksumEn,
                                    c.session, c.target, c.q };
                (void)UhfParam_Save(&pc);
            }
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_GET_TAGS: {
            /* [sub, count]  count=0 全部 */
            uint16_t want = (f->dataLen >= 2u) ? f->data[1] : 0u;
            uint16_t total = App_UHF_TagCount();
            if (want != 0u && want < total) total = want;
            uint16_t maxData = PROTO_MAX_DATA;
            uint8_t r[2 + total * 18];
            uint16_t pos = 0;
            r[pos++] = sub;
            r[pos++] = UHF_ERR_OK;
            r[pos++] = (uint8_t)(total & 0xFF);
            r[pos++] = (uint8_t)(total >> 8);
            AppUHFTag_t t;
            while (total > 0u && App_UHF_TagTake(&t) == 0) {
                uint16_t need = 2u + t.epcLen;
                if (pos + need > maxData) break;
                r[pos++] = t.epcLen;
                r[pos++] = (uint8_t)(t.rssi > 255u ? 255u : t.rssi);
                for (uint8_t i = 0; i < t.epcLen; i++) r[pos++] = t.epc[i];
                total--;
            }
            Proto_TxResponse(ch, FC_UHF_CTRL, r, pos);
            break;
        }
        default:
            {
                uint8_t r[2] = { sub, UHF_ERR_PARAM };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            }
            break;
        }
        break;
    }

    case FC_AM_CTRL: {
        /* AM 解码器. data[0]=子命令. 响应 data[0]=cmd, data[1]=err, 其余随 cmd. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        switch (sub) {
        case AM_SUB_GET_CONFIG: {
            AppAMConfig_t c;
            App_AM_GetConfig(&c);
            uint8_t r[2 + 2 + 2 + 1 + 2 + 1 + 1 + 2 + 1 + 1] = {
                sub, AM_ERR_OK,
                (uint8_t)((c.threshold >> 8) & 0xFF), (uint8_t)(c.threshold & 0xFF),
                (uint8_t)((c.hitCount >> 8) & 0xFF), (uint8_t)(c.hitCount & 0xFF),
                c.freqRange,
                (uint8_t)((c.recvDelay >> 8) & 0xFF), (uint8_t)(c.recvDelay & 0xFF),
                c.recvLength,
                c.phaseInvert,
                (uint8_t)((c.phaseSync >> 8) & 0xFF), (uint8_t)(c.phaseSync & 0xFF),
                c.decodeVolt,
                c.mode
            };
            Proto_TxResponse(ch, FC_AM_CTRL, r, sizeof(r));
            break;
        }
        case AM_SUB_SET_CONFIG: {
            /* [sub, thrH,thrL, hitH,hitL, freq, delayH,delayL, len, invert, syncH,syncL, volt, mode] */
            if (f->dataLen < 15u) {
                uint8_t r[2] = { sub, AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
                break;
            }
            AppAMConfig_t c;
            c.threshold   = (uint16_t)(((uint16_t)f->data[1] << 8) | f->data[2]);
            c.hitCount    = (uint16_t)(((uint16_t)f->data[3] << 8) | f->data[4]);
            c.freqRange   = f->data[5];
            c.recvDelay   = (uint16_t)(((uint16_t)f->data[6] << 8) | f->data[7]);
            c.recvLength  = f->data[8];
            c.phaseInvert = f->data[9];
            c.phaseSync   = (uint16_t)(((uint16_t)f->data[10] << 8) | f->data[11]);
            c.decodeVolt  = f->data[12];
            c.mode        = f->data[13];

            int e = App_AM_SetConfig(&c, 0);
            uint8_t r[2] = { sub, AM_ERR_OK };
            if (e == APP_AM_ERR_PARAM) r[1] = AM_ERR_PARAM;
            else if (e == APP_AM_ERR_LINK) r[1] = AM_ERR_LINK;
            else {
                AMUserCfg_t pc = { c.threshold, c.hitCount, c.freqRange,
                                   c.recvDelay, c.recvLength, c.phaseInvert,
                                   c.phaseSync, c.decodeVolt, c.mode };
                (void)AmParam_Save(&pc);
            }
            Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
            break;
        }
        case AM_SUB_GET_PARAM: {
            /* [sub, amCmd] */
            if (f->dataLen < 2u) {
                uint8_t r[2] = { sub, AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
                break;
            }
            uint16_t val = 0;
            int e = App_AM_GetParam(f->data[1], &val);
            uint8_t r[1 + 1 + 2 + 1] = { sub, AM_ERR_OK,
                (uint8_t)(f->data[1]),
                (uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF) };
            if (e == APP_AM_ERR_LINK) r[1] = AM_ERR_LINK;
            else if (e == APP_AM_ERR_TIMEOUT) r[1] = AM_ERR_TIMEOUT;
            Proto_TxResponse(ch, FC_AM_CTRL, r, sizeof(r));
            break;
        }
        case AM_SUB_SET_PARAM: {
            /* [sub, amCmd, valH, valL] */
            if (f->dataLen < 4u) {
                uint8_t r[2] = { sub, AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
                break;
            }
            uint16_t val = (uint16_t)(((uint16_t)f->data[2] << 8) | f->data[3]);
            int e = App_AM_SetParam(f->data[1], val);
            uint8_t r[2] = { sub, AM_ERR_OK };
            if (e == APP_AM_ERR_LINK) r[1] = AM_ERR_LINK;
            else if (e == APP_AM_ERR_PARAM) r[1] = AM_ERR_PARAM;
            Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
            break;
        }
        case AM_SUB_QUERY: {
            int e = App_AM_Query();
            uint8_t r[1 + 1 + 1] = { sub, AM_ERR_OK,
                (uint8_t)App_AM_GetLinkStatus() };
            if (e != APP_AM_ERR_OK) r[1] = AM_ERR_LINK;
            Proto_TxResponse(ch, FC_AM_CTRL, r, sizeof(r));
            break;
        }
        default:
            {
                uint8_t r[2] = { sub, AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
            }
            break;
        }
        break;
    }

    default:
        break;
    }
}
