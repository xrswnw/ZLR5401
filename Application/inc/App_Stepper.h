#ifndef __APP_STEPPER_H
#define __APP_STEPPER_H

#include <stdint.h>

/* =====================================================================
 * 步进电机应用层 (DRV8434S 状态机 + 控制 API)
 *  - 状态机: IDLE(停止) / RUN(运行) / FAULT(故障), 由 App_Stepper_Process 驱动
 *  - 运动: 方向 + 目标微步数 (0 = 持续运行), 逐步 SPI 步进, 由主循环节拍推进
 *  - 每 tick 检查 nFAULT 与故障寄存器, 异常置 FAULT 并停转
 *  底层: App_Motor_HL (SPI2+GPIO) + DRV8434S SDK (Application/Drv8434S)
 * ===================================================================== */

typedef enum {
    APP_STEPPER_IDLE  = 0,   /* 负载关断 (EN_OUT=0) */
    APP_STEPPER_RUN   = 1,   /* 运行中 */
    APP_STEPPER_FAULT = 2    /* 故障保持 */
} AppStepperState_t;

/* 一次运动请求 */
typedef struct {
    uint8_t  dir;            /* 0=CW, 1=CCW */
    uint32_t steps;          /* 目标微步数, 0 = 持续运行 */
} AppStepperMove_t;

/* ---- 初始化 / 周期处理 ---- */
void     App_Stepper_Init(void);         /* HAL + SDK 初始化, 上电配置, 停转 */
void     App_Stepper_Process(void);      /* 主循环节拍: 推进步进 + 故障监测 */

/* ---- 控制 API (由协议层或任务调用) ---- */
int      App_Stepper_Move(AppStepperMove_t *mv);      /* 启动一次运动 (先停止再启动) */
int      App_Stepper_Stop(void);                       /* 停止并关断输出 */
int      App_Stepper_DcBrakeStop(void);                /* DC磁制动停: 短暂维持磁场抗滑行再断电 (触点停位用) */
int      App_Stepper_SetSpeedHz(uint32_t hz);          /* 设定步进频率 (微步/s, 1~2000) */
int      App_Stepper_SetTorquePercent(uint8_t pct);    /* 转矩百分比 6~100 (整数, 内部转 TRQ_DAC) */
int      App_Stepper_ClearFault(void);                 /* 清故障并回到 IDLE */

/* ---- 状态 / 信息 (供协议查询) ---- */
AppStepperState_t App_Stepper_GetState(void);
uint8_t  App_Stepper_GetFault(void);       /* 最近一次读取的 FAULT 寄存器原始值 */
uint8_t  App_Stepper_GetDiag1(void);
uint8_t  App_Stepper_GetDiag2(void);
uint8_t  App_Stepper_GetMicrostep(void);  /* 诊断: 实时读 DRV8434S CTRL3 微步位 */
uint32_t App_Stepper_GetStepsDone(void);   /* 本次运动已完成微步数 */

#endif /* __APP_STEPPER_H */
