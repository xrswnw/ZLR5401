#include "stm32f10x.h"
#include "Boot_FaultStorm.h"
#include "Boot_SysTick_HL.h"
#include "Boot_Led.h"
#include "Boot_Iwdg_HL.h"
#include "Boot_CustomProtocol.h"   /* Proto_Poll */

/* =====================================================================
 * 实现: 计数器放在两域 .bss 之间的 RAM 空洞 —
 *   Boot .bss 止于 ~0x20002290 (链接期 _ebss, 启动零初始化不会触碰
 *   本地址); App .bss 覆盖 0x20000000~_ebss(>0x20002A00, App 启动
 *   零 .bss 时会清掉它 = "App 成功启动即清零" 的天然语义).
 *   选址约束 (改地址须复核两边 map):
 *     > Boot _ebss (Boot 复位不清) 且 < App _ebss (App 启动清)
 *   且远离栈 (两域栈顶均 0x2000C000, 深度余量充足).
 * ===================================================================== */

#define STORM_MAGIC_ADDR   0x20002A00u
#define STORM_COUNT_ADDR   0x20002A04u
#define STORM_MAGIC        0x53544F52u   /* 'STOR' */

static volatile uint32_t *magicAt(void) { return (volatile uint32_t *)STORM_MAGIC_ADDR; }
static volatile uint32_t *countAt(void) { return (volatile uint32_t *)STORM_COUNT_ADDR; }

/* 计数读取 (magic 无效视作 0, 顺带写入标记) */
uint32_t BootStorm_GetCount(void)
{
    if (*magicAt() != STORM_MAGIC) {
        *countAt() = 0u;
        *magicAt() = STORM_MAGIC;
        return 0u;
    }
    return *countAt();
}

void BootStorm_Clear(void)
{
    if (*magicAt() == STORM_MAGIC) *countAt() = 0u;
    else { *countAt() = 0u; *magicAt() = STORM_MAGIC; }
}

int BootStorm_Check(void)
{
    return (BootStorm_GetCount() >= BOOT_STORM_THRESHOLD) ? 1 : 0;
}

void BootStorm_OnFault(void)
{
    uint32_t n = BootStorm_GetCount() + 1u;
    *countAt() = n;

    if (n < BOOT_STORM_THRESHOLD) {
        NVIC_SystemReset();          /* 未达阈值: 复位自愈 (原 Round_098 修复) */
    }

    /* 达阈值: 不再复位 (复位只会循环), 常驻升级循环等主机 IAP 救援.
     * fault 上下文无法安全返回; SysTick 重启后协议响应的 50ms 上限
     * 依赖 ms 计时 (Round_098 BUG#5 教训), 必须先 Init. USB 若已在
     * 故障中损坏, 循环静默但不再风暴; IWDG 持续喂, 不会复位. */
    SysTickHl_Init();
    for (;;) {
        Proto_Poll();
        BootLedProcess();
        IwdgHl_Feed();
    }
}
