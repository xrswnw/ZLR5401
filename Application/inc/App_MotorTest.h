#ifndef __APP_MOTORTEST_H
#define __APP_MOTORTEST_H

#include <stdint.h>

/* =====================================================================
 * 电机自动化行程测试模块 (FC_MOTOR_CTRL 子命令 TEST, 由上位机触发)
 *  流程: 正转向上 -> 触上行程开关 -> 立即反转向下 -> 触下行程开关,
 *  完成 1 次往返, 重复 passes 次后停转.
 *  state: IDLE / RUN / DONE / FAULT, 由主循环 App_MotorTest_Process 驱动.
 *  极性相关: 正转只认 KEY_UP、反转只认 KEY_DOWN; 反向错触判 FAULT.
 *
 *  速度与超时联动: 速度越低, 单程走完所需时间越长. 若仅降速而超时不放大,
 *  会在行程中途被误判单程超时 -> 假 FAULT. 二者须成对调整
 *  (超时 @ 旧速2000 = 10s; 每降 k 倍速, 超时应放大 ~k 倍).
 * ===================================================================== */

#define MT_TEST_TIMEOUT_MS   90000u  /* 单程无触点超时 -> FAULT. 1000微步/s下正程≈2.1s, 超大余量; 该值远大于任何档位单程所需 */
#define MT_DEBOUNCE_MS       5u       /* 触点连续保持触发时长, 防抖. 20ms->5ms 缩短反转迟滞(高速下触点抖动风险略升, 已实测OK) */

typedef enum {
    MT_STATE_IDLE  = 0,
    MT_STATE_RUN   = 1,
    MT_STATE_DONE  = 2,
    MT_STATE_FAULT = 3
} AppMotorTestState_t;

void      App_MotorTest_Init(void);                 /* 复位状态 = IDLE */
int       App_MotorTest_Start(uint8_t passes);      /* 0 OK / -1 BUSY / -2 PARAM / -3 电机故障 */
int       App_MotorTest_Stop(void);                 /* 中止, 停电机, 回 IDLE */
void      App_MotorTest_Process(void);              /* 主循环节拍 */
int       App_MotorTest_IsBusy(void);               /* 1=正在测试 */
uint8_t   App_MotorTest_GetState(void);            /* IDLE/RUN/DONE/FAULT */
uint8_t   App_MotorTest_GetFaultReason(void);      /* 测试故障原因 (诊断) */
void      App_MotorTest_GetProgress(uint8_t *total, uint8_t *done);  /* 进度 */

#endif /* __APP_MOTORTEST_H */
