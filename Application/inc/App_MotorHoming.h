#ifndef __APP_MOTORHOMING_H
#define __APP_MOTORHOMING_H

#include <stdint.h>

/* =====================================================================
 * 上电行程自检 / 回零 (App_MotorHoming.c)
 *  在开全局中断后、首次业务运动前调用一次; 无论电机停在何处均完整自检:
 *     阶段1 向上找 KEY_UP  (验证上行程开关在位/可达)
 *     阶段2 向下回 KEY_DOWN (验证下行程开关并建立下行程绝对基准)
 *     任一阶段失败(堵转/超步/超时) -> 置对应错误位(bit0=上,bit1=下)并禁止 MOVE/TEST
 *  返回值: 0=自检通过且已回零(可运动)  非0=失败(行程开关异常, 需人工处置)
 * ===================================================================== */

/* 行程自检/回零结果 */
typedef enum {
    MOTOR_HOMING_OK     = 0,   /* 成功: 已回零至 KEY_DOWN, 可运动 */
    MOTOR_HOMING_NOSW   = -1,  /* 上/下行程开关均缺失(开路): 禁止运动 */
    MOTOR_HOMING_TIMEOUT= -2,  /* 回零超时/超限(单开关失效) */
    MOTOR_HOMING_FAULT  = -3   /* 电机故障 */
} MotorHomingResult_t;

MotorHomingResult_t App_MotorHoming_Run(void);

/* 1=已回零/建立基准 (可安全 MOVE/TEST); 0=行程开关缺失或回零失败 */
int  App_MotorHoming_IsReady(void);

#endif /* __APP_MOTORHOMING_H */
