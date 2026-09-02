#ifndef __APP_LOCKERSEEK_H
#define __APP_LOCKERSEEK_H

#include <stdint.h>

/* =====================================================================
 * 解锁流程支撑 (Unlock 0x0A 唯一消费者):
 *   - 泵循环: 阻塞在分发上下文时手动推进各状态机 + 喂狗 + 节拍,
 *     内嵌 Proto_Poll (进度查询/CANCEL 经白名单放行, 其余 BUSY)。
 *   - 寻触驱动: 朝 dir 持续运行至 KEY_UP/KEY_DOWN 触点, 停滞/超步/
 *     超时/瞬态重试判据与回零一致 (App_MotorHoming 已验证模式)。
 *
 * BeepPulse 到期推进在泵循环内 (App_NewPeriph_BeepPulseActive):
 * 蜂鸣脉冲"只开不关", 到期静音靠轮询 — 主循环之外 (阻塞流程) 不调
 * 用则长鸣不止 (Round_089 教训)。
 * ===================================================================== */

/* ---- 寻触方向 / 限值 / 工况 (与回零一致, 实测行程x2 + 裕量) ---- */
#define LSEEK_DIR_UP        0u      /* dir=0 正转向上 (KEY_UP) */
#define LSEEK_DIR_DOWN      1u      /* dir=1 反转向下 (KEY_DOWN) */
#define LSEEK_RISE_MAX      (4085u * 2u + 800u)
#define LSEEK_LOWER_MAX     (4324u * 2u + 800u)
#define LSEEK_TIMEOUT_MS    10000u
#define LSEEK_STALL_MS      400u    /* 步数停滞判丢步 */
#define LSEEK_SPEED_HZ      2000u
#define LSEEK_TORQUE_PCT    40u

/* 绑定调用方的 CANCEL 请求/回退免疫标志 (寻触段内实时读取)。
 * 传 NULL = 该标志不参与判定 (永不打断/永不免疫)。
 * 各流程 Run 入口绑定, Finish 时解绑。 */
void LockerSeek_BindAbort(volatile uint8_t *abortFlag, volatile uint8_t *immuneFlag);

/* 泵循环: 推进 RGB 灯语/UHF/步进/AM/协议 + 喂狗 + 蜂鸣到期 + 20ms 节拍 */
void LockerSeek_Pump(void);

/* 寻触段: 0=已触到并停在该端; 1=未触到 (停滞/超步/超时/被停机);
 * 2=被 CANCEL 打断 (免疫段除外)。stepsOut=该段已走微步数。 */
int LockerSeek_Run(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut);

/* 寻触 + 失败重试一次 (覆盖上电首驱 nFAULT 瞬态误停与启动全失步)。 */
int LockerSeek_RunRetry(uint8_t dir, uint32_t maxSteps, uint32_t *stepsOut);

#endif /* __APP_LOCKERSEEK_H */
