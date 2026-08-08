#ifndef __APP_DISPATCH_H
#define __APP_DISPATCH_H

#include "App_CustomProtocol.h"
#include "App_Param.h"

extern DeviceParam_t g_sParam;   /* 定义在 App_Dispatch.c */

/* 应用层初始化: 注册帧回调 */
void AppDispatchInit(void);

/* 帧回调: 协议层解析出完整帧后调用 */
void AppDispatch(ProtoFrame_t *frame);

/* 主循环调: 处理延迟/分片任务 (如 DEVICE_INFO 响应分片发送) */
void AppDispatch_Poll(void);

#endif /* __APP_DISPATCH_H */
