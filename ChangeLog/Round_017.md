# Round_017 — 老清单业务链整批废除 (2026-09-29)

需求背景: 用户裁决 "老清单业务彻底不回归, 现在只有 0x10/0x11"。
0x23 老清单链 (0x01 CONFIGURE / 0x02 ADD / 0x03 START / 0x05 QUERY /
0x06 CONSUME_SOFT / 0x07 GET_EVENT) 及其载体模块 App_Locker 整批关闭,
处置先例同 0x08 (Round_011) / 0x0A (Round_013): 码位显式回 err=2 PARAM,
实现保留 #if 0 备恢复。

## A. 固件变更

1. **Dispatch (App_Dispatch.c)**: 六 case 实现 #if 0 封存, 码位合并
   fallthrough 显式回 [sub, 2]; CANCEL (0x04) 改为仅服务解锁流程 —
   busy 时 Abort, 空闲时无操作直回 OK (原 App_Locker_Cancel 分支撤除)。
2. **互斥职能移交**: App_LockerUnlock 三处 `App_Locker_IsIdle()` 前置
   检查移除 (unlk_run/epc_run/am_run) — 业务互斥由 s_busy + Dispatch
   门禁独立承担; include 撤除。
3. **fill_busy / IO_DIAG**: 终帧 BUSY 诊断字节 lockerState 恒 0
   (布局不变, 上位机兼容); IO_DIAG lockerState 快照同样恒 0。
4. **SELFTEST RERUN / RGB SET 门禁**: 原 `App_Locker_IsIdle() &&
   !App_LockerUnlock_IsBusy()` 收敛为仅 IsBusy 判定。
5. **main.c**: App_Locker_Init/Process 撤除 (状态机不再驱动)。
6. **CMakeLists**: App_Locker.c 退出编译 (源文件保留在库中备恢复)。

## B. 协议注释 (App_CustomProtocol.h)

0x23 子命令块整段重写: 六码位标注 [Round_017 已废除] 回 PARAM;
0x05 原 state/hm/sc/su/faultReason 布局废止 (进度统一走 0x09
statusBits); CANCEL 语义更新为仅解锁流程; 0x07 事件队列废止 (事件改
推送帧 0x0F/0x0B/0x0C/0x0E)。

## C. 测试 (Agent/Round_016/test/ 扩展)

- sim stubs 撤除 App_Locker 桩 (解锁流程已无依赖) — 132 checks
  0 failures 回归 + ASAN/UBSAN 干净。
- static_check 新增 5 项: Dispatch #if 0 关闭标注 / CMakeLists 不再
  编译 App_Locker.c / 解锁+主循环无活 App_Locker 引用 / 0x05 废除
  标注 / IO_DIAG lockerState=0 — 全绿 (共 19 项)。
- frame_check ALL OK。

## D. 协议文档 V1.7

- "状态查询 0x23 05" 页重做为 "已废除命令组" 页: 六码位 + 回 PARAM
  + 替代路径 (0x09 位图 / 推送帧) + 处置先例说明。
- 取消 0x04 页: 语义改为仅服务解锁流程, 空闲无操作直回 OK。
- 灯语页 "0x23 05 状态查询定位" 改 0x25 自检 / 0x26 IO 诊断。
- 命令一览 0x23 行更新; 版本历史/封底 V1.7。
- WPS 导出 PDF 34 页 496689B; pptx XML 复核老链字段 (state/hm/sc/su/
  faultReason/硬标期望) 全部清除; judge 验收 5 关键页 5/5 PASS
  (废除页/取消页/封底/版本页/statusBits 回归页)。
