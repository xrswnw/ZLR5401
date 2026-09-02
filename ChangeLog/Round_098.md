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


## 修正批 2 — §6 优化项实现 (2026-09-02 第二批, 用户裁决 #3/4/7/8/9/10/11/12/15/20/21)

| 项 | 位置 | 实现 |
|----|------|------|
| #3 | App_Dispatch.c / App_MotorTest | MOVE/TEST/后台回零在途时再下发一律 MOTOR_ERR_BUSY, 不替换在途运动 |
| #4 | App_MotorTest.h/.c + QUERY | 单程超时 90s→10s; 新增停转宽限 300ms→FAULT(mtReason=5); 完成结果经 QUERY testState+diag1 取走, 完成后可立即再 TEST |
| #7 | Boot main/BootStorm (新) | RAM 风暴计数 @0x20002A00/0x2A04 (Boot _ebss 不清区): fault 复位 <3 次正常 2.5s 窗口, ≥3 常驻升级循环; JumpToApp/EXEC/EXIT_BOOT 清零 |
| #8 | App_Dispatch.c | 新增 FC_IO_DIAG(0x10): 10B [result, ir, keyUp, keyDown, uhfPowered, antennaOk, amLink, homing(0-3), locker, testState] |
| #9 | App 启动 (POST) | UHF 配置恢复先于 USB 就绪: POST 期自动上电+下发持久化配置, 枚举即可用即 READY |
| #10 | App_AM | 链路断线自动探测: 突发帧静默判掉线, 0x63 探测退避 2s/4s/8s, 自动回链 |
| #11 | App 启动 + MOVE/TEST/LOCKER 门控 | 回零后台化: USB 就绪 ~7.2s / 回零 ~7.3-12s, 期间运动类命令 BUSY, IO_DIAG homing 可观测 |
| #12 | Protocol/App_Protocol.html + Boot_Protocol.html | 全量刷新: 15 命令表+0x10; MOVE BUSY/TEST 完成语义/QUERY 重映射; UHF QUERY NOT_READY; SET_CONFIG 持久化+开机恢复; ONE_SHOT irWait/hold 拆分; 新增 §16 FC_IO_DIAG; Boot 7× 40KB→232KB + 启动伪码 + 风暴保护节 |
| #15 | App_Dispatch.c | UHF CLOSE 后 QUERY 返回 NOT_READY (原 LINK) |
| #20 | App_LockerOneShot + 协议 | ONE_SHOT 请求拆 irWaitMs/holdMs (等待窗/保持窗独立), 响应含 demagCnt |
| #21 | App_UHF.c ScanSession + App_Dispatch.c | Locker START 强制扫描会话 S0 (保存→切换→结束还原); ScanSessionBegin apply 3 次重试×300ms (模块中止后 ~700ms 忙窗) |
| 新BUG-1 | App_UHF.c | ScanSessionBegin 无重试: 模块忙窗内 apply 失败即 S0 切换失败 → 匹配防抖饿死 (S2 周期#2 失败根因) |
| 新BUG-2 | App_Dispatch.c | SET_CONFIG 仅在 OK 时持久化: LINK 失败配置已生效但不落盘, 复位回退 → 除 PARAM 错外均持久化 |

## 修正批 2 回归终态

P2 16/0 · P3 26/0 (+4 OBS 钳位语义, 既有) · P4 23/0 · P5 16/0 · P6 51/0 ·
P8-S3 10/10 (session=S2 连续 2 周期亦过) · P9-optfix 30/0 (新增: 互斥/完成/IO_DIAG/
QUERY 重映射/OneShot 布局/S2 会话/回零时序) · P9b-storm 9/0 (JLink RAM 注入,
计数=3 锁定+EXIT_BOOT 救援+计数=2 不锁)。
固件 ZLR5401_202609022300.hex, App text 77120B, RAM 风暴孔约束核验通过。

## Round_011 — RGB 灯带灯语方案 A "五色叙事" (2026-09-02)

需求: Agent/Round_011/Demand.md (灯带 8 色状态演示, 普通人视角直观简洁)。

| 位置 | 变更 |
|------|------|
| App_RgbLed_Pattern.h/.c | SCAN_WAIT(青慢闪) 拆为 IR_WAIT(白慢闪, 等放标) + SCAN_ACTIVE(蓝慢闪, 盘点中); 新增闪显 TAG_SEEN(蓝单闪, 读到一张标签) / WARN_2S(黄慢闪 2s, NO_IR/NO_TAG/窗满); SOFT_FAIL 更名 FAIL_DOUBLE(消磁失败+链路断); 全部含无蓝降级 (白→黄/蓝→绿/蓝闪→黄闪) |
| App_LockerOneShot.c / App_LockerUnlock.c / App_Locker.c | 三主线调用点改色: 等放标白慢闪 / 盘点蓝慢闪 / 每解码一张蓝单闪 (命中绿单闪随后覆盖); 全部链路断出口 (UHF_OPEN/UHF_LINK/AM_LINK/UHF_LOST) 红双闪; NO_IR/NO_TAG/PARTIAL_TIMEOUT 黄慢闪 2s |
| Protocol/App_Protocol.html | 灯语 note 更新为方案 A; ONE_SHOT 流程逐阶段 [灯] 标注; 硬件层净化 32 处 (去引脚/器件型号/总线/寄存器表述, 仅协议层语义) |
| Protocol/App_Unlock_Flow.html (新) | 解锁全流程详解: 就绪时序 + 三主线逐阶段 (协议交互+灯语+蜂鸣+推送帧) + 失败→错误码→灯语总表 + 灯语时序图; 全文协议层视角 |

纯灯语层变更: 仲裁器架构/协议帧/业务判定零改动。回归 P2 16/0 · P3 26/0 ·
P4 23/0 · P5 16/0 · P6 51/0 · P9 30/0。固件 ZLR5401_202609022328.hex。
灯语肉眼辨识度留人工验收 (Agent/Round_011/Report.html §5)。

## Round_011 续 — 解锁通道统一: 废除 0x08 ONE_SHOT, 单标=0x0A epcCnt=1 (2026-09-02)

用户裁决 (业务六步流程定稿): 硬标段预留解锁窗 W=2min+30s/额外标签,
逐张比对回传 (0x0B 确认/0x0C 失配), n==m 即硬标段结束进软标,
每个软标一次解码、用完即结账完成 — 与 UNLOCK_MULTI (0x0A) 既有语义
一一对应; 单标流程与之重复且 "保持期 3s 稳定即结账" 判据与
"逐张确认 + n==m" 不一致, 整体废除, 单标=0x0A epcCnt=1 (W=120000)。

| 位置 | 变更 |
|------|------|
| App_LockerOneShot.c/.h (删) | 模块整体移除 (含 CMake 源表条目); 灯语槽位 RGBSRC_ONESHOT→RGBSRC_UNLOCK |
| App_Dispatch.c | 0x08 分支改废弃应答 (保留码位, 一律回 [08,2] PARAM 区别未知子命令); MOTOR/UHF/AM/LOCKER/RGB/SELFTEST 六处互斥检查去 OneShot; CANCEL 分支去 0x08 打断路径 |
| App_LockerUnlock.c/.h | 去对 0x08 的双向互斥检查; GET_PROGRESS (0x09) 统一多标签布局 [sub,err,phase,holdMs(3),total,confirmed,bitmap,softCnt,softDone] (空闲 phase=0 同布局) — holdMs 3 字节修复 W>65.5s 时 16bit 回填截断 (潜在溢出 bug 一并消除) |
| App_CustomProtocol.h | 0x08 定义改废弃注释; GET_PROGRESS 定义改唯一布局 (3 字节 holdMs) |
| 测试 p6/p9/p0 | p6 Part C 重写 (0x08 废弃×2 + 0x0A epcCnt=1 单标全流程 + CANCEL 打断); p9 O5 改三形态帧均 PARAM; p0 光电探针改走 0x0A (原 0x08 帧长度误判已修: EPC 需 12 字节, 加 drain 防推送帧污染后续用例) |
| Protocol 两文档 | App_Protocol §13 重写: 13.1=0x08 废除声明 (迁移指引), 13.2=0x0A 唯一流程 (含业务六步闭环), 13.3=终帧; 错误码表/附录 C 常量/脚注同步; App_Unlock_Flow 全文统一 (流程一=0x0A 单标多标同流程, 原 0x08 章节改迁移指引+语义存档) |

回归终态: P1 15/0 · P2 16/0 (B5c 自检读取竞态 flaky, 复跑通过) ·
P3 26/0 · P4 22/0 · P5 16/0 · P9 28/2 (O5 废弃×3 全过; O6c 2 失败=标签
不在场, UHF 独立探针 NO_TAG 证实) · P6 40/9 (新增路径全绿: 0x08→PARAM×2/
单标 0x0A epcCnt=1 短窗 PARTIAL+长窗打断 ABORTED/GET_PROGRESS 唯一布局;
9 失败=B2~B7 真标签匹配链 + D4/D5 长窗确认, 均标签不在场所致,
与 0x08 移除无因果 — UHF INVENTORY 探针 err=5 NO_TAG 证实)。
固件 ZLR5401_202609030003.hex。
