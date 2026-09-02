#ifndef __APP_MOTORHOMING_H
#define __APP_MOTORHOMING_H

#include <stdint.h>

/* =====================================================================
 * 上电行程自检 / 回零 (App_MotorHoming.c) — Round_098 #11 后台化
 *  非阻塞状态机: System_Init 末尾 App_MotorHoming_Start() 启动,
 *  主循环 App_MotorHoming_Process() 推进; 回零期间 USB 协议照常服务
 *  (MOVE/TEST 回 BUSY, 见 App_Dispatch), 回零完成后自动放行。
 *  无论电机停在何处均完整自检:
 *     阶段1 向上找 KEY_UP  (验证上行程开关在位/可达)
 *     阶段2 向下回 KEY_DOWN (验证下行程开关并建立下行程绝对基准)
 *     任一阶段失败(堵转/超步/超时) -> 整体重试至多 3 次(200ms 间隔),
 *     全部失败置对应错误位(bit0=上,bit1=下)+自检锁存位, 禁止 MOVE/TEST
 * ===================================================================== */

/* 回零状态 (IO 诊断/上位机可见) */
typedef enum {
    HOMING_STAT_NONE    = 0,   /* 未启动 */
    HOMING_STAT_RUNNING = 1,   /* 进行中 (BUSY) */
    HOMING_STAT_READY   = 2,   /* 已回零, 可运动 */
    HOMING_STAT_FAILED  = 3    /* 失败 (行程开关异常, 需人工处置) */
} MotorHomingStatus_t;

void    App_MotorHoming_Start(void);      /* 启动后台回零 (幂等: 已启动/完成则忽略) */
void    App_MotorHoming_Process(void);    /* 主循环推进 (未启动时为空操作) */
uint8_t App_MotorHoming_GetStatus(void);  /* MotorHomingStatus_t */

/* 1=已回零/建立基准 (可安全 MOVE/TEST); 0=未完成/失败/未启动 */
int  App_MotorHoming_IsReady(void);

#endif /* __APP_MOTORHOMING_H */
