#include "Boot_Dispatch.h"
#include "Boot_Config.h"
#include "Boot_CustomProtocol.h"
#include "Boot_Param.h"
#include "Boot_Param_HL.h"
#include "Boot_SysTick_HL.h"   /* SysTickHl_DelayMs (FC_RESET 等 20ms)*/
#include "Boot_Iwdg_HL.h"      /* IwdgHl_Feed (长擦除循环喂狗)*/
#include "stm32f10x.h"

IapCtx_t g_sIap;
DeviceParam_t g_sParam;

void BootDispatchInit(void) {
    Proto_Init();
    Proto_SetDeviceAddr(g_sParam.deviceAddr);
    Proto_RegisterFrameCb(BootDispatch);
    /* USB transport 由后续 Boot_Usb_Init 注册; 默认通道设为 USB (Boot 层走 USB HID, 与 App 对齐)*/
    Proto_SetDefaultChannel(PROTO_CH_USB);
}

void BootDispatch(ProtoFrame_t *f) {
    uint8_t devAddr = f->devAddr;
    uint8_t ch = f->channel;   /* 从哪收就从哪回*/

    switch (f->func) {

    case FC_HANDSHAKE: {
        /* 协议 (App/Boot 一致): 响应 payload (28B), 单包可发
         * result(1) | ProtoVer(1) | Status(1)
         * | UID(12) | UidHash(4 LE) | Layer(1) | UpgradeCount(4 LE) | BaudRate(4 LE)*/
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
        rsp[pos++] = 0;  /* Layer=0=BOOT*/
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount);
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount >> 8);
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount >> 16);
        rsp[pos++] = (uint8_t)(g_sParam.upgradeCount >> 24);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate >> 8);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate >> 16);
        rsp[pos++] = (uint8_t)(g_sParam.baudRate >> 24);
        Proto_TxResponseTo(ch, devAddr, FC_HANDSHAKE, rsp, pos);
        break;
    }

    case FC_UPGRADE_START: {
        uint8_t rsp[1];
        if (f->dataLen < 16) {
            rsp[0] = UPG_NO_SPACE;
            Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_START, rsp, 1);
            break;
        }
        g_sIap.FwSize = (uint32_t)f->data[0]
                    | ((uint32_t)f->data[1] << 8)
                    | ((uint32_t)f->data[2] << 16)
                    | ((uint32_t)f->data[3] << 24);
        g_sIap.FwCrc32 = (uint32_t)f->data[4]
                     | ((uint32_t)f->data[5] << 8)
                     | ((uint32_t)f->data[6] << 16)
                     | ((uint32_t)f->data[7] << 24);
        g_sIap.FwVer = (uint32_t)f->data[8]
                   | ((uint32_t)f->data[9] << 8)
                   | ((uint32_t)f->data[10] << 16)
                   | ((uint32_t)f->data[11] << 24);
        g_sIap.BindVerify = (uint32_t)f->data[12]
                        | ((uint32_t)f->data[13] << 8)
                        | ((uint32_t)f->data[14] << 16)
                        | ((uint32_t)f->data[15] << 24);
        /* 默认校验级别 MID(含向量校验); FULL 由主机显式请求*/
        g_sIap.VerifyLevel = (f->dataLen > 16) ? f->data[16] : VERIFY_LEVEL_MID;

        if (g_sIap.FwSize > APP_FLASH_SIZE || g_sIap.FwSize == 0) {
            rsp[0] = UPG_NO_SPACE;
            Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_START, rsp, 1);
            break;
        }

        FlashHl_WriteBegin();
        /* 按需擦除: 仅擦 ceil(FwSize / PAGE) 页, 越界仍以 APP_FLASH_END 兜底*/
        int eraseOk = 1;
        uint32_t erasePages = (g_sIap.FwSize + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
        for (uint32_t i = 0; i < erasePages; i++) {
            uint32_t addr = APP_FLASH_ORIGIN + i * FLASH_PAGE_SIZE;
            if (addr >= APP_FLASH_END) break;
            if (FlashHl_ErasePage(addr) != 0) { eraseOk = 0; break; }
            /* 整片擦除是一段连续调用(主循环无法插入喂狗),
             * 40 页最坏 ~1.2~2.4s 可能逼近/超过 IWDG 超时; 每页擦后喂一次,
             * 把最长无喂狗间隔压到单页擦除时长(~30~60ms) << IWDG 超时.*/
            IwdgHl_Feed();
        }
        FlashHl_WriteEnd();

        if (!eraseOk) {
            rsp[0] = UPG_ERASE_FAIL;
            Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_START, rsp, 1);
            break;
        }

        g_sIap.Written = 0;
        g_sIap.State = IAP_ERASED;
        rsp[0] = RESULT_OK;
        Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_START, rsp, 1);
        break;
    }

    case FC_FW_DATA: {
        uint8_t rsp[3];
        if (f->dataLen < 4 || g_sIap.State != IAP_ERASED) {
            rsp[0] = (uint8_t)f->data[0];
            rsp[1] = (uint8_t)(f->dataLen > 1 ? f->data[1] : 0);
            rsp[2] = DATA_ADDR_ERR;
            Proto_TxResponseTo(ch, devAddr, FC_FW_DATA, rsp, 3);
            break;
        }

        uint16_t seq = (uint16_t)f->data[0] | ((uint16_t)f->data[1] << 8);
        uint16_t addrOffset = (uint16_t)f->data[2] | ((uint16_t)f->data[3] << 8);
        uint16_t dataLen = f->dataLen - 4;
        const uint8_t *data = &f->data[4];

        uint32_t target = APP_FLASH_ORIGIN + addrOffset;
        if (target < APP_FLASH_ORIGIN || target + dataLen > APP_FLASH_END) {
            rsp[0] = (uint8_t)seq;
            rsp[1] = (uint8_t)(seq >> 8);
            rsp[2] = DATA_ADDR_ERR;
            Proto_TxResponseTo(ch, devAddr, FC_FW_DATA, rsp, 3);
            break;
        }

        FlashHl_WriteBegin();
        int writeOk = 1;
        for (uint16_t i = 0; i < dataLen; i += 4) {
            uint32_t word = 0xFFFFFFFF;
            for (uint16_t j = 0; j < 4 && (i + j) < dataLen; j++) {
                ((uint8_t *)&word)[j] = data[i + j];
            }
            if (FlashHl_WriteWord(target + i, word) != 0) {
                writeOk = 0; break;
            }
        }
        FlashHl_WriteEnd();

        if (!writeOk) {
            rsp[0] = (uint8_t)seq;
            rsp[1] = (uint8_t)(seq >> 8);
            rsp[2] = DATA_FLASH_FAIL;
            Proto_TxResponseTo(ch, devAddr, FC_FW_DATA, rsp, 3);
            break;
        }

        /* 以"最高已覆盖地址"计进度 (按最大写地址而非累计字节),
           使重复/乱序重发幂等: 重复块不再推进 Written, 不会提前越过 FwSize。 */
        uint32_t covered = target + dataLen - APP_FLASH_ORIGIN;
        if (covered > g_sIap.Written)
            g_sIap.Written = covered;
        if (g_sIap.Written >= g_sIap.FwSize)
            g_sIap.State = IAP_DATA_DONE;

        rsp[0] = (uint8_t)seq;
        rsp[1] = (uint8_t)(seq >> 8);
        rsp[2] = RESULT_OK;
        Proto_TxResponseTo(ch, devAddr, FC_FW_DATA, rsp, 3);
        break;
    }

    case FC_UPGRADE_VERIFY: {
        uint8_t rsp[2];
        Memset8(rsp, 0, sizeof(rsp));

        /* 基本级: 大小校验*/
        if (g_sIap.Written != g_sIap.FwSize ||
            g_sIap.State != IAP_DATA_DONE) {
            rsp[0] = VERIFY_SIZE_MISMATCH;
            Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_VERIFY, rsp, 2);
            break;
        }

        /* 基本级: CRC32校验*/
        uint32_t calcCrc = Crc32Calc((const uint8_t *)APP_FLASH_ORIGIN,
                                      g_sIap.FwSize);
        if (calcCrc != g_sIap.FwCrc32) {
            rsp[0] = VERIFY_CRC_MISMATCH;
            Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_VERIFY, rsp, 2);
            break;
        }

        /* 中级: MSP向量校验*/
        if (g_sIap.VerifyLevel <= VERIFY_LEVEL_MID) {
            uint32_t mspVal = *(volatile uint32_t *)APP_FLASH_ORIGIN;
            if ((mspVal & 0x2FFC0000) != 0x20000000) {
                rsp[0] = VERIFY_VECTOR_INVALID;
                Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_VERIFY, rsp, 2);
                break;
            }
            /* Reset向量校验*/
            uint32_t resetVec = *(volatile uint32_t *)(APP_FLASH_ORIGIN + 4);
            if (resetVec < APP_FLASH_ORIGIN ||
                resetVec >= APP_FLASH_ORIGIN + g_sIap.FwSize) {
                rsp[0] = VERIFY_VECTOR_INVALID;
                rsp[1] = 1;  /* Reset向量越界*/
                Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_VERIFY, rsp, 2);
                break;
            }
        }

        /* 全级: 绑定校验*/
        if (g_sIap.VerifyLevel == VERIFY_LEVEL_FULL) {
            uint32_t localBind = Crc32CalcReflect(
                (const uint8_t *)APP_FLASH_ORIGIN,
                g_sIap.FwSize,
                (const uint8_t *)&g_sParam.deviceUidHash, 4);
            if (localBind != g_sIap.BindVerify) {
                rsp[0] = VERIFY_BIND_FAIL;
                Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_VERIFY, rsp, 2);
                break;
            }
        }

        g_sIap.State = IAP_VERIFY_OK;
        rsp[0] = RESULT_OK;
        Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_VERIFY, rsp, 2);
        break;
    }

    case FC_UPGRADE_EXEC: {
        uint8_t rsp[1];
        if (g_sIap.State != IAP_VERIFY_OK) {
            /* 未通过校验, 不允许执行*/
            rsp[0] = VERIFY_CRC_MISMATCH;
            Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_EXEC, rsp, 1);
            break;
        }
        rsp[0] = RESULT_OK;
        Proto_TxResponseTo(ch, devAddr, FC_UPGRADE_EXEC, rsp, 1);

        /* 与 FC_RESET/EXIT_BOOT 对齐 — 发响应后延时 20ms, 等 USB 1ms 帧周期
         * 把 ACK 完整推给主机再复位, 避免 ACK 随 NVIC_SystemReset 丢失.
         * (原 EXEC 缺此延时, USB 通道下升级成功确认可能丢, 主机误判失败.)*/
        SysTickHl_DelayMs(20);

        /* 两步原子提交, 收窄"标记可跳"与"记录新CRC"的半提交窗口.
         * Step 1: 先写 校验值/计数/绑定, deviceStatus 仍保持 UPG (不掉电则下次仍留升级态).
         * Step 2: 再单独一次 ParamSave 把 deviceStatus=RUN 作为"提交点"(最后写).
         * 断电语义: 只要 RUN 没写成, 下次仍留 UPG 升级态; 此时 appCrc32 已是新值且
         * App 区已是新固件, 主机重发 START+DATA+VERIFY 即可, 不会跳坏 App.
         * 代价: 每次升级多一次 ParamSave (多擦写一次参数页, 寿命可接受).
         * 注: 不改校验逻辑 — 新 MCU 首烧按 MID(无绑定)直接放行, FULL 绑定仍拦跨设备刷写.*/
        g_sParam.appCrc32      = g_sIap.FwCrc32;
        g_sParam.appSize       = g_sIap.FwSize;
        g_sParam.upgradeCount++;
        g_sParam.bindVerify    = g_sIap.BindVerify;
        ParamSave(&g_sParam);

        /* Step 2: 提交点 — 最后才把 deviceStatus 置 RUN*/
        g_sParam.deviceStatus  = PARAM_STATUS_RUN;
        ParamSave(&g_sParam);

        NVIC_SystemReset();
        break;
    }

    case FC_DEVICE_INFO: {
        /* 协议 (Boot 层): 响应 payload (34B), 单包可发
         * result(1) | Addr(1) | hwVersion(16) | Version/bootVersion(16)*/
        uint8_t rsp[1 + 1 + VER_STR_LEN + VER_STR_LEN];
        Memset8(rsp, 0, sizeof(rsp));
        uint16_t pos = 0;
        rsp[pos++] = RESULT_OK;
        rsp[pos++] = g_sParam.deviceAddr;
        Memcpy(&rsp[pos], g_sParam.hwVersion, VER_STR_LEN); pos += VER_STR_LEN;
        Memcpy(&rsp[pos], g_sParam.bootVersion, VER_STR_LEN); pos += VER_STR_LEN;
        Proto_TxResponseTo(ch, devAddr, FC_DEVICE_INFO, rsp, pos);
        break;
    }

    case FC_RESET: {
        /* 空参数指令, 回 OK 后 NVIC_SystemReset
         * 模式与 FC_UPGRADE_EXEC (line 235-254) 一致: 先发响应, 延时 20ms
         * 等 USB 1ms 帧周期 + 主机轮询拿走再 reset, 避免 ACK 随复位丢。*/
        uint8_t rsp[1];
        if (f->dataLen != 0) {
            rsp[0] = 1;     /* param len must be 0*/
            Proto_TxResponseTo(ch, devAddr, FC_RESET, rsp, 1);
            break;
        }
        rsp[0] = RESULT_OK;
        Proto_TxResponseTo(ch, devAddr, FC_RESET, rsp, 1);
        SysTickHl_DelayMs(20);
        NVIC_SystemReset();
        break;
    }

    case FC_EXIT_BOOT: {
        /* 退出升级. 主机进 Boot 后若不打算升级, 发本指令退出.
         * 校验 App flash 完好 (向量 + CRC, 复用 JumpToApp 的判定):
         * 1) MSP 向量合法 (0x2FFC0000 mask == 0x20000000)
         * 2) Reset 向量落在 App 区
         * 3) 若参数记录了 appCrc32/appSize, App flash CRC 必须匹配
         * 校验通过: 清 deviceStatus UPG->RUN + ParamSave + 回 OK + 延 50ms + 复位
         * -> 复位后 Boot 读 RUN, stayInUpdate=false, 2.5s 窗口后 JumpToApp.
         * 校验失败: 回错误码, 不复位 (flash 被擦除/损坏时不能跳空 App).*/
        uint8_t rsp[1];
        if (f->dataLen != 0) {
            rsp[0] = 1;     /* param len must be 0*/
            Proto_TxResponseTo(ch, devAddr, FC_EXIT_BOOT, rsp, 1);
            break;
        }

        /* 1) MSP 向量合法性*/
        if ((*(volatile uint32_t *)APP_FLASH_ORIGIN & 0x2FFC0000) != 0x20000000) {
            rsp[0] = EXIT_BOOT_NO_APP;
            Proto_TxResponseTo(ch, devAddr, FC_EXIT_BOOT, rsp, 1);
            break;
        }
        /* 2) Reset 向量合法性*/
        uint32_t resetVec = *(volatile uint32_t *)(APP_FLASH_ORIGIN + 4);
        if (resetVec < APP_FLASH_ORIGIN ||
            resetVec >= APP_FLASH_ORIGIN + APP_FLASH_SIZE) {
            rsp[0] = EXIT_BOOT_NO_APP;
            Proto_TxResponseTo(ch, devAddr, FC_EXIT_BOOT, rsp, 1);
            break;
        }
        /* 3) CRC 校验 (仅当参数记录了有效 CRC/Size; 与 JumpToApp 一致)*/
        if (g_sParam.appCrc32 != 0 && g_sParam.appSize != 0 &&
            g_sParam.appSize <= APP_FLASH_SIZE) {
            uint32_t crc = Crc32Calc((const uint8_t *)APP_FLASH_ORIGIN,
                                      g_sParam.appSize);
            if (crc != g_sParam.appCrc32) {
                rsp[0] = EXIT_BOOT_CRC_MISMATCH;
                Proto_TxResponseTo(ch, devAddr, FC_EXIT_BOOT, rsp, 1);
                break;
            }
        }

        /* 校验通过: 清 UPG -> RUN, 持久化, 复位跳 App*/
        g_sParam.deviceStatus = PARAM_STATUS_RUN;
        if (ParamSave(&g_sParam) != 0) {
            rsp[0] = ENTER_BOOT_PARAM_ERR;   /* param 写失败, 复用 ENTER_BOOT 的 param err 码*/
            Proto_TxResponseTo(ch, devAddr, FC_EXIT_BOOT, rsp, 1);
            break;
        }
        rsp[0] = RESULT_OK;
        Proto_TxResponseTo(ch, devAddr, FC_EXIT_BOOT, rsp, 1);
        SysTickHl_DelayMs(50);
        NVIC_SystemReset();
        break;
    }

    default:
        break;
    }
}
