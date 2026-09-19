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

## 微调 — 0x0A 默认盘点时限 1000→500ms (2026-09-03)

需求重申 (业务追加点1: "EPC校验时默认盘点500Ms"): tmoMs=0 缺省由
1000ms 改 500ms (UNLK_INVENTORY_TMO_MS), 协议文档 13.2 同步 (0→500)。
仅编译回归 (App_202609032336.hex), 台架 P6/P9 待跑。
追加点1 其余各条 (逐张稳定确认推 0x0B 带信息 / 失配 0x0C / 流程结束
关 AM) 经核对与现实现一致; 电机/AM 时序两处有意偏差 (消磁待软标段才开、
磁块硬标段末即回降) 待用户裁决是否改按字面 (电机升起同时开消磁、流程
结束才回降)。流程结束 AM 恢复"仅检测"为 void 直发无复核, 若该帧丢失
AM 将滞留检测消磁模式 (静置自消磁可疑根因之一), 加固待裁决。

## Round_014 — 0x0A 流程五项裁决落地 (2026-09-03)

用户五项裁决 (对照当日早前"待裁决"清单全部闭卷):
1. 流程结束 AM 固定"仅检测", 不恢复流程前预设 (维持原实现);
2. 稳定判据改 1.5s: UNLK_STABLE_CONFIRM_MS 3000→1500;
3. 外来标签同样按稳定判据逐张上报: 废除"外来独占连续 3 轮"事件
   结算 (UNLK_MISMATCH_CONFIRM_ROUNDS 删除), 新增外来标签独立缓存
   (UNLK_MAX_FGN_TAGS=4, FgnTagRec_t 多标签缓存, 满后只计数不上报),
   稳定一张推一帧 0x0C (尾字节 rounds→hits=稳定期命中次数, 单向
   掩码不重报, 离场超窗释放缓存槽);
4. 电机至上行程开关同时开 AM 消磁 (softCnt>0, 首确认升起时切,
   失败软标入口再试一次, 再败 err=10), 磁块保持升起至整个流程
   结束才统一回降 (lower_if_risen 公共出口, CANCEL/AM_LINK/软标段
   全部路径复用), AM 出口统一切回仅检测;
5. 软标窗 5min 满未校验完成按超时失败结账: 新增 endReason=7
   UNLK_END_SOFT_TIMEOUT (终帧布局不变, 蜂鸣/绿三连闪不发, 红双闪)。
另: 0x0A 默认盘点时限 1000→500ms (当日早前已改, 见上节)。

| 位置 | 变更 |
|------|------|
| App_LockerUnlock.h | 判据 1.5s; UNLK_END_SOFT_TIMEOUT=7; UNLK_MAX_FGN_TAGS=4; 流程注释 ⑸~⑻ 重写 |
| App_LockerUnlock.c | FgnTagRec_t 外来缓存+fgn_track; 失配改逐张稳定确认; 升起同时切消磁+s_demagBase 提前至首确认; lower_if_risen 统一回降出口; ⑹⑺⑸ CANCEL/AM_LINK 出口重排; 软标超时失败结账 |
| App_CustomProtocol.h | 0x0A 注释: 0x0C 语义/hits, endReason 1/2/4/6/7 列表 |
| Protocol/App_Protocol.html | 13.1 业务闭环/流程 ⑸~⑻, 0x0C/0x0D 推送表, endReason note+7, err=10 说明, 图 A/B 重排 (SOFT 前置于 LOWER), 灯语红双闪含软标超时 |

仅编译回归 (App_202609032344.hex); 台架 P6 (0x0A 全路径) /
P9 待跑 — 行为变更点: 失配帧推送时机/频度、磁块保持时长、软标
段超时终态、消磁起始时刻, 需重点复测。

### Round_014 台架回归终态 (2026-09-03, 固件 App_202609032344.hex)

烧录实测全绿: proto_smoke 22/0 (新码 0x20~0x26 全通道 + 旧码拒绝) ·
unlock_multi 20/0 含新增 M9b 三连 (软标段中途 IO_DIAG keyUp=0 压上行程
开关实证磁块保持升起[裁决4]; 5min 窗满 endReason=7 SOFT_TIMEOUT 无
0x0E[裁决5]; 流程后 AM GET_PARAM 真读回 mode=1 仅检测[裁决1]) ·
确认帧判据耗时实测 1500ms 整 (裁决2 门限精确生效) · M5 外来标签稳定
一张一帧 0x0C (裁决3) · p6_locker 49/0 (D4a 短窗改 1200ms<1.5s 门限) ·
p1 15/0 · p2 16/0 · p5_am 16/0 · p9_optfix 30/0 (冷启动)。
测试基建同步: zlr/const.py + lib.py FC 码 0x20~0x26 (lib FC_SELFTEST
0x0F→0x25); p9 IO_DIAG 0x10→0x26; unlock_multi M6e 改 11B holdMs3B 布局;
新增 M9b (--soft-timeout); p6 D4a/D5 门限适配 1.5s。
文档修正: App_CustomProtocol.h FC_IO_DIAG 注释 keyUp/keyDown 极性
原误标 "1=触发", 实为低有效 0=压到 (与 LockerSeek 判据/协议文档一致)。
已知非阻塞项: p9 O1b 对 MT_STATE_DONE 残留敏感 (上轮 O3 行程测试的
DONE 态跨运行携带, 冷启动 30/0; MotorTest 模块本次未改动);
M2 双真标/M3 逐张放取/软标真实解码 0x0E 仍需双标签/软标台架。

### Round_014 台架补测 — 双真标 + 真实软标闭环 (2026-09-03)

台架补上 2 硬标 + 1 软标, 前日三项待跑闭环全部完成:

