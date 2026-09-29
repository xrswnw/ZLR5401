# Round_016 — 0x09 进度帧 v3 单字节 statusBits 位图重设计 (2026-09-29)

需求背景: 客户复议 Round_014 的 0x09 v2 布局 (phase+motorPh+flags 三
字段 14 字节), 裁决简化为"一个字节讲清全部"的观测位图。经两轮方案
推演 (bit1 红外 / bit2 标签 / bit3-4 电机 / bit5 异常标签骨架 → 补
bit0 BUSY / bit6 ALARM / bit7 UHF, lastCycle 保留, 尾加 Rfu; 0x11 同构
status + AmTag + Rfu), 落地为统一 9 字节应答帧, 两通道帧长一致、槽位
复用、一份解析代码通吃。交互模型同步澄清: 拉模式 (0x09 一问一答,
节奏上位机定, 建议 100ms), 设备主动推的仅事件帧 0x0F/0x0B/0x0C/0x0E。

## A. statusBits 单字节位定义 (App_LockerUnlock.h UNLK_ST_*)

| 位 | 名称 | 0x10 语义 | 0x11 语义 |
|----|------|-----------|-----------|
| bit0 | BUSY | 流程进行中 (受理~终帧) | 同左 |
| bit1 | IR | 红外遮挡 (200ms 去抖; 仅升起门, 升后只报不控) | 恒 0 |
| bit2 | TAG | 名单存在 CONFIRMED 有效子集 | AM 消磁在线 |
| bit3-4 | MOTOR | 0 静·底 / 1 上行 / 2 在顶 / 3 下行 | 恒 0 |
| bit5 | FGN | 外来已判介入 (读到 2 次即置位) | 恒 0 |
| bit6 | ALARM | 报警中 (红 50/50 + 蜂鸣 50ms/500ms) | 恒 0 |
| bit7 | UHF | UHF 失联累计中 (5s 未恢复→终帧 4) | 恒 0 |

表达力优于相位枚举: 报警快降三因一眼可辨 — 0x79 介入 / 0x59 全失 /
0x1D 超窗正常回降; 速查值 0x00 空闲 / 0x01 等标 / 0x05 门差红外 /
0x0D 上行 / 0x15 在顶保持 / 0x75 在顶介入 / 0x81 失联中。

## B. 帧布局变更 (仅两帧, 其余全部不动)

1. **0x09 GET_PROGRESS v3 (9 字节)**: [cmd,err,status,tag,elapsed,
   riseCnt,alarmCnt,lastCycle,rsv]。tag: 0x10=CONFIRMED 张数 / 0x11=
   已解锁数; elapsed u8 秒 0xFF 封顶; rsv 恒 0。旧 v2 的
   phase/motorPh/flags 折叠进 status; total/bitmap 撤除 (终帧仍带
   最终 bitmap); holdMs(ms) 改 elapsed(s)。
2. **0x0F 受理推送**: 字节[2] phase→statusBits (0x10=0x01 /
   0x11=0x05 消磁在线), winMs 语义不变。
3. 0x10/0x11 命令帧、0x0B/0x0C/0x0E 事件帧、0x04、全部终帧及错误
   分支逐字节不动。

## C. 状态机卫生 + 缺陷修复

1. **GetProgress 重写** (App_LockerUnlock.c): 空闲全零 (status=0x00
   无歧义); 0x11 路径 s_amDemagFlag 判定后早退 (IR/MOTOR/FGN/ALARM/
   UHF 自然为零)。
2. **tag 字段缺陷修复**: 原拟用 s_confirmed — 其仅在
   TRACKING→CONFIRMED 跃迁拍递增, 已确认标签后续拍不重新计数, 不反映
   "当前在场"; 改用 s_validCnt (每拍重算的当前有效子集数)。
3. **am_run 跨通道残留清零补齐**: s_riseCnt/s_alarmCnt/s_fgnSeen/
   s_tagsFound 受理时清零 — 修复前一 0x10 跑出统计后接 0x11, 0x09
   上升计数/报警计数/lastCycle 泄漏旧值 (sim V3 系列断言覆盖)。
4. **防踩踏三锁复核维持**: 单腿串行 (LockerSeek_RunRetry 阻塞至腿
   完成, 同一时刻仅一腿) / 回降免疫 (s_immune=1 期间 CANCEL 不打断
   回降腿, epc2_alarm_lower + lower_if_risen + motor_fail 三处) /
   门禁 (busy 白名单仅 0x04/0x09, Round_015 裁决)。本轮位图仅改观测
   面, 不触电机控制路径。

## D. 模拟测试 (Agent/Round_016/test/)

Round_015 套件移植 + v3 增量:
- P1 改位图断言: BUSY/TAG/MOTOR=2 在顶/FGN+ALARM 介入/子集回稳/
  riseCnt/lastCycle 全时点核对; tag 语义 (s_validCnt) 验证。
- **V3 新增**: 空闲 0x09 全零 / 0x10 0x0F [2]=0x01+winMs 三字节 /
  0x11 0x0F [2]=0x05 / 0x11 计数中 status=0x05·tag=已解锁·elapsed /
  0x11 统计不泄漏 0x10 riseCnt/alarmCnt/lastCycle。
- E1~E9/A1~A6/M1 全部保留回归 (Round_014/015 行为)。
- 结果: **132 checks 0 failures**; ASAN/UBSAN 干净; frame_check.py
  ALL OK (F5: 9 字节位图解码 + 速查值算术校验 + 受理帧 status);
  static_check.sh 13 项全绿 (v2 UNLK_FLG_ 宏残留检查: grep 全空)。
- 固件 build_app.sh 编译通过。

## E. 协议文档 V1.6

generate_pptx_v3.js: 0x09 页重写为 v3 九字节 + 测试帧 (真实 CRC); 新增
statusBits 位定义+速查值页 (左位表右值表); 0x10/0x11 流程图受理行
phase→status; 0x11 章进度句改 status=0x05·tag=已解锁数; 0x04 页
门禁描述修正; 版本历史/封底 V1.6。WPS 导出 PDF 34 页 494490B;
pptx XML 复核: motorPh/holdMs/phase=7/v2 布局/V1.5 于 全部清除,
statusBits/0x79/0x59/0x1D 全部就位; judge 视觉验收 6 关键页 6/6 PASS。
