#include "App_RgbLed_Pattern.h"
#include "App_RgbLed_HL.h"
#include "App_Config.h"
#include "App_Stepper.h"
#include "App_SysTick_HL.h"

/* =====================================================================
 * RGB 灯带状态指示仲裁器实现 — 依据 Agent/Round_010/Plan.html。
 * 无调光: 图样 = (颜色掩码, 周期, 亮窗) 查表; 闪显为有限时长窗口,
 * 稳态由各流程按槽位声明, 仲裁取最高优先级图样。
 * ===================================================================== */

/* 颜色掩码 (蓝灯硬件缺失时降级: 白->黄, 粉红->红; Round_011 图样级降级见查表) */
#if RGB_HAS_BLUE
#define C_GREEN   (RGB_BIT_G)
#define C_RED     (RGB_BIT_R)
#define C_YELLOW  (RGB_BIT_G | RGB_BIT_R)
#define C_BLUE    (RGB_BIT_B)
#define C_WHITE   (RGB_BIT_G | RGB_BIT_R | RGB_BIT_B)
#define C_PINK    (RGB_BIT_R | RGB_BIT_B)
#else
#define C_GREEN   (RGB_BIT_G)
#define C_RED     (RGB_BIT_R)
#define C_YELLOW  (RGB_BIT_G | RGB_BIT_R)
#define C_BLUE    (RGB_BIT_G)                  /* 降级: 蓝只亮绿 */
#define C_WHITE   (RGB_BIT_G | RGB_BIT_R)       /* 降级: 白只亮黄 */
#define C_PINK    (RGB_BIT_R)                   /* 降级: 粉红只亮红 */
#endif

/* 稳态图样: periodMs=0 为常亮; 否则 (now % periodMs) < onMs 时亮 */
typedef struct {
    uint8_t  onMask;
    uint16_t periodMs;
    uint16_t onMs;
} PatDesc_t;

/* Round_011 方案A "五色叙事": 等放标=白慢闪(该你了), 盘点中=蓝慢闪(该我了);
 * 无蓝硬件时图样级降级 (白慢闪->黄慢闪, 蓝慢闪->绿慢闪), 与颜色掩码降级独立. */
static const PatDesc_t s_pat[] = {
#if RGB_HAS_BLUE
    [RGBPAT_IR_WAIT]         = { C_WHITE, RGB_PAT_BLINK_MS, RGB_PAT_BLINK_MS / 2u },
    [RGBPAT_SCAN_ACTIVE]     = { C_BLUE,  RGB_PAT_BLINK_MS, RGB_PAT_BLINK_MS / 2u },
#else
    [RGBPAT_IR_WAIT]         = { C_YELLOW, RGB_PAT_BLINK_MS, RGB_PAT_BLINK_MS / 2u },
    [RGBPAT_SCAN_ACTIVE]     = { C_GREEN,  RGB_PAT_BLINK_MS, RGB_PAT_BLINK_MS / 2u },
#endif
    [RGBPAT_RISE_HOLD_GREEN] = { C_GREEN, 0u, 0u },
#if RGB_HAS_BLUE
    [RGBPAT_SOFT_WAIT_WHITE] = { C_WHITE, 0u, 0u },           /* 白常亮 */
#else
    [RGBPAT_SOFT_WAIT_WHITE] = { C_YELLOW, RGB_PAT_BLINK_MS, RGB_PAT_BLINK_MS / 2u },  /* 降级黄慢闪 */
#endif
    [RGBPAT_HOMING_SLOW]     = { C_YELLOW, RGB_PAT_BLINK_MS, RGB_PAT_BLINK_MS / 2u },
    [RGBPAT_TEST_FAST]       = { C_YELLOW, RGB_PAT_FAST_MS, RGB_PAT_FAST_MS / 2u },
    [RGBPAT_FAULT_SOLID]     = { C_RED, 0u, 0u },
};

/* 一次性闪显: 有限时长窗口, 到期回落稳态 */
typedef struct {
    uint8_t  onMask;
    uint16_t periodMs;
    uint16_t onMs;
    uint16_t totalMs;
} FlashDesc_t;

static const FlashDesc_t s_flash[] = {
    [RGBFLASH_MATCH_OK]    = { C_GREEN,  200u,  200u,  200u },   /* 绿单闪 */
#if RGB_HAS_BLUE
    [RGBFLASH_SOFT_OK]     = { C_WHITE,  200u,  200u,  200u },   /* 白单闪 */
#else
    [RGBFLASH_SOFT_OK]     = { C_YELLOW, 200u,  200u,  200u },   /* 降级黄单闪 */
#endif
    [RGBFLASH_FAIL_DOUBLE] = { C_RED,    500u,  250u, 1000u },   /* 红双闪 */
    [RGBFLASH_MISMATCH_3S] = { C_RED,    500u,  250u, 3000u },   /* 红快闪 3s */
    [RGBFLASH_DONE_3GREEN] = { C_GREEN,  600u,  300u, 1800u },   /* 绿三连闪 */
#if RGB_HAS_BLUE
    [RGBFLASH_TAG_SEEN]    = { C_BLUE,   200u,  200u,  200u },   /* 蓝单闪: 读到一张标签 */
#else
    [RGBFLASH_TAG_SEEN]    = { C_YELLOW, 200u,  200u,  200u },   /* 降级黄单闪 (与命中绿闪区分) */
#endif
    [RGBFLASH_WARN_2S]     = { C_YELLOW, 1000u,  500u, 2000u },  /* 黄慢闪 2s: 未放标/无标签收尾 */
};