- **M2 双真标全流程**: 2 EPC (…eb7dbcdb + …eb7e7507) 同时在场,
  0x0A m=2 → end=ALL_OK, confirmed=2/2, bitmap=0x03, 确认 seq=[1,2],
  7.6s 收束 (unlock_multi 18/0 — 原 4 SKIP 项中 M2 转正)。
- **M5 多外来缓存实证** (意外收获): m=1 假EPC 双真标在场 →
  **2×0x0C 各带独立真实 EPC**, 一张一帧 (裁决3 多标签缓存逐张上报实证,
  原单标台架只能见 1 帧)。
- **M9c 真实软标解码闭环** (新增 soft_real.py): 0x0A m=1 softCnt=1,
  真软标在 AM 区 → 终帧 err=0 + ≥1×0x0E 解码帧收讫; 事后 AM
  GET_STATUS deactCnt 0→1 / deact=1(消磁成功) / failCnt=0,
  AM mode 回 1 仅检测 (裁决1), locker/motor 双 IDLE。
  对照 M9b (无软标 5min→SOFT_TIMEOUT): 真实软标在窗内即解码即结账,
  与软标段 AM 才开始消磁 + 首帧解码即完成的设计闭环自洽 (裁决4/5)。
  注: 该次运行 soft_real.py 打印段索引 bug (元组下标误写) 在证据
  采集后崩溃, 已修复; 软标一次性已消费, 终帧字段以事后状态佐证。

至此 0x0A 六步业务闭环 (付款→受理→比对→升降+消磁→软标→结账)
全路径真实台架验证完毕。仍需人工: M8 蜂鸣听感 / M7-补充 升降中
打断 / M3 逐张放取时序。

## Round_015 — 0x0A 全确认门控 + epcCnt=0 纯软标通道 (2026-09-04)

用户两项行为变更裁决 (2026-09-03 深夜):
1. **全确认门控**: 所有 EPC 均校验通过 (n==m) 才动电机 — 原首确认
   即升起改为循环收齐后才升起; PARTIAL/UHF_LOST 出口磁块全程不动
   (不升不降不消磁, 不进软标段) — 部分确认不再解锁/消磁 (防盗语义)。
2. **epcCnt=0 纯软标**: 原入口 PARAM 拒绝 → 合法 (需 softCnt>0, =0
   仍 PARAM): 跳过光电门控与 EPC 校验, 受理帧 phase=5 SOFT/
   win=300000, 受理即计时, 直接 升起+开消磁+软解码 (无硬标结账场景)。

| 位置 | 变更 |
|------|------|
| App_LockerUnlock.c | push_start 增 phase 参; 升起块自确认处理内移至循环后 (hardEnd==ALL_OK 门控); epcCnt=0 分支: 跳 ⑵⑷⑸ (win=软标窗, s_irMs=受理时刻, hardEnd 预置 ALL_OK); ⑺ 软标段仅 ALL_OK 进入; UHF Stop/盘点仅 epcCnt>0 |
| App_Dispatch.c | 0x0A 校验: epcCnt=0 需 softCnt>0; epcLen/帧长校验仅 epcCnt>0 时要求 |
| App_LockerUnlock.h | 契约注释 ⑴⑵⑸⑹⑺ 重写 (纯软标通道 + 全确认门控); UNLK_PH_RISE 注释 |
| App_CustomProtocol.h | 0x0A 注释: epcCnt=0 语义 + 全确认门控 + 0x0F phase 变体 |
| Protocol/App_Protocol.html | 13.1 业务步骤 4/5 + 请求段 epcCnt 0~4 + W 段 + 流程 ⑴⑵⑵⑸⑹⑺ + 0x0F/0x0D 推送行 + 13.2 err=0/err=2 + endReason note + GET_PROGRESS holdMs + 图 A/B (升起移至 n==m 后, 纯软标旁路) |
| 测试 | unlock_multi: P1a 更名(epcCnt=0且softCnt=0), M4 断言强化 rise/lower==0, 新增 M10a~d (纯软标受理/升起/CANCEL/AM回1) + M10e (--soft-timeout 5min SOFT_TIMEOUT); soft_real.py end 断言收紧为 ==1 |

### Round_015 台架回归 (固件 2026-09-04 00:40 编译烧录)

- unlock_multi 23 记录: P1×5 ✓ · M1 ✓ (单标全确认后升, 判据 1500ms 整,
  ALL_OK rise/lower=4321/4321) · **M4 ✓ (PARTIAL 1/2 确认 rise/lower=0/0
  — 全确认门控实证: 未全过磁块不动)** · M5 ✓ · **M10a~d ✓ (纯软标:
  受理 phase=5 win=300000, keyUp=0 升起中, CANCEL→ABORTED
  rise/lower>0, 0x0D[end=1,0,0,0], AM mode 回 1)** · M6×6/M7/R1 ✓。
- p1 15/0 · p2 16/0 (SELFTEST 全过) · p5_am 16/0 · proto_smoke 21/1
  (T3c 单轮盘点 0 标签 — 台架硬标已被取走, 非链路故障) ·
  p6 40/9 + p9 29/1 (B2~B7/O6c/D4/D5 均需真标在场, 属台架条件;
  D4 终帧 0a0002·0000·0001·rise=0·lower=0 恰为全确认门控 0 确认
  不动磁块的直接实证) · **M10e ✓ (纯软标 5min 窗满: 305.7s ->
  end=SOFT_TIMEOUT, rise/lower=4321/4321, softDone=0/1, 裁决5)**。
- M2 双真标在新门控下未复测 (回归中途硬标被取离读区; 新旧门控对
  M2 终帧无差异 — 全确认时升起点后移, ALL_OK/bitmap/seq 不变)。
  台上恢复双标后可一键复测。
