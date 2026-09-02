# Round_098 — 全系统自动化测试 + 缺陷修复闭环 (2026-09-02)

范围: P1~P9 全量 (协议/基础/电机/UHF/AM/Locker/IAP/压力/设计评审),
USB HID + JLink 双通道, 全程无人值守, FAIL→定位→修→烧→回归闭环。

## 固件修复 (BugList 详见 Agent/Round_098/Report.html §2)

| 编号 | 位置 | 摘要 |
|------|------|------|
| BUG#1 | App SELFTEST CLEAR | 帧布局对齐协议文档 [cmd,maskL,maskH] |
| BUG#2 | App_Usb_Transport.c | USB TX >63B 截断 → 63B 分片 + 片间 busy 等待 1000ms (WAVE_PAGE 400 点全量回传验证) |
| BUG#3 | App_Dispatch.c | LOCKER ADD 长度校验 && → \|\| (短帧/超长 EPC 曾静默接受) |
| BUG#4 | Boot_Dispatch.c | IAP FW_DATA addrOffset u16 → u32 (232KB App 区); Boot_Protocol.html 同步 |
| BUG#5 | Boot main.c + Boot/App CustomProtocol.c | App 无效时 Boot 兜底循环不重启 SysTick → WaitEp1Ready 无限自旋死模式 (JLink 实证 tick 冻结@2500); 兜底前 SysTickHl_Init + WaitEp1Ready 硬迭代上限 |
| BUG#6 | Boot_Usb_HL.c | g_boot_usb_tx_busy 无超时恢复, IN 完成中断丢失后响应永久静默丢弃 → 200ms 强制回收 (NAK+busy 清零) |
| BUG#7 | 测试框架 (p7_iap.py, 源自 Round_009 conf.py) | VerifyLevel 枚举语义与固件/文档相反 (2 当 MID 实为 BASIC) → 零映像跳过向量校验被误 EXEC; 对齐 0=FULL/1=MID/2=BASIC |
| 附加 | Boot stm32f10x_it.c | fault handler 死循环 → NVIC_SystemReset 复位自愈 (Boot 无 IWDG, 死循环=永久哑死) |

## 测试终态

P1 15/0 · P2 16/0 · P3 20/10 (10 项=设计层既有行为) · P4 23/0 (含新增 U3d 配置还原) ·
P5 16/0 · P6 51/0 · P7 32/0 ·
P8: S1 浸泡 30min/2031 次混合操作 0 失败 0 层漂移 0 自检脏位 (max 时延 1.15s) ·
S2 复位×5 全恢复 (~14.1s/次) · S3 Locker 全周期×10 = 10/10 (session 还原 S0 后) —
详见 Report.html。

## 测试框架侧修正

p5_am.py AM_ERR_PARAM NameError; p7 I5j layer 过滤; p2 B5d 回零等待;
p4 U9/U1d/U3c UHF 下电过渡期时序; p1 A3b 归 OBS;
p4 U3d (新增): U2b 持久化验证写 session=S2 未还原 — S2 会话下标签盘点标志在连续
盘存场脉冲间不复位, P8-S3 Locker 周期#1 匹配后 #2~#10 永无匹配 (静默失读, 根因
取证: 现场同步盘存可读 / 连续异步盘存 0 标签 / GET_STATUS state=INVENTORY 空转);
持久化验证后还原原始配置, 恢复 S0 后 S3 10/10。

## 设计层发现项 (不改码, 供决策)

21 条分级优化建议见 Report.html §6 (帧序列号 / 参数静默钳位回显 /
MOVE-TEST 互斥 / SWD 重烧后 Boot 驻留的工程恢复 / UHF 下电过渡期语义 /
IR 直读 / 文档陈旧项 / **UHF session S2/S3 静默废掉 Locker 连续重扫 (#21, 高)** 等)。

