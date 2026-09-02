#ifndef __BOOT_FAULTSTORM_H
#define __BOOT_FAULTSTORM_H

#include <stdint.h>

/* =====================================================================
 * Boot fault 复位风暴保护 (Round_098 优化 #7)
 *  背景: Round_098 把 Boot 的 HardFault 等死循环改为 NVIC_SystemReset
 *  复位自愈 (Boot 无 IWDG, 死循环=永久哑死). 但若故障源是确定性的
 *  (如 RAM/外设状态损坏), 复位后同路径再次 fault -> 快速复位循环,
 *  主机永远抓不到 2.5s 升级窗之外的稳定窗口.
 *  机制: fault 计数存于两域都不清零的 RAM 空洞 (Boot .bss 之上,
 *  App .bss 之内 — 见 Boot_FaultStorm.c 地址说明):
 *    - 每次 fault: 计数+1 后复位; 连续 >=3 次 (期间无 App 成功启动/
 *      无主机升级动作) -> 不再复位, 常驻升级循环 (主机可稳定 IAP 救援).
 *    - App 成功启动会经 C 启动零 .bss 覆盖本地址 -> 计数天然清零;
 *      跳转 App 前 / EXIT_BOOT / EXEC 提交 也主动清零.
 *    - 掉电 (POR) 清 RAM -> 会话级语义.
 * ===================================================================== */

#define BOOT_STORM_THRESHOLD 3u   /* 连续 fault 次数阈值 */

/* fault handler 调用: 计数+1; 达阈值进入常驻救援循环(不返回), 否则复位 */
void BootStorm_OnFault(void);

/* Boot main 启动早期调用: 返回 1=已达阈值 (应强制常驻升级循环) */
int  BootStorm_Check(void);

/* 主动清零计数: 跳转 App 前 / EXIT_BOOT / EXEC 提交成功时调用 */
void BootStorm_Clear(void);

/* 当前计数 (诊断/测试用) */
uint32_t BootStorm_GetCount(void);

#endif /* __BOOT_FAULTSTORM_H */
