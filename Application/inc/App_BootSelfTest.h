#ifndef __APP_BOOTSELFTEST_H
#define __APP_BOOTSELFTEST_H

#include <stdint.h>

/* =====================================================================
 * 上电自检 (POST) + 设备级自检错误位
 *   1. RGB 彩灯自检 + 逐个探测外设 (步进电机 / UHF / AM), 结果经调试串口打印
 *   2. 探测结果沉淀为 16bit 锁存错误位图 (App_SelfTest_*),
 *      经 FC_SELFTEST_CTRL (0x0F) 供上位机 读/重探/清
 *   3. 运行期故障由各模块调 App_SelfTest_SetErrBits 置位锁存:
 *      - main.c: 参数区 CRC 失败回落默认 -> PARAM_CRC
 *      - App_Locker.c locker_enter_fault: reason 1(UHF链路)->UHF_COMM,
 *        reason 2(未回零)->TRAVEL_SW
 *   4. 通信位 (UHF_COMM/AM_COMM) 可自愈: QUERY 时实时链路已恢复则清除
 *      (上电探测早于外设就绪导致的误锁存, 见 App_Dispatch.c QUERY 分支).
 *      其余位 (器件/参数/行程) 仍严格锁存, 只能 RERUN 重探或 CLEAR 清.
 *   需在开全局中断后调用 (UHF/AM 帧收依赖 USART 收中断)。
 * ===================================================================== */

/* ---- 设备级自检错误位 (锁存位图, bit=1 故障) ---- */
#define SELF_ERR_MOTOR_SPI    0x0001u  /* bit0  DRV8434S SPI 配置回读不一致 (通信/落配置失败) */
#define SELF_ERR_MOTOR_FAULT  0x0002u  /* bit1  DRV8434S 器件故障 (FAULT 位) */
#define SELF_ERR_UHF_COMM     0x0004u  /* bit2  UHF Open/Query 通信失败 */
#define SELF_ERR_AM_COMM      0x0008u  /* bit3  AM 消磁器 Query 通信失败 */
#define SELF_ERR_PARAM_CRC    0x0010u  /* bit4  参数区 CRC 失败回落默认值 (仅上电置位) */
#define SELF_ERR_TRAVEL_SW    0x0020u  /* bit5  行程开关缺失/回零失败 (bit0=上 bit1=下) */
#define SELF_ERR_KNOWN_MASK   (SELF_ERR_MOTOR_SPI  | SELF_ERR_MOTOR_FAULT | \
                               SELF_ERR_UHF_COMM   | SELF_ERR_AM_COMM     | \
                               SELF_ERR_PARAM_CRC  | SELF_ERR_TRAVEL_SW)
/* bit6~15 预留 (协议 errBitsH 高 10 位恒 0) */

/* ---- 实时诊断快照 (FC_SELFTEST_CTRL QUERY 响应附带, 非锁存) ---- */
typedef struct {
    uint8_t motorCommOk;   /* 1 = DRV8434S SPI 配置回读一致 (实时再读) */
    uint8_t motorFaultReg; /* DRV8434S 实时故障寄存器 */
    uint8_t uhfLink;       /* UHF 链路: 0=正常 1=超时 2=CRC 错误 (最近一次总线事务结果) */
    uint8_t amLink;        /* AM 链路: 0=正常 非0=掉线 (最近一次总线事务结果, 空闲不衰减) */
    uint8_t switchErr;     /* 行程开关错误位: bit0=上 bit1=下 */
} SelfTestLive_t;

/* ---- 锁存错误位 API ---- */
uint16_t App_SelfTest_GetErrBits(void);              /* 读锁存位图 */
void     App_SelfTest_SetErrBits(uint16_t mask);     /* 置位 (OR, 运行期各模块报障) */
void     App_SelfTest_ClearErrBits(uint16_t mask);   /* 清指定位 */
uint16_t App_SelfTest_ProbePeripherals(void);        /* 重探外设并刷新锁存位, 返回探测结果位图 */
void     App_SelfTest_ReadLive(SelfTestLive_t *out); /* 实时诊断快照 (只读无阻塞) */

/* ---- 上电自检入口 (RGB 彩灯 + 外设探测 + 调试串口打印) ---- */
void App_BootSelfTest_Run(void);

#endif /* __APP_BOOTSELFTEST_H */
