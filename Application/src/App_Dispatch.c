#include "App_Dispatch.h"
#include "App_CustomProtocol.h"
#include "App_Led.h"
#include "App_SysTick_HL.h"
#include "App_Param.h"
#include "App_Stepper.h"
#include "App_MotorTest.h"
#include "App_MotorHoming.h"
#include "App_UHF.h"
#include "App_AM.h"
#include "App_AM_HL.h"
#include "App_Locker.h"
#include "App_LockerOneShot.h"
#include "App_RgbLed_Pattern.h"
#include "App_BootSelfTest.h"
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
        /* 步进电机控制. data[0]=cmd. 响应 payload: [cmd, err, ...].
         * 0x08 流程进行中: 仅放行只读 (QUERY/HEALTH/STATS), 控制类回 BUSY. */
        uint8_t rsp[8];
        uint8_t err = MOTOR_ERR_PARAM;
        uint16_t pos = 0; uint8_t cmd = 0;

        if (f->dataLen >= 1) cmd = f->data[0];

        if (App_LockerOneShot_IsBusy() &&
            cmd != MOTOR_CMD_QUERY && cmd != MOTOR_CMD_HEALTH && cmd != MOTOR_CMD_STATS) {
            uint8_t r[2] = { cmd, MOTOR_ERR_BUSY };
            Proto_TxResponse(ch, FC_MOTOR_CTRL, r, 2);
            break;
        }

        switch (cmd) {
        case MOTOR_CMD_MOVE: {
            if (f->dataLen >= 5 && f->data[1] <= 1u) {
                /* MOVE 不受行程自检门控: 电机完全开放给上位机控制;
                 * 开关错误位(switchErr)仅作报警/诊断, 由上位机自行决策. */
                AppStepperMove_t mv;
                mv.dir = f->data[1];
                mv.steps = (uint32_t)f->data[2] | ((uint32_t)f->data[3] << 8) | ((uint32_t)f->data[4] << 16);
                err = (App_Stepper_Move(&mv) == 0) ? MOTOR_ERR_OK : MOTOR_ERR_FAULT;
            }
            break;
        }
        case MOTOR_CMD_STOP:
            App_Stepper_Stop();
            App_MotorTest_Stop();   /* 一并复位行程测试态, 便于上位机从测试时/FAULT 恢复 */
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
            /* Query 响应: [cmd, err, state, fault, mtReason, diag2, stepsDoneL, stepsDoneH, switchErr, testState]
             * switchErr bit0=上行程错, bit1=下行程错 (回零/运行中开关监控置位)
             * state 歧义消解: testState(末字节)!=0 时 state 为行程测试态
             * (1=RUN 2=DONE 3=测试FAULT), 否则为步进态 (2=步进FAULT)。 */
            uint8_t qrsp[10];
            qrsp[0] = cmd;
            qrsp[1] = MOTOR_ERR_OK;
            /* 行程测试进行中/结束后上报测试态, 让上位机看到 DONE(2)/FAULT(3);
             * 测试态=IDLE 时回退到步进状态. */
            qrsp[2] = (App_MotorTest_GetState() != MT_STATE_IDLE)
                      ? (uint8_t)App_MotorTest_GetState()
                      : (uint8_t)App_Stepper_GetState();
            qrsp[3] = App_Stepper_GetFault();
            qrsp[4] = App_MotorTest_GetFaultReason();   /* 测试故障原因; 非测试时为 0 */
            qrsp[5] = App_Stepper_GetDiag2();
            qrsp[6] = (uint8_t)(App_Stepper_GetStepsDone() & 0xFF);
            qrsp[7] = (uint8_t)((App_Stepper_GetStepsDone() >> 8) & 0xFF);
            qrsp[8] = App_Stepper_GetSwitchErr();       /* 行程开关错误位 */
            qrsp[9] = (uint8_t)App_MotorTest_GetState(); /* 行程测试态 (0=非测试, 消歧义) */
            Proto_TxResponse(ch, FC_MOTOR_CTRL, qrsp, sizeof(qrsp));
            break;
        }
        case MOTOR_CMD_TEST: {
            /* [cmd, passes]  passes=往返次数; 进行中再下发回 BUSY */
            if (f->dataLen < 2u) {
                err = MOTOR_ERR_PARAM;
                break;
            }
            if (App_MotorHoming_IsReady() == 0) {   /* 行程基准未建立 -> 禁止 TEST */
                err = MOTOR_ERR_FAULT;
                break;
            }
            int e = App_MotorTest_Start(f->data[1]);
            if (e == 0)       err = MOTOR_ERR_OK;
            else if (e == -1) err = MOTOR_ERR_BUSY;
            else if (e == -2) err = MOTOR_ERR_PARAM;
            else              err = MOTOR_ERR_FAULT;
            break;
        }
        case MOTOR_CMD_CLEAR:
            App_Stepper_ClearFault();
            /* 一并复位行程测试态: 若处于 TEST FAULT(3), 仅清步进故障不够,
             * 需终止测试态才能真正恢复到 IDLE(0) 供 QUERY 上报. */
            App_MotorTest_Stop();
            err = MOTOR_ERR_OK;
            break;
        case MOTOR_CMD_HEALTH: {
            /* 健康/堵转监测: [cmd,err,olovState,threshL,threshH,trqL,trqH,reason] */
            uint16_t th = App_Stepper_GetOlovThreshold();
            uint16_t tq = App_Stepper_GetTorqueCount();
            uint8_t hrsp[8];
            hrsp[0] = cmd;
            hrsp[1] = MOTOR_ERR_OK;
            hrsp[2] = (uint8_t)App_Stepper_GetOlovState();
            hrsp[3] = (uint8_t)(th & 0xFF);
            hrsp[4] = (uint8_t)((th >> 8) & 0xFF);
            hrsp[5] = (uint8_t)(tq & 0xFF);
            hrsp[6] = (uint8_t)((tq >> 8) & 0xFF);
            hrsp[7] = App_Stepper_GetStats().lastReason;
            Proto_TxResponse(ch, FC_MOTOR_CTRL, hrsp, sizeof(hrsp));
            break;
        }
        case MOTOR_CMD_STATS: {
            /* 运行统计: [cmd,err,runSec(3),startCnt(2),lastReason] */
            AppStepperStats_t st = App_Stepper_GetStats();
            uint8_t srsp[8];
            srsp[0] = cmd;
            srsp[1] = MOTOR_ERR_OK;
            srsp[2] = (uint8_t)(st.runSeconds & 0xFF);
            srsp[3] = (uint8_t)((st.runSeconds >> 8) & 0xFF);
            srsp[4] = (uint8_t)((st.runSeconds >> 16) & 0xFF);
            srsp[5] = (uint8_t)(st.startCount & 0xFF);
            srsp[6] = (uint8_t)((st.startCount >> 8) & 0xFF);
            srsp[7] = st.lastReason;
            Proto_TxResponse(ch, FC_MOTOR_CTRL, srsp, sizeof(srsp));
            break;
        }
        default:
            break;
        }

        if (cmd != MOTOR_CMD_QUERY && cmd != MOTOR_CMD_HEALTH && cmd != MOTOR_CMD_STATS) {
            rsp[pos++] = cmd;
            rsp[pos++] = err;
            Proto_TxResponse(ch, FC_MOTOR_CTRL, rsp, pos);
        }
        break;
    }

    case FC_UHF_CTRL: {
        /* UHF 模块. data[0]=子命令. 响应 data[0]=cmd, data[1]=err, 其余随 cmd.
         * 盘点为同步阻塞: UHF_SUB_INVENTORY 返回时标签结果已随响应一并带回.
         * 0x08 流程进行中: 仅放行只读 (QUERY/GET_STATUS), 其余回 BUSY. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        if (App_LockerOneShot_IsBusy() &&
            sub != UHF_SUB_QUERY && sub != UHF_SUB_GET_STATUS) {
            uint8_t r[2] = { sub, UHF_ERR_BUSY };
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }

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
            /* [sub, timeoutL, timeoutH]  (timeout 可选, 缺省 1000ms)
             * 同步阻塞盘点: 全程 0x22->0x29 完成后, 把标签结果直接放进响应返回.
             * 响应: [sub,err,count(2 LE), {rssi,epcLen,epc..} x count] */
            uint16_t timeout = (f->dataLen >= 3u)
                               ? (uint16_t)(f->data[1] | ((uint16_t)f->data[2] << 8)) : 1000u;
            if (App_UHF_IsBusy()) {
                uint8_t r[2] = { sub, UHF_ERR_BUSY };
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
                break;
            }
            int n = App_UHF_InventorySync(timeout);
            uint8_t r[2 + 2 + 8 * (3 + 16)];
            uint16_t pos = 0;
            r[pos++] = sub;
            r[pos++] = UHF_ERR_OK;
            if (n < 0) {
                r[1] = UHF_ERR_LINK;
                if (n == APP_UHF_ERR_BUSY)       r[1] = UHF_ERR_BUSY;
                else if (n == APP_UHF_ERR_PARAM)  r[1] = UHF_ERR_PARAM;
                else if (n == APP_UHF_ERR_NOT_READY) r[1] = UHF_ERR_NOT_READY;
                else if (n == APP_UHF_ERR_NO_TAG)    r[1] = UHF_ERR_NO_TAG;
                /* 发送头两字节 */
                r[2] = 0; r[3] = 0;
                Proto_TxResponse(ch, FC_UHF_CTRL, r, 4);
                break;
            }
            uint16_t count = (uint16_t)n;
            r[pos++] = (uint8_t)(count & 0xFF);
            r[pos++] = (uint8_t)(count >> 8);
            AppUHFTag_t t;
            uint16_t sent = count;
            while (sent > 0u && App_UHF_TagTake(&t) == 0) {
                uint16_t need = 2u + t.epcLen;
                if (pos + need > sizeof(r)) break;
                r[pos++] = (uint8_t)(t.rssi > 255u ? 255u : t.rssi);
                r[pos++] = t.epcLen;
                for (uint8_t i = 0; i < t.epcLen; i++) r[pos++] = t.epc[i];
                sent--;
            }
            Proto_TxResponse(ch, FC_UHF_CTRL, r, pos);
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
            uint8_t r[1 + 1 + 7] = { sub, UHF_ERR_OK, c.powerDbm, c.antenna,
                c.checksumEn, c.session, c.target, c.q, c.band };
            Proto_TxResponse(ch, FC_UHF_CTRL, r, sizeof(r));
            break;
        }
        case UHF_SUB_SET_CONFIG: {
            /* [sub, powerDbm, antenna, checksumEn, session, target, q, (band)] */
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
            c.band       = (f->dataLen >= 8u) ? f->data[7] : 0x01u;
            int e = App_UHF_SetConfig(&c, 0);
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_PARAM) r[1] = UHF_ERR_PARAM;
            else if (e == APP_UHF_ERR_LINK) r[1] = UHF_ERR_LINK;
            else {
                /* 持久化 */
                UHFUserCfg_t pc = { c.powerDbm, c.antenna, c.checksumEn,
                                    c.session, c.target, c.q, c.band };
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
        case UHF_SUB_GET_STATUS: {
            /* [cmd, err, state, link, totalTags(低8), powered, antennaOk,
               lastErr(低8), antRl(2 高在前), antVswr(2 高在前)] */
            AppUHFStatus_t st;
            App_UHF_GetStatus(&st);
            uint8_t r[12] = {
                sub, UHF_ERR_OK,
                (uint8_t)App_UHF_GetState(),
                (uint8_t)App_UHF_GetLinkStatus(),
                (uint8_t)(App_UHF_GetTotalTags() > 255u ? 255u : App_UHF_GetTotalTags()),
                st.powered, st.antennaOk,
                (uint8_t)(st.lastErr & 0xFF),
                (uint8_t)(st.antRl >> 8), (uint8_t)(st.antRl & 0xFF),
                (uint8_t)(st.antVswr >> 8), (uint8_t)(st.antVswr & 0xFF)
            };
            Proto_TxResponse(ch, FC_UHF_CTRL, r, sizeof(r));
            break;
        }
        case UHF_SUB_CHECK_ANT: {
            int e = App_UHF_CheckAntenna();
            AppUHFStatus_t st;
            App_UHF_GetStatus(&st);
            uint8_t r[1 + 1 + 1 + 2 + 2] = {
                sub, UHF_ERR_OK, st.antennaOk,
                (uint8_t)(st.antRl >> 8), (uint8_t)(st.antRl & 0xFF),
                (uint8_t)(st.antVswr >> 8), (uint8_t)(st.antVswr & 0xFF)
            };
            if (e == APP_UHF_ERR_NOT_READY) r[1] = UHF_ERR_NOT_READY;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, sizeof(r));
            break;
        }
        case UHF_SUB_SCAN_START: {
            /* [sub, cycleL, cycleH]  启动自动扫描.
             * 响应 [sub, err]: 0=已启动/已在扫; busy=其它操作抢先; not_ready=未上电.
             * 扫描为连续盘点, 标签入缓冲(带RSSI), 主机用 GET_TAGS 拉取取走, 不主动上报. */
            uint16_t cycle = (f->dataLen >= 3u)
                             ? (uint16_t)(f->data[1] | ((uint16_t)f->data[2] << 8)) : 1000u;
            int e = App_UHF_ScanStart(cycle);
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_BUSY)      r[1] = UHF_ERR_BUSY;
            else if (e == APP_UHF_ERR_NOT_READY) r[1] = UHF_ERR_NOT_READY;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_SCAN_STOP: {
            /* [sub]  停止自动扫描 */
            int e = App_UHF_ScanStop();
            uint8_t r[2] = { sub, UHF_ERR_OK };
            if (e == APP_UHF_ERR_BUSY) r[1] = UHF_ERR_BUSY;
            Proto_TxResponse(ch, FC_UHF_CTRL, r, 2);
            break;
        }
        case UHF_SUB_GET_DUMP: {
            /* [cmd,err,cnt,reqCmd,reqLen,reqData..,rawDataLenL,rawDataLenH,rspCmd,
               rspStatusL,rspStatusH,rspLenL,rspLenH,rspData..] */
            AppUHFDump_t dg;
            App_UHF_GetDump(&dg);
            uint8_t r[2 + 1 + 1 + 1 + 5 + 2 + 1 + 2 + 2 + 40];
            uint16_t pos = 0;
            r[pos++] = sub; r[pos++] = UHF_ERR_OK;
            r[pos++] = dg.cnt;
            r[pos++] = dg.reqCmd; r[pos++] = dg.reqLen;
            for (uint8_t i = 0; i < dg.reqLen; i++) r[pos++] = dg.reqData[i];
            r[pos++] = (uint8_t)(dg.rawDataLen & 0xFF);
            r[pos++] = (uint8_t)((dg.rawDataLen >> 8) & 0xFF);
            r[pos++] = dg.rspCmd;
            r[pos++] = (uint8_t)(dg.rspStatus & 0xFF);
            r[pos++] = (uint8_t)((dg.rspStatus >> 8) & 0xFF);
            r[pos++] = (uint8_t)(dg.rspLen & 0xFF);
            r[pos++] = (uint8_t)((dg.rspLen >> 8) & 0xFF);
            for (uint8_t i = 0; i < dg.rspLen && pos < sizeof(r); i++) r[pos++] = dg.rspData[i];
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
        /* AM 解码器. data[0]=子命令. 响应 data[0]=cmd, data[1]=err, 其余随 cmd.
         * 0x08 流程进行中: 仅放行 AM_SUB_GET_STATUS (本地读, 无总线动作;
         * QUERY 等会 RxFlush 冲掉流程正在等的 cmd17), 其余回 BUSY. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        if (App_LockerOneShot_IsBusy() && sub != AM_SUB_GET_STATUS) {
            uint8_t r[2] = { sub, AM_ERR_BUSY };
            Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
            break;
        }

        switch (sub) {
        case AM_SUB_GET_CONFIG: {
            AppAMConfig_t c;
            App_AM_GetConfig(&c);
            uint8_t r[2 + 2 + 2 + 1 + 2 + 1 + 1 + 2 + 1 + 1 + 1] = {
                sub, AM_ERR_OK,
                (uint8_t)((c.threshold >> 8) & 0xFF), (uint8_t)(c.threshold & 0xFF),
                (uint8_t)((c.hitCount >> 8) & 0xFF), (uint8_t)(c.hitCount & 0xFF),
                c.freqRange,
                (uint8_t)((c.recvDelay >> 8) & 0xFF), (uint8_t)(c.recvDelay & 0xFF),
                c.recvLength,
                c.phaseInvert,
                (uint8_t)((c.phaseSync >> 8) & 0xFF), (uint8_t)(c.phaseSync & 0xFF),
                c.decodeVolt,
                c.mode,
                c.mainsFreq
            };
            Proto_TxResponse(ch, FC_AM_CTRL, r, sizeof(r));
            break;
        }
        case AM_SUB_SET_CONFIG: {
            /* [sub, thrH,thrL, hitH,hitL, freq, delayH,delayL, len, invert, syncH,syncL, volt, mode, (mains)] */
            if (f->dataLen < 14u) {
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
            /* 版本兼容: dataLen>=15 才含 mains (新主机); 否则保持当前值不覆盖 */
            AppAMConfig_t cur;
            App_AM_GetConfig(&cur);
            c.mainsFreq   = (f->dataLen >= 15u) ? f->data[14] : cur.mainsFreq;

            int e = App_AM_SetConfig(&c, 0);
            uint8_t r[2] = { sub, AM_ERR_OK };
            if (e == APP_AM_ERR_PARAM) r[1] = AM_ERR_PARAM;
            else if (e == APP_AM_ERR_LINK) r[1] = AM_ERR_LINK;
            else {
                AMUserCfg_t pc = { c.threshold, c.hitCount, c.freqRange,
                                   c.recvDelay, c.recvLength, c.phaseInvert,
                                   c.phaseSync, c.decodeVolt, c.mode, c.mainsFreq };
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
        case AM_SUB_GET_STATUS: {
            /* [cmd, link, evt(4), last(4), deact, deactCnt(4), failCnt(4)]  无 err 字段
               deact: 0=空闲 1=消磁成功 2=消磁失败 (cmd17 突发结算: 1帧=成功/≥2帧=失败)
               deactCnt/failCnt: 成功/失败消磁事件累计 (LE 32) */
            uint32_t evt = App_AM_GetEventCount();
            uint32_t last = App_AM_GetLastEventMs();
            uint32_t dok  = App_AM_GetDeactCount();
            uint32_t dfail = App_AM_GetFailCount();
            uint8_t r[1 + 1 + 4 + 4 + 1 + 4 + 4] = {
                sub,
                (uint8_t)App_AM_GetLinkStatus(),
                (uint8_t)(evt & 0xFF),      (uint8_t)((evt >> 8) & 0xFF),
                (uint8_t)((evt >> 16) & 0xFF), (uint8_t)((evt >> 24) & 0xFF),
                (uint8_t)(last & 0xFF),     (uint8_t)((last >> 8) & 0xFF),
                (uint8_t)((last >> 16) & 0xFF), (uint8_t)((last >> 24) & 0xFF),
                (uint8_t)App_AM_GetDeactState(),
                (uint8_t)(dok & 0xFF),      (uint8_t)((dok >> 8) & 0xFF),
                (uint8_t)((dok >> 16) & 0xFF), (uint8_t)((dok >> 24) & 0xFF),
                (uint8_t)(dfail & 0xFF),    (uint8_t)((dfail >> 8) & 0xFF),
                (uint8_t)((dfail >> 16) & 0xFF), (uint8_t)((dfail >> 24) & 0xFF)
            };
            Proto_TxResponse(ch, FC_AM_CTRL, r, sizeof(r));
            break;
        }
        case AM_SUB_SET_MODE: {
            /* [cmd, mode]  仅切工作模式 + 持久化 */
            if (f->dataLen < 2u || f->data[1] > 2u) {
                uint8_t r[2] = { sub, AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
                break;
            }
            int e = App_AM_SetParam(AM_CMD_MODE, f->data[1]);
            uint8_t r[2] = { sub, AM_ERR_OK };
            if (e == APP_AM_ERR_LINK) { r[1] = AM_ERR_LINK; Proto_TxResponse(ch, FC_AM_CTRL, r, 2); break; }
            /* 同步持久化: 读当前缓存改 mode 后存回 */
            AppAMConfig_t c;
            App_AM_GetConfig(&c);
            c.mode = f->data[1];
            AMUserCfg_t pc = { c.threshold, c.hitCount, c.freqRange,
                               c.recvDelay, c.recvLength, c.phaseInvert,
                               c.phaseSync, c.decodeVolt, c.mode, c.mainsFreq };
            (void)AmParam_Save(&pc);
            Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
            break;
        }
        case AM_SUB_GET_WAVE: {
            /* [cmd]  触发一次同步波形采集 (0x64, 阻塞~1s): 回 [cmd,err,pointsH,pointsL] */
            int e = App_AM_CaptureWave();
            uint8_t r[5] = { sub, AM_ERR_OK, 0u, 0u, 0u };
            if (e == APP_AM_ERR_LINK) { r[1] = AM_ERR_LINK; Proto_TxResponse(ch, FC_AM_CTRL, r, 2); break; }
            uint16_t pts = App_AM_GetWavePoints();
            r[2] = (uint8_t)((pts >> 8) & 0xFF);
            r[3] = (uint8_t)(pts & 0xFF);
            Proto_TxResponse(ch, FC_AM_CTRL, r, 5);
            break;
        }
        case AM_SUB_GET_WAVE_PAGE: {
            /* [cmd, page]  取一页波形 (48 点/页): 回 [cmd,page,err,pointsH,pointsL,w(≤48)] */
            if (f->dataLen < 2u) {
                uint8_t r[2] = { sub, AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 2);
                break;
            }
            uint16_t page = f->data[1];
            uint8_t w[AM_WAVE_PAGE];
            uint16_t n = 0u;
            int e = App_AM_GetWavePage(page, w, &n);
            if (e != APP_AM_ERR_OK) {
                uint8_t r[3] = { sub, (uint8_t)(page & 0xFF), AM_ERR_PARAM };
                Proto_TxResponse(ch, FC_AM_CTRL, r, 3);
                break;
            }
            uint16_t wcount = App_AM_GetWavePoints();
            uint8_t r[52];
            r[0] = sub;
            r[1] = (uint8_t)(page & 0xFF);
            r[2] = AM_ERR_OK;
            r[3] = (uint8_t)((wcount >> 8) & 0xFF);
            r[4] = (uint8_t)(wcount & 0xFF);
            for (uint16_t i = 0; i < n; i++) r[5 + i] = w[i];
            Proto_TxResponse(ch, FC_AM_CTRL, r, (uint16_t)(5u + n));
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

    case FC_LOCKER_CTRL: {
        /* 开锁器业务编排. data[0]=子命令. 响应 data[0]=cmd, data[1]=err, 其余随 cmd.
         * 0x08 同步流程进行中: 仅放行 QUERY/GET_PROGRESS/CANCEL, 其余回 BUSY. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        if (App_LockerOneShot_IsBusy() &&
            sub != LOCKER_SUB_QUERY && sub != LOCKER_SUB_GET_PROGRESS &&
            sub != LOCKER_SUB_CANCEL) {
            uint8_t r[2] = { sub, LOCKER_ERR_BUSY };
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            break;
        }

        switch (sub) {
        case LOCKER_SUB_CONFIGURE: {
            /* [cmd, hardCountL, hardCountH, softCountL, softCountH]
             * v1 精简: 清空清单, 仅设软标数 (hardCount 由随后 ADD 累积).
             * host 下发的 hardCount 仅用于预检上限, 不实际写入. */
            uint16_t softCnt = 0u;
            if (f->dataLen >= 5u) {
                if (((uint16_t)f->data[1] | ((uint16_t)f->data[2] << 8)) > APP_LOCKER_MAX_HARD) {
                    uint8_t r[2] = { sub, LOCKER_ERR_PARAM };
                    Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
                    break;
                }
                softCnt = (uint16_t)(f->data[3] | ((uint16_t)f->data[4] << 8));
            } else if (f->dataLen >= 3u) {
                softCnt = (uint16_t)(f->data[1] | ((uint16_t)f->data[2] << 8));
            }
            AppLockerItem_t none[1] = { { {0u}, 0u, 0u } };
            int e = App_Locker_Configure(none, 0u, softCnt);
            uint8_t r[2] = { sub, LOCKER_ERR_OK };
            if (e == -1)        r[1] = LOCKER_ERR_BUSY;
            else if (e == -2)   r[1] = LOCKER_ERR_PARAM;
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            break;
        }
        case LOCKER_SUB_ADD: {
            /* [cmd, epcLen, epc..] */
            if (f->dataLen < 2u) {
                uint8_t r[2] = { sub, LOCKER_ERR_PARAM };
                Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
                break;
            }
            uint8_t epcLen = f->data[1];
            if ((uint16_t)2u + epcLen > f->dataLen && epcLen > APP_LOCKER_MAX_EPC) {
                uint8_t r[2] = { sub, LOCKER_ERR_PARAM };
                Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
                break;
            }
            AppLockerItem_t it;
            it.epcLen = (epcLen > APP_LOCKER_MAX_EPC) ? APP_LOCKER_MAX_EPC : epcLen;
            it.matched = 0;
            for (uint8_t i = 0; i < it.epcLen; i++) it.epc[i] = f->data[2 + i];
            int e = App_Locker_AddTag(&it);
            uint8_t r[2] = { sub, LOCKER_ERR_OK };
            if (e == -1) r[1] = LOCKER_ERR_BUSY;
            else if (e == -2) r[1] = LOCKER_ERR_PARAM;
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            break;
        }
        case LOCKER_SUB_START: {
            int e = App_Locker_Start();
            uint8_t r[2] = { sub, LOCKER_ERR_OK };
            if (e == -1)      r[1] = LOCKER_ERR_BUSY;   /* 已激活, 需先 CANCEL */
            else if (e == -2) r[1] = LOCKER_ERR_PARAM;
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            break;
        }
        case LOCKER_SUB_CANCEL:
            if (App_LockerOneShot_IsBusy()) {
                /* 0x08 流程进行中: 打断请求 (立即回 OK, 流程安全回降后回最终帧) */
                App_LockerOneShot_Abort();
            } else {
                App_Locker_Cancel();
            }
            {
                uint8_t r[2] = { sub, LOCKER_ERR_OK };
                Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            }
            break;
        case LOCKER_SUB_GET_PROGRESS: {
            /* [cmd]  流程中进度快照 (非流程时 phase=0) */
            LockerOneShotProgress_t p;
            App_LockerOneShot_GetProgress(&p);
            uint8_t r[3 + 2 + 2 + 1 + 1 + 1 + 12];
            uint16_t pos = 0;
            r[pos++] = sub;
            r[pos++] = LOCKER_ERR_OK;
            r[pos++] = p.phase;
            r[pos++] = (uint8_t)(p.holdMs & 0xFF);  r[pos++] = (uint8_t)((p.holdMs >> 8) & 0xFF);
            r[pos++] = (uint8_t)(p.steps & 0xFF);    r[pos++] = (uint8_t)((p.steps >> 8) & 0xFF);
            r[pos++] = p.tagPresent;
            r[pos++] = p.demagDone;
            r[pos++] = p.epcLen;
            for (uint8_t i = 0; i < p.epcLen; i++) r[pos++] = p.epc[i];
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, pos);
            break;
        }
        case LOCKER_SUB_QUERY: {
            AppLockerCtx_t c;
            App_Locker_GetCtx(&c);
            uint8_t r[1 + 1 + 1 + 2 + 2 + 2 + 1] = {
                sub, LOCKER_ERR_OK,
                (uint8_t)c.state,
                (uint8_t)(c.hardMatched & 0xFF), (uint8_t)((c.hardMatched >> 8) & 0xFF),
                (uint8_t)(c.softCount  & 0xFF),  (uint8_t)((c.softCount >> 8) & 0xFF),
                (uint8_t)(c.softUsed   & 0xFF),  (uint8_t)((c.softUsed >> 8) & 0xFF),
                App_Locker_GetFaultReason()
            };
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, sizeof(r));
            break;
        }
        case LOCKER_SUB_CONSUME_SOFT: {
            int e = App_Locker_StopDecode();
            uint8_t r[2] = { sub, LOCKER_ERR_OK };
            if (e == -1) r[1] = LOCKER_ERR_BUSY;
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            break;
        }
        case LOCKER_SUB_GET_EVENT: {
            uint8_t code, epc[APP_LOCKER_MAX_EPC], elen;
            uint16_t hm, su, sc, hc;
            int rv = App_Locker_PopEvent(&code, epc, &elen, &hm, &su, &sc, &hc);
            uint8_t r[1 + 1 + 1 + APP_LOCKER_MAX_EPC + 1 + 2 + 2 + 2 + 2];
            uint16_t pos = 0;
            r[pos++] = sub; r[pos++] = LOCKER_ERR_OK;
            if (rv != 0) {
                r[1] = LOCKER_ERR_NO_EVENT;
                Proto_TxResponse(ch, FC_LOCKER_CTRL, r, pos);
                break;
            }
            r[pos++] = code;
            r[pos++] = elen;
            for (uint8_t i = 0; i < elen; i++) r[pos++] = epc[i];
            r[pos++] = (uint8_t)(hm & 0xFF); r[pos++] = (uint8_t)((hm >> 8) & 0xFF);
            r[pos++] = (uint8_t)(su & 0xFF); r[pos++] = (uint8_t)((su >> 8) & 0xFF);
            r[pos++] = (uint8_t)(sc & 0xFF); r[pos++] = (uint8_t)((sc >> 8) & 0xFF);
            r[pos++] = (uint8_t)(hc & 0xFF); r[pos++] = (uint8_t)((hc >> 8) & 0xFF);
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, pos);
            break;
        }
        case LOCKER_SUB_ONE_SHOT: {
            /* [cmd, tmoL,tmoH, maxHoldL,maxHoldH, epcLen, epc.., (demagCnt)]
             * 单标签同步开锁: 阻塞全流程后回一帧 (App_LockerOneShot.c)。
             * demagCnt: 消磁标签数, 缺省 0=跳过消磁流程。 */
            if (f->dataLen < 6u || f->data[5] == 0u || f->data[5] > 12u ||
                (uint16_t)(6u + f->data[5]) > f->dataLen) {
                uint8_t r[2] = { sub, ONE_ERR_PARAM };
                Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
                break;
            }
            uint16_t tmoMs = (uint16_t)(f->data[1] | ((uint16_t)f->data[2] << 8));
            uint16_t holdMs = (uint16_t)(f->data[3] | ((uint16_t)f->data[4] << 8));
            uint8_t demagCnt = 0u;
            if ((uint16_t)(6u + f->data[5]) < f->dataLen)
                demagCnt = f->data[6u + f->data[5]];

            LockerOneShotResult_t res;
            App_LockerOneShot_Run(&f->data[6], f->data[5], tmoMs, holdMs, demagCnt, &res);
            App_LockerOneShot_Finish();   /* 清 busy/abort/phase (含 CANCEL 打断路径) */

            uint8_t r[32];
            uint16_t pos = 0;
            r[pos++] = sub;
            r[pos++] = res.err;
            switch (res.err) {
            case ONE_ERR_OK:
                r[pos++] = res.endReason;
                r[pos++] = res.epcLen;
                for (uint8_t i = 0; i < res.epcLen; i++) r[pos++] = res.epc[i];
                r[pos++] = (uint8_t)(res.riseSteps  & 0xFF); r[pos++] = (uint8_t)((res.riseSteps  >> 8) & 0xFF);
                r[pos++] = (uint8_t)(res.lowerSteps & 0xFF); r[pos++] = (uint8_t)((res.lowerSteps >> 8) & 0xFF);
                r[pos++] = res.demagDone;
                break;
            case ONE_ERR_BUSY:
                r[pos++] = res.lockerState;
                r[pos++] = res.uhfState;
                r[pos++] = res.stepperState;
                break;
            case ONE_ERR_UHF_OPEN:
            case ONE_ERR_UHF_LINK:
            case ONE_ERR_NO_TAG:
                r[pos++] = (uint8_t)(int8_t)res.uhfRawErr;
                break;
            case ONE_ERR_MISMATCH:
                r[pos++] = (uint8_t)(res.tagsFound & 0xFF);
                r[pos++] = res.epcLen;
                for (uint8_t i = 0; i < res.epcLen; i++) r[pos++] = res.epc[i];
                break;
            case ONE_ERR_HOMING:
                r[pos++] = res.switchErr;
                break;
            case ONE_ERR_MOTOR_FAULT:
                r[pos++] = res.fault;
                r[pos++] = res.diag1;
                r[pos++] = res.diag2;
                r[pos++] = (uint8_t)(res.steps & 0xFF);
                r[pos++] = (uint8_t)((res.steps >> 8) & 0xFF);
                r[pos++] = (uint8_t)((res.steps >> 16) & 0xFF);
                r[pos++] = res.phase;
                r[pos++] = res.retreat;
                break;
            case ONE_ERR_MOTOR_TIMEOUT:
                r[pos++] = (uint8_t)(res.steps & 0xFF);
                r[pos++] = (uint8_t)((res.steps >> 8) & 0xFF);
                r[pos++] = (uint8_t)((res.steps >> 16) & 0xFF);
                r[pos++] = res.phase;
                r[pos++] = res.retreat;
                break;
            default:   /* ONE_ERR_PARAM: 无诊断字段 */
                break;
            }
            Proto_TxResponse(ch, FC_LOCKER_CTRL, r, pos);
            break;
        }
        default:
            {
                uint8_t r[2] = { sub, LOCKER_ERR_PARAM };
                Proto_TxResponse(ch, FC_LOCKER_CTRL, r, 2);
            }
            break;
        }
        break;
    }

    case FC_RGB_CTRL: {
        /* RGB 三色灯. data[0]=sub. 响应 data[0]=sub, data[1]=err. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        switch (sub) {
        case RGB_CMD_SET: {
            /* [sub, mask, reserved]. 仅设备空闲时接受为手动灯语
             * (10s 自动回收, 见 App_RgbLedPat_Manual); 业务运行中拒绝.
             * mask=0 撤销手动设色. */
            if (f->dataLen < 3u) {
                uint8_t r[2] = { sub, RGB_ERR_PARAM };
                Proto_TxResponse(ch, FC_RGB_CTRL, r, 2);
                break;
            }
            if (App_Locker_IsIdle() && !App_LockerOneShot_IsBusy() &&
                !App_MotorTest_IsBusy()) {
                App_RgbLedPat_Manual(f->data[1], RGB_MANUAL_HOLD_MS);
                uint8_t r[3] = { sub, RGB_ERR_OK, f->data[1] };
                Proto_TxResponse(ch, FC_RGB_CTRL, r, sizeof(r));
            } else {
                uint8_t r[3] = { sub, RGB_ERR_BUSY, 0u };
                Proto_TxResponse(ch, FC_RGB_CTRL, r, sizeof(r));
            }
            break;
        }
        default:
            {
                uint8_t r[2] = { sub, RGB_ERR_PARAM };
                Proto_TxResponse(ch, FC_RGB_CTRL, r, 2);
            }
            break;
        }
        break;
    }

    case FC_SELFTEST_CTRL: {
        /* 设备级自检/锁存错误位. data[0]=sub, 响应 data[0]=sub, data[1]=err.
         * App 专属 (Boot 不支持 0x0F, 收到忽略).
         * 错误位定义/置位来源见 App_CustomProtocol.h SELFTEST_* 与 App_BootSelfTest.h. */
        uint8_t sub = (f->dataLen >= 1u) ? f->data[0] : 0u;

        switch (sub) {
        case SELFTEST_SUB_QUERY: {
            uint16_t bits = App_SelfTest_GetErrBits();
            SelfTestLive_t live;
            App_SelfTest_ReadLive(&live);
            /* 通信位自愈: 链路已恢复 (实时状态=正常) 时清除锁存的通信失败位
             * (上电探测早于外设就绪导致的误锁存). 器件/参数位仍锁存. */
            uint16_t heal = 0u;
            if (live.uhfLink == 0u) heal |= SELF_ERR_UHF_COMM;
            if (live.amLink  == 0u) heal |= SELF_ERR_AM_COMM;
            if (heal != 0u) {
                App_SelfTest_ClearErrBits(heal);
                bits &= (uint16_t)~heal;
            }
            uint8_t r[10] = {
                sub, SELFTEST_ERR_OK,
                (uint8_t)(bits & 0xFFu), (uint8_t)(bits >> 8),
                live.motorCommOk, live.motorFaultReg,
                live.uhfLink, live.amLink,
                (uint8_t)((bits & SELF_ERR_PARAM_CRC) ? 1u : 0u),
                live.switchErr
            };
            Proto_TxResponse(ch, FC_SELFTEST_CTRL, r, sizeof(r));
            break;
        }
        case SELFTEST_SUB_RERUN: {
            if (App_Locker_IsIdle() && !App_LockerOneShot_IsBusy()) {
                /* 探测阻塞 ~3s (UHF Open+Query/AM Query 回帧超时), 内部逐段喂狗 */
                uint16_t bits = App_SelfTest_ProbePeripherals();
                uint8_t r[4] = { sub, SELFTEST_ERR_OK,
                                (uint8_t)(bits & 0xFFu), (uint8_t)(bits >> 8) };
                Proto_TxResponse(ch, FC_SELFTEST_CTRL, r, sizeof(r));
            } else {
                uint8_t r[2] = { sub, SELFTEST_ERR_BUSY };
                Proto_TxResponse(ch, FC_SELFTEST_CTRL, r, 2);
            }
            break;
        }
        case SELFTEST_SUB_CLEAR: {
            if (f->dataLen >= 4u) {
                uint16_t mask = (uint16_t)(f->data[2] | ((uint16_t)f->data[3] << 8));
                App_SelfTest_ClearErrBits(mask);
                uint16_t bits = App_SelfTest_GetErrBits();
                uint8_t r[4] = { sub, SELFTEST_ERR_OK,
                                (uint8_t)(bits & 0xFFu), (uint8_t)(bits >> 8) };
                Proto_TxResponse(ch, FC_SELFTEST_CTRL, r, sizeof(r));
            } else {
                uint8_t r[2] = { sub, SELFTEST_ERR_PARAM };
                Proto_TxResponse(ch, FC_SELFTEST_CTRL, r, 2);
            }
            break;
        }
        default: {
            uint8_t r[2] = { sub, SELFTEST_ERR_PARAM };
            Proto_TxResponse(ch, FC_SELFTEST_CTRL, r, 2);
            break;
        }
        }
        break;
    }

    default:
        break;
    }
}
