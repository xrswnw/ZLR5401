#ifndef __APP_RGB_LED_PATTERN_H
#define __APP_RGB_LED_PATTERN_H

#include <stdint.h>

/* =====================================================================
 * RGB 灯带状态指示仲裁器 — 设计依据: Agent/Round_010/Plan.html
 *   灯带为开关型 GPIO (无调光), 图样 = 颜色掩码 + 闪烁周期/占空比.
 *   输出优先级 (每拍唯一图样, 单色同屏):
 *     L1 行程开关错误 (黄/粉红 500ms 闪, 沿用旧 AppLed_SwitchErrProcess)
 *     L2 一次性闪显 (事件确认: 匹配/失配/软标/完成, 叠加在稳态之上)
 *     L3 稳态声明 (Locker/OneShot/MotorTest 按状态申请, 最高优先级者胜)
 *     L4 上位机手动设色 (仅 IDLE 可设, 超时自动回收)
 *     L5 灭
 *   调用约定: App_RgbLedPat_Tick() 由主循环每拍调用; 阻塞流程
 *   (OneShot one_pump) 亦须调用, 否则阻塞期间灯语冻结.
 * ===================================================================== */

/* 稳态图样 (值越大优先级越高; 见 App_Config.h RGB_HAS_BLUE 降级) */
typedef enum {
    RGBPAT_OFF             = 0,   /* 灭 (待机/回降/无需关注) */
    RGBPAT_MANUAL          = 1,   /* 上位机手动设色 (占位, 仲裁层级) */
    RGBPAT_SCAN_WAIT       = 2,   /* EPC 扫描等硬标签: 青慢闪 (降级绿慢闪) */
    RGBPAT_RISE_HOLD_GREEN = 3,   /* 升起/保持: 绿常亮 */
    RGBPAT_SOFT_WAIT_WHITE = 4,   /* 软标等待(红外/消磁): 白常亮 (降级黄慢闪) */
    RGBPAT_HOMING_SLOW     = 5,   /* 回零: 黄慢闪 */
    RGBPAT_TEST_FAST       = 6,   /* 行程测试/安全回退: 黄快闪 */
    RGBPAT_FAULT_SOLID     = 7    /* 故障: 红常亮 */
} AppRgbPat_t;

/* 一次性闪显 (叠加在当前稳态上, 到期自动回落稳态) */
typedef enum {
    RGBFLASH_NONE       = 0,
    RGBFLASH_MATCH_OK   = 1,   /* 硬标签匹配: 绿单闪 200ms */
    RGBFLASH_SOFT_OK    = 2,   /* 软标消耗/消磁成功: 白单闪 (降级黄) */
    RGBFLASH_SOFT_FAIL  = 3,   /* 消磁失败: 红双闪 1s */
    RGBFLASH_MISMATCH_3S = 4,  /* EPC 失配: 红快闪 3s, 不升起 */
    RGBFLASH_DONE_3GREEN = 5   /* 结账完成: 绿三连闪 1.8s */
} AppRgbFlash_t;

/* 稳态声明来源槽位 (各流程互斥, 占槽声明, 仲裁取最高优先级) */
typedef enum {
    RGBSRC_LOCKER = 0,          /* App_Locker 结账编排 */
    RGBSRC_ONESHOT,              /* App_LockerOneShot 单标签同步 */
    RGBSRC_MOTORTEST,            /* App_MotorTest 行程测试 */
    RGBSRC_COUNT
} AppRgbSrc_t;

void App_RgbLedPat_Init(void);

/* 每拍推进: 行程错误/闪显/稳态仲裁/手动回收, 直接写 RgbLedHl_Set.
 * 主循环每迭代调用一次 (阻塞流程内由其泵循环调用). */
void App_RgbLedPat_Tick(void);

/* 稳态声明/撤销 (pat=RGBPAT_OFF 即撤销). 各流程每拍重复声明幂等. */
void App_RgbLedPat_Set(uint8_t src, uint8_t pat);
void App_RgbLedPat_Clear(uint8_t src);

/* 一次性闪显入队 (深度 1, 新事件覆盖未完成的旧闪显) */
void App_RgbLedPat_Flash(uint8_t id);

/* 上位机手动设色: mask 为 RGB_BIT_* 掩码; mask=0 立即撤销.
 * holdMs 到期后自动灭并让位业务灯语 (仅 IDLE 语义由 Dispatch 保证). */
void App_RgbLedPat_Manual(uint8_t mask, uint16_t holdMs);

#endif /* __APP_RGB_LED_PATTERN_H */
