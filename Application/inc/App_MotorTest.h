#ifndef __APP_MOTORTEST_H
#define __APP_MOTORTEST_H

#include <stdint.h>

/* =====================================================================
 * 电机自动化行程测试模块 (FC_MOTOR_CTRL 子命令 TEST, 由上位机触发)
 *  流程: 以最高速正转 -> 触发(任一)行程开关 -> 立即反转 -> 再触发 -> 完成 1 次往返.
 *  state: IDLE / RUN / DONE / FAULT, 由主循环 App_MotorTest_Process 驱动.
 *  极性无关: 任一触点触发即换向, 自校准, 不依赖方向位物理极性.
 * ===================================================================== */

#define MT_TEST_TIMEOUT_MS   10000u   /* 单程无触点超时 -> FAULT */
#define MT_DEBOUNCE_MS       20u      /* 触点连续保持触发时长, 防抖 */

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
void      App_MotorTest_GetProgress(uint8_t *total, uint8_t *done);  /* 进度 */

#endif /* __APP_MOTORTEST_H */
