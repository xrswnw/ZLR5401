# Round_014 — 0x10/0x11 解锁状态机重设计 + 电机提速 2000 + 宿主机模拟全绿 (2026-09-29)

需求: 客户联调后 0x10 EPC 解锁按名单组合语义重设计 (任意非空子集在
场即有效, 外来介入报警), 0x11 恢复 AM 消磁控制 (受理切消磁、逐帧即时
计数、达标/超窗挂起); 无真机, 模拟通过后直接交客户实测。

## A. 用户裁决两项 (AskUserQuestion)

1. **电机提速 = 全部电机腿统一 2000 µ步/s** (≈300RPM, 行程 8170 步
   ≈4.1s; 2000 > STEP_RAMP_MIN_HZ 1000, 起步重新进斜坡段): LSEEK /
   HOMING / MT_TEST 三处常量 1000→2000。否决了"仅解锁升降腿"的推荐案。
   实测判读项: 回零失步风险 (黑匣子 TRACE 首验) + 斜坡起步噪声。
2. **异常报警节奏 = 红灯 50ms 亮 / 50ms 灭闪 + 每 500ms 同步 50ms
   蜂鸣, 持续到异常解除, 状态帧同步置报警位** (否决纯灯闪无蜂鸣案)。

## B. 0x10 EPC 解锁重设计 (App_LockerUnlock.c EpcRun 全重写)

新帧格式 (破坏性变更): `[0x10, tmoL,tmoH, epcCnt, (epcLen,EPC…)×n,
rsv…]` — tmo=u16 秒流程窗 (>0, 上限 600 钳位), epcCnt 1~10, 每张
len 1~12 变长, 名单内重复 EPC 拒绝 (PARAM)。

标签模型 (10 槽 s_tag + 4 槽外来):
- **稳定在场**: 首读 (PENDING→TRACKING) 起 1500ms 内无 1500ms 级
  断读 → CONFIRMED (EPC_STABLE_MS/ABSENT_MS 1500, 容忍 UHF 漏读,
  取代旧 10 连读计数判据); 断读超 1500ms 回 PENDING (非单向)。
- **外来介入**: 非名单 EPC 累计读到 2 次 (EPC_FGN_MIN_HITS, 介入要
  快, 不等 1.5s); 外来 1500ms 未再读 = 稳定消失 (槽释放, 槽释放后
  再次介入重报 0x0C)。
- **有效组合**: 名单任意非空子集 CONFIRMED 即有效 (A/B/C/AB/AC/
  BC/ABC 全部有效, 客户确认)。

状态机 (WAIT → RISING → UP 两态主循环, 防误踩踏核心):
- WAIT: 有效子集 + IR 遮挡 (200ms 去抖) 且无介入 → 升起 (2000Hz
  RunRetry)。**IR 仅作 WAIT 升起门控** — 在顶以标签为准, 不因光电
  遮不全误降 (客户实测痛点)。
- UP: **在场即保持** (废除固定 holdTop 时长), 盘点监守不停。
- 异常 = 外来介入 或 (升/在顶期名单全失): RGBPAT_ALARM_BLINK 红
  50/50 闪 (最高优先级稳态) + 蜂鸣 50ms/500ms + **快速下行** (免疫
  CANCEL), 回 WAIT; 外来稳定消失且子集恢复 → 经红外门再升, 循环至
  超窗。alarmNow 判定在 WAIT 期不触发回降 (未升起无踏踏风险)。
- 超窗: 在顶 → 回降 + ALL_OK + 蜂鸣 1s/绿三闪; 未在顶 (未升/异常
  回降未恢复) → TIMEOUT 黄慢闪。CANCEL: 安全回降 + 全状态复原 +
  ABORTED。UHF 失联 5s → UHF_LOST。

**关键 bug (模拟器抓到)**: 升起/回降腿期间 RunRetry 阻塞在自身 Pump
循环, 主循环盘点接力不执行 → 标签断读 1500ms 误判全失 → 监守误停机。
修复: 盘点接力移入 `epc2_tag_update()` 尾部 (每泵拍路径都调用),
删除主循环末尾接力。教训: 阻塞腿内的 UHF 连续性必须由每拍公共路径
保证。

终帧: `[0x10,err,endReason,bitmap,rise(2),lower(2),elapsed(2秒),
riseCnt,alarmCnt,fgnSeen]`。推送: 0x0F 受理 (winMs=tmo×1000) /
0x0B 每次进入稳定态一帧 / 0x0C 外来介入每事件一帧。

## C. 0x11 恢复 AM 控制 (AmRun)

- 受理 → SET_MODE(mode0 消磁) 回显确认 (重试 3, 全败尽力复原 +
  AM_LINK + 红双闪, 不推 0x0F); → tmo 秒计数窗 **逐帧即时入账**
  (App_AM_GetEventCount, 每条 cmd17 帧一计 — 客户裁决 "收到一个
  消磁帧立马更新状态, 无需等待无需稳定, 它只有一次", 废除 3s 突发
  静默结算与窗末宽限), 每计推 0x0E + 白单闪; → 达标/超窗 →
  SET_MODE(mode1 仅检测) 挂起确认 (**失败不阻塞, amFail=1 终帧如实
  上报** — 解码器自行恢复消磁风险无法根除, 客户知情) → 蜂鸣 1s +
  绿三闪。rise/lower 恒 0 (不动电机裁决维持)。