static uint8_t  s_claim[RGBSRC_COUNT];   /* 各源当前稳态声明 (0=OFF) */
static uint8_t  s_flashId;               /* 进行中的闪显 (0=无) */
static uint32_t s_flashT0;               /* 闪显起点 ms */
static uint8_t  s_manualMask;            /* 上位机手动颜色 (0=无) */
static uint32_t s_manualUntil;           /* 手动到期时刻 (0=无) */

void App_RgbLedPat_Init(void)
{
    for (uint8_t i = 0u; i < RGBSRC_COUNT; i++) s_claim[i] = RGBPAT_OFF;
    s_flashId = RGBFLASH_NONE;
    s_flashT0 = 0u;
    s_manualMask = 0u;
    s_manualUntil = 0u;
}

void App_RgbLedPat_Set(uint8_t src, uint8_t pat)
{
    if (src >= RGBSRC_COUNT) return;
    if (pat > RGBPAT_FAULT_SOLID) return;
    s_claim[src] = pat;
}

void App_RgbLedPat_Clear(uint8_t src)
{
    if (src >= RGBSRC_COUNT) return;
    s_claim[src] = RGBPAT_OFF;
}

void App_RgbLedPat_Flash(uint8_t id)
{
    if (id == RGBFLASH_NONE || id > RGBFLASH_WARN_2S) return;
    s_flashId = id;
    s_flashT0 = SysTickHl_GetMs();
}

void App_RgbLedPat_Manual(uint8_t mask, uint16_t holdMs)
{
    mask = (uint8_t)(mask & (RGB_BIT_G | RGB_BIT_R | RGB_BIT_B));
    if (mask == 0u || holdMs == 0u) {          /* mask=0: 撤销手动 */
        s_manualMask = 0u;
        s_manualUntil = 0u;
        return;
    }
    s_manualMask = mask;
    s_manualUntil = SysTickHl_GetMs() + holdMs;
}

void App_RgbLedPat_Tick(void)
{
    uint32_t now = SysTickHl_GetMs();

    /* L1 行程开关错误 (迁移自 App_Led.c AppLed_SwitchErrProcess):
     * 上行程错=黄闪, 下行程错=粉红闪, 两路同错=黄/粉红交替. */
    uint8_t err = App_Stepper_GetSwitchErr();
    if (err != 0u) {
        uint8_t phase = (uint8_t)((now / RGB_PAT_SWERR_MS) & 1u);   /* 0=前半亮 */
        if ((err & 0x01u) != 0u)      RgbLedHl_Set(phase ? C_YELLOW : 0u);
        else if ((err & 0x02u) != 0u) RgbLedHl_Set(phase ? C_PINK : 0u);
        else                          RgbLedHl_Set(phase ? C_PINK : C_YELLOW);
        return;
    }

    /* L2 一次性闪显: 优先于稳态, 窗口结束回落 */
    if (s_flashId != RGBFLASH_NONE) {
        uint32_t el = now - s_flashT0;
        if (el >= s_flash[s_flashId].totalMs) {
            s_flashId = RGBFLASH_NONE;
        } else {
            uint32_t ph = el % s_flash[s_flashId].periodMs;
            RgbLedHl_Set((ph < s_flash[s_flashId].onMs) ? s_flash[s_flashId].onMask : 0u);
            return;
        }
    }

    /* L3 稳态仲裁: 取最高优先级声明 */
    uint8_t best = RGBPAT_OFF;
    for (uint8_t i = 0u; i < RGBSRC_COUNT; i++) {
        if (s_claim[i] > best) best = s_claim[i];
    }
    if (best != RGBPAT_OFF) {
        if (s_pat[best].periodMs == 0u) {
            RgbLedHl_Set(s_pat[best].onMask);
        } else {
            uint32_t ph = now % s_pat[best].periodMs;
            RgbLedHl_Set((ph < s_pat[best].onMs) ? s_pat[best].onMask : 0u);
        }
        return;
    }

    /* L4 上位机手动设色: 超时自动回收 (业务灯语让位后重新可见直至到期) */
    if (s_manualMask != 0u) {
        if ((int32_t)(now - s_manualUntil) >= 0) {
            s_manualMask = 0u;
            s_manualUntil = 0u;
        } else {
            RgbLedHl_Set(s_manualMask);
            return;
        }
    }

    RgbLedHl_Set(0u);
}
