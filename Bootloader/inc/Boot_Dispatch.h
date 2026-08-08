#ifndef __BOOT_DISPATCH_H
#define __BOOT_DISPATCH_H

#include "Boot_CustomProtocol.h"
#include "Boot_Param.h"

/* IAP context state */
typedef enum {
    IAP_IDLE = 0,
    IAP_ERASED,
    IAP_DATA_DONE,
    IAP_VERIFY_OK
} IapState_t;

typedef struct {
    IapState_t State;
    uint32_t   FwSize;
    uint32_t   FwCrc32;
    uint32_t   FwVer;
    uint32_t   BindVerify;
    uint32_t   Written;
    uint8_t    VerifyLevel;
} IapCtx_t;

extern IapCtx_t g_sIap;
extern DeviceParam_t g_sParam;

/* 应用层初始化: 注册帧回调 */
void BootDispatchInit(void);

/* 帧回调: 协议层解析出完整帧后调用 */
void BootDispatch(ProtoFrame_t *frame);

#endif /* __BOOT_DISPATCH_H */