- 终帧 deactFail 字段重义: 1 = 挂起确认失败。amCnt=0 直通不碰 AM。

## D. 0x09 GET_PROGRESS v2 (0x10/0x11 统一 14 字节)

`[sub,err,phase,holdMs(3 LE),total,confirmed,bitmap,softCnt,softDone,
lastCycle,motorPh,flags]`
- 0x10: softCnt=升至顶次数, softDone=报警次数, lastCycle=0无/1升至
  顶/2全失回降/3介入回降, motorPh=UNLK_MPH (0静/1上行/2在顶/3下行),
  flags=bit0 有效子集稳定 bit1 介入 bit2 报警 bit3 红外遮挡。
- 0x11: confirmed/softDone=已解锁数 (逐帧即时), motorPh 恒 0,
  flags 仅 bit4 DEMAG_ON。

## E. 灯语/蜂鸣增补 (App_RgbLed_Pattern)

- RGBPAT_ALARM_BLINK=9 (红 100ms 周期 50/50), Set 上限检查同步放宽。
- 蜂鸣: EPC_ALARM_BEEP_MS 50 / EPC_ALARM_BEEP_PERIOD_MS 500,
  epc2_alarm_poll 每拍调度; 流程末 ALL_OK 蜂鸣 1s (EPC_BEEP_FLOW_DONE_MS)。

## F. 宿主机 C 模拟器 (Agent/Round_014/test/sim/, 无真机交付门)

编译**真实 App_LockerUnlock.c** + mock 桩 (sim_env.h/sim_stubs.c/
sim_main.c/run_sim.sh): Pump 虚拟时钟 20ms/拍, UHF 100ms 轮注入
在场标签, 电机 2000Hz (40 步/拍, 触点 8170/0), AM 帧时间驱动, 钩子
sim_at/sim_progress_at, 日志 frames/beeps/leds/legs/amModes/samples。

场景 89 项断言全绿: E1 单标全程 / E2 在顶介入回降再升 (D 窗口定在
在顶期) / E2b 升起途中介入即停 / E3 全失回降再升 / E4 等待期介入不
升 / E5 CANCEL / E6 UHF 失联 / E7 参数校验 (epcCnt 0/11、tmo=0、
重复 EPC、长度不足) / A1-A5 AM (逐帧达标/超窗/取消/链路失败/amCnt0)
/ P1 0x09 采样实时性 / M1 2000Hz 时序预算 (行程 4.1s < 10s 超时)。

调试期问题 (记录教训):
- 剧本结构 bug: add_tag 在 reset_scenario 后被清 → 每场景显式
  reset→布剧本→Run; g_script memset 后 nTags 计数缺失 → 辅助函数
  内联计数。
- ASan global-buffer-overflow: g_leds[256] 每拍记录溢出覆盖相邻全局
  → 日志数组扩容 + 全部写入边界保护。
- elapsedMs=0 边界: 受理时刻 g_ms 恰 0 → sim 起点 g_ms=1000。
- 桩签名按真实头文件逐一对齐 (IsReady int/BeepPulse uint32_t 等)。

## G. 帧级验证 + 真机冒烟脚本

- Agent/Round_014/test/frame_check.py: 复用 Round_008 zlr 库, 新
  0x10 变长帧装配 / CRC 往返 / 边界 → ALL OK。
- Agent/Round_014/test/unlock_smoke_v2.py: 真机四模式冒烟
  (epc/epc_cancel/am/progress), 0x09 v2 解析含 motorPh/flags 变化
  打印, 终帧新字段解析。**客户实测入口脚本**。

## H. 协议文档 V1.4 (generate_pptx_v3.js → pptx → WPS PDF)

33 页: 版本页 V1.4+变更摘要; 0x09 v2 布局; 0x10 新帧表+组合有效+
报警行为+终帧; 0x11 恢复 AM 控制+挂起不确定注记; 错误码/结束原因双表
(err10 语义更新, endReason2 标注不再触发); EPC/AM 双流程图重画 (EPC
含报警分支, AM 含切模式/挂起确认); 灯语表加红 50/50 报警行; 封面/
封底 V1.4。例帧 8 帧全新 (0x10 变长 req/rsp、0x09 v2、0x11 新终帧、
0x0B/0x0C/0x0F 推送), CRC 经 zlr 库重算。

布局修复两轮: 0x10/0x11 参数页测试帧栈溢出页底 → desc/exps 精简;
judge 视觉门抓 p1 封面底标语截断 + p30 流程图底注截断 → 复检双 PASS
(31/33→33/33)。

**交付物**: Protocol/App_Protocol_V1.4.pptx / App_Protocol_V1.4.pdf
(485690 B, 33 页, WPS 输出为PDF)。

## 待办

- 烧录回归: `./build_all.sh` 已过 (待用户 USB), 真机用
  unlock_smoke_v2.py 四模式冒烟; 2000Hz 回零失步 + 斜坡起步噪声由
  motor_trace.py --watch 首验。
- 客户实测判读项: 外来介入报警节奏 (50/50+蜂鸣) 现场接受度;
  AM 挂起失败 deactFail=1 是否出现 (解码器自恢复)。
