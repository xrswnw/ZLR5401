#ifndef __APP_CUSTOM_PROTOCOL_H
#define __APP_CUSTOM_PROTOCOL_H

#include <stdint.h>

/* =====================================================================*/
/* 协议层: 帧定义 / 组帧 / 解析 / CRC*/
/* 与传输媒介解耦 — 经 ProtoTransport_t 接口收发, UART/USB/NET 各自实现*/
/* 并注册; 帧带 channel 字段, 回复按收帧通道路由 (从哪收就从哪回)。*/
/* =====================================================================*/

#define PROTO_HEADER       0x7753U   /* 帧头 "Sw" LE (0x53 'S' + 0x77 'w')*/
#define PROTO_VERSION      3
#define PROTO_MAX_DATA     1024
#define PROTO_FRAME_MIN    5

#define PROTO_DEV_ADDR_BROADCAST  0xFFU
#define PROTO_RESERVED           0x00U

/* ---- 传输通道标识 ----
 * 协议层与传输媒介解耦: 帧经哪个通道收到, 回复就经哪个通道发出
 * (从哪收就从哪回)。后续 USB/NET 各自实现 ProtoTransport_t 并注册即可。*/
#define PROTO_CH_NONE  0x00U   /* 未指定 — Tx 时回落到默认通道*/
#define PROTO_CH_UART  0x01U
#define PROTO_CH_USB   0x02U
#define PROTO_CH_NET   0x03U
#define PROTO_CH_MAX   0x04U   /* 注册表容量 (含 NONE 占位)*/

/* Request function codes*/
#define FC_HANDSHAKE        0x01
#define FC_ENTER_BOOT       0x02
#define FC_UPGRADE_START    0x03
#define FC_FW_DATA          0x04
#define FC_UPGRADE_VERIFY   0x05
#define FC_UPGRADE_EXEC     0x06
#define FC_DEVICE_INFO      0x07
#define FC_RESET            0x08   /* 软件复位 (空参数, 回 OK 后 NVIC_SystemReset;)*/
#define FC_EXIT_BOOT        0x09   /* 退出升级 (Boot 处理): 校验 App 完好则清 UPG->RUN + 复位跳 App*/
#define FC_MOTOR_CTRL       0x20   /* 步进电机 DRV8434S 控制 (子命令编码, 见下)*/
#define FC_UHF_CTRL         0x21   /* UHF 超高频 SIM7500 模块控制 (子命令编码, 见下)*/
#define FC_AM_CTRL          0x22   /* AM 解码器控制 (子命令编码, 见下)*/
#define FC_LOCKER_CTRL      0x23   /* 开锁器业务编排 (子命令编码, 见下)*/
#define FC_RGB_CTRL         0x24   /* RGB 三色灯控制 (见下)*/
#define FC_SELFTEST_CTRL    0x25   /* 设备级自检/错误位 (子命令编码, 见下; App 专属, Boot 忽略)*/
#define FC_IO_DIAG          0x26   /* IO/传感器直读快照 (App 专属, Boot 忽略; Round_098 优化 #8)*/

/* ---- FC_IO_DIAG (0x26) — IO/传感器直读快照 (排障用, 不锁存) ----
 * 请求 data 空. 响应 data: [err, ir, keyUp, keyDown, uhfPowered, uhfAntennaOk,
 *   amLink, homingStat, lockerState, motorTestState] 10 字节基础位,
 *   + [35B 调试位 (Round_013, 布局见 App_Dispatch.c IO_DIAG case, 可忽略)]:
 *   [10..15] 0x21 同步盘点结果计数 ok/notRdy/busy/noTag/tmo/otherCmd,
 *   [16..32] 最后成功 EPC (len+16B), [33..44] 0x10 校对段匹配快照
 *   (total/tagLen/phase/vMiss/epc[0..7]) — 流程内读取失效排查用。
 *   ir:      PC11 光电门 1=检测到 (原始电平, 无去抖)
 *   keyUp/keyDown: 行程开关原始电平, 低有效: 0=压到(触发) 1=释放
 *                 (与 LockerSeek 寻触判据 ReadKey==0 一致)
 *   uhfPowered: UHF 模块上电; uhfAntennaOk: 天线回波检测通过 (未上电/未检为 0)
 *   amLink:   0=AM 链路正常 (与 AM GET_STATUS 同源)
 *   homingStat: 0=未启动 1=回零中 2=已就绪 3=失败
 *   lockerState/motorTestState: 业务状态机快照 (各子命令 QUERY 同源) */

/* ---- FC_SELFTEST_CTRL (0x25) — 设备级自检/锁存错误位 ----
 * 错误位 16bit 锁存位图 (errBits, bit=1 故障, 高字节在前回传时 L 在前):
 *   bit0=MOTOR_SPI   DRV8434S SPI 配置回读不一致 (通信/落配置失败)
 *   bit1=MOTOR_FAULT DRV8434S 器件故障 (FAULT 位)
 *   bit2=UHF_COMM    UHF Open/Query 通信失败
 *   bit3=AM_COMM     AM 消磁器 Query 通信失败
 *   bit4=PARAM_CRC   参数区 CRC 失败回落默认值 (仅上电置位, 探测不刷新)
 *   bit5=TRAVEL_SW   行程开关缺失/回零失败 (bit0=上 bit1=下)
 *   bit6~15 预留 (恒 0)
 * 置位来源: 上电 POST 探测 / 运行期 Locker 故障联动 (UHF链路->bit2,
 * 未回零->bit5) / 参数区 CRC 失败 (bit4).
 * bit0~3、bit5 随 RERUN 探测刷新; bit4 只能 CLEAR 手动清. */
#define SELFTEST_SUB_QUERY  0x01   /* data: [cmd]  读锁存错误位+实时诊断快照 (无阻塞):
                                        回 [cmd,err,errBitsL,errBitsH,motorCommOk,drvFault,
                                            uhfLink,amLink,paramCrc,switchErr]
                                        motorCommOk: 1=DRV8434S SPI 配置回读一致 (实时再读)
                                        drvFault:    DRV8434S 实时故障寄存器
                                        uhfLink:     0=正常 1=超时 2=CRC 错误
                                        amLink:      0=正常 非0=掉线
                                        paramCrc:    1=上电参数区曾 CRC 失败回落
                                        switchErr:   行程开关错误位 bit0=上 bit1=下 */
#define SELFTEST_SUB_RERUN  0x02   /* data: [cmd]  重探外设并刷新锁存位 (阻塞~3s, 须 Locker/Unlock 空闲):
                                        回 [cmd,err,errBitsL,errBitsH] */
#define SELFTEST_SUB_CLEAR  0x03   /* data: [cmd,maskL,maskH]  清指定位 (bit 定义同上, 可多选):
                                        回 [cmd,err,errBitsL,errBitsH] (清后的剩余位图) */
#define SELFTEST_ERR_OK     0
#define SELFTEST_ERR_PARAM  1
#define SELFTEST_ERR_BUSY   2   /* RERUN 时 Locker/Unlock 非空闲 */

/* ---- FC_RGB_CTRL (0x24) — RGB 三色灯控制 ----
 * data: [mask, reserved]   mask=颜色位掩码(bit0=G,bit1=R,bit2=B, 其余预留), reserved=预留字节.
 * 响应: data[0]=mask(回显, err!=OK 时为 0), data[1]=err(0=OK). err 值见 RGB_ERR_*.
 * 仅设备空闲 (Locker/Unlock/行程测试均不运行) 时接受, 作为手动灯语保持
 * 10s 自动回收 (业务灯语优先); mask=0 撤销. 运行中回 RGB_ERR_BUSY. */
#define RGB_CMD_SET          0x01   /* data: [cmd,mask,reserved] */
#define RGB_ERR_OK           0
#define RGB_ERR_PARAM        1
#define RGB_ERR_BUSY         2

/* ---- FC_LOCKER_CTRL (0x23) 子命令编码 (data[0]) ----
 * 所有响应同步: data[0]=cmd, data[1]=err(0=OK). err 值见 LOCKER_ERR_*.
 * 注: 协议帧 data 上限 PROTO_MAX_DATA=1024 且解析器为流式字节重组,
 *     单帧可跨多个 HID report。
 * [Round_017 已废除] 老清单业务链 (0x01 CONFIGURE / 0x02 ADD / 0x03
 * START / 0x05 QUERY / 0x06 CONSUME_SOFT / 0x07 GET_EVENT) 整批关闭
 * (用户裁决 "老清单业务彻底不回归, 现在只有 0x10/0x11"): 码位显式回
 * err=2 PARAM (区别于未知子命令), 实现保留 #if 0 备恢复, 处置先例同
 * 0x08/0x0A。业务互斥由解锁流程 (App_LockerUnlock) 独立承担。 */
#define LOCKER_SUB_CONFIGURE  0x01   /* [Round_017 已废除] 老清单配置 — 回 PARAM */
#define LOCKER_SUB_ADD        0x02   /* [Round_017 已废除] 老清单追加 EPC — 回 PARAM */
#define LOCKER_SUB_START      0x03   /* [Round_017 已废除] 老清单启动扫描 — 回 PARAM */
#define LOCKER_SUB_CANCEL     0x04   /* data: [cmd]  取消解锁流程 (0x10/0x11): 立即
                                        回 OK, 设备免疫安全回降后终帧 endReason=6;
                                        流程空闲时为无操作直回 OK */
#define LOCKER_SUB_QUERY      0x05   /* [Round_017 已废除] 老清单状态查询 — 回 PARAM
                                        (原 state/hm/sc/su/faultReason 布局废止;
                                        解锁进度统一走 0x09 statusBits 位图) */
#define LOCKER_SUB_CONSUME_SOFT 0x06 /* [Round_017 已废除] 老软标上报 — 回 PARAM */
#define LOCKER_SUB_GET_EVENT  0x07   /* [Round_017 已废除] 老事件队列 — 回 PARAM
                                        (解锁事件改推送帧 0x0F/0x0B/0x0C/0x0E) */
#define LOCKER_SUB_ONE_SHOT   0x08   /* [Round_011 已废除] 单标签同步开锁流程整体移除:
                                        单标场景统一走 0x0A (epcCnt=1, W=120000 即 2min)。
                                        保留码位显式回 err=2 PARAM (区别于未知子命令);
                                        主机不得再下发本子命令。 */
#define LOCKER_SUB_GET_PROGRESS 0x09 /* data: [cmd]  流程中拉取进度 (v3 布局,
                                        Round_016 起 0x10/0x11 统一; 非流程时
                                        全零):
                                        回 [cmd,err,status,tag,elapsed,
                                        riseCnt,alarmCnt,lastCycle,rsv] (9B)
                                        status=单字节观测位图 (查表即读,
                                        不需理解流程内部相位):
                                          bit0 BUSY  流程进行中 (受理~终帧)
                                          bit1 IR    红外遮挡 (200ms 去抖后;
                                                     仅作升起门, 升起后只报不控)
                                          bit2 TAG   0x10=名单存在 CONFIRMED
                                                     有效子集; 0x11=AM 消磁在线
                                          bit3-4 MOTOR 电机状态/位置:
                                                     0=静止·底 1=上行 2=在顶
                                                     3=下行 (正常回降/异常快降)
                                          bit5 FGN   外来标签已判介入 (读到 2 次)
                                          bit6 ALARM 报警进行中 (红50/50+蜂鸣)
                                          bit7 UHF   UHF 失联累计中 (5s 未恢复
                                                     -> 终帧 endReason=4)
                                        tag: 0x10=当前 CONFIRMED 名单张数;
                                             0x11=已解锁数 (逐帧即时)
                                        elapsed: 流程已进行秒 (u8, 0xFF 封顶;
                                             窗口全程时长见终帧)
                                        riseCnt/alarmCnt/lastCycle: 0x10 循环
                                        统计 (升顶次数/报警次数/最近电机事件
                                        0无 1升至顶 2全失回降 3外来回降
                                        4红外丢失回降 Round_020 新增);
                                        0x11 恒 0。rsv 保留恒 0。
                                        典型值速查: 0x00 空闲 / 0x01 等标签 /
                                        0x05 门只差红外 / 0x0D 上行中 /
                                        0x15 在顶保持 / 0x75 在顶·外来介入 /
                                        0x79 介入快降 / 0x59 全失快降 /
                                        0x1D 超窗正常回降 / 0x81 UHF 失联中。 */
#define LOCKER_SUB_UNLOCK_MULTI 0x0A /* [Round_013 已关闭] 多标签解锁流程停用: 分发层显式
                                        回 PARAM (同 0x08), 灯语槽位/UNLK_ERR_* 错误码移交
                                        0x10 UNLOCK_EPC; 原实现保留于 App_Dispatch.c #if 0
                                        备恢复。data: [cmd,tmoL,tmoH,holdL,holdH,softCnt,epcCnt,epcLen,
                                        epcCnt*epcLen 字节]  多标签解锁整合主路径 (App_LockerUnlock.c):
                                        单帧下发 m(<=4) 张期望 EPC + 软标数, 阻塞自治至结账完成。
                                        epcCnt=0 且 softCnt>0: 纯软标结账 — 跳过光电门控与
                                        EPC 校验直接进入 升起+消磁+软解码 (受理即计时, 窗口
                                        即软标窗); epcCnt=0 且 softCnt=0 -> err=2 PARAM。
                                        全确认门控: 所有 EPC 均校验通过 (n==m) 才升电机并开
                                        消磁; PARTIAL/UHF_LOST 出口磁块全程不动、不消磁。
                                        阶段推送帧 (func 同 0x23^0xFF, data[0] 为下述子码):
                                          0x0F 受理帧: [0x0F,0,phase(1=WAIT_TAG,纯软标=5 SOFT),
                                                        winMs(3 LE)]
                                          0x0B 确认帧: [0x0B,seq,epcLen,epc..,confirmed,total,
                                                        判据耗时(2 LE ms),流程耗时(2 LE 秒)]  每张一帧+蜂鸣200ms
                                          0x0C 失配事件帧: [0x0C,epcLen,epc..,hits]  外来标签
                                                        稳定确认一张一帧 (判据同期望标签,
                                                        缓存≤4张), 推帧不终止
                                          0x0D 硬标完成帧: [0x0D,endReason,bitmap,confirmed,total,elapsed(2 LE 秒)]
                                          0x0E 软标解码帧: [0x0E,done,softCnt]
                                        终帧 = 0x0A 回显 (流程结束标志): [0x0A,err,...]:
                                          err=0: [endReason,bitmap,confirmed,total,rise(2),lower(2),
                                                  softDone,softCnt,elapsed(2 LE 秒)]
                                                  endReason: 1 ALL_OK / 2 硬标窗满 PARTIAL /
                                                  4 UHF_LOST / 6 ABORTED / 7 软标窗满未校验
                                                  完成按超时失败结账
                                          err=1 BUSY: [lockerState,uhfState,stepperState]
                                          err=3/4 UHF: [uhfRawErr]  err=7 HOMING: [switchErr]
                                          err=8 MOTOR_FAULT: [fault,diag1,diag2,steps(3),phase,retreat]
                                          err=9 MOTOR_TIMEOUT: [steps(3),phase,retreat]
                                          err=2 PARAM / 10 AM_LINK / 11 NO_IR: 无诊断字段
                                        失败码/结束原因/阶段/参数宏见 App_LockerUnlock.h (UNLK_*)。
                                        解锁态内 (受理帧起) 仅响应 CANCEL/GET_PROGRESS, 其余子命令
                                        及 MOTOR/UHF/AM 控制类一律 BUSY。 */
#define LOCKER_SUB_EVT_TAG      0x0B /* 设备推送: 某期望 EPC 稳定确认 (布局见上) */
#define LOCKER_SUB_EVT_MISMATCH 0x0C /* 设备推送: 外来标签稳定确认失配 (一张一帧) */
#define LOCKER_SUB_EVT_HARD_DONE 0x0D /* 设备推送: 硬标段结束 (n==m / 窗满部分 / CANCEL / 失联) */
#define LOCKER_SUB_EVT_SOFT     0x0E /* 设备推送: 软标解码计数 +1 */
#define LOCKER_SUB_EVT_START    0x0F /* 设备推送: 解锁命令已受理 (含实际解锁窗 W) */
#define LOCKER_SUB_UNLOCK_EPC  0x10 /* data: [cmd, tmoL,tmoH, epcCnt, (epcLen, epc...)
                                        ×epcCnt, rsv...]  EPC 解锁 (Round_013 新,
                                        Round_014 按客户联调诉求重设计, 阻塞自治,
                                        App_LockerUnlock.c EpcRun):
                                        tmo=u16 秒流程窗 (>0, 上限 600s 钳位);
                                        epcCnt=名单张数 1~10 (缓冲 10 张), 每张
                                        变长 (epcLen 1~12 + EPC 字节), 名单内
                                        EPC 不得重复 (违者 PARAM); rsv 尾随字节
                                        忽略。
                                        流程: UHF 0x22 多标签连续盘点; 名单任意
                                        非空子集稳定在场 (首读起 700ms 无
                                        1500ms 级断读) + 红外遮挡 (200ms 去抖)
                                        且无外来介入 -> 升起 (4000µ步/s);
                                        顶部保持须 在场+红外遮挡 (Round_020:
                                        红外丢失立马下降); 名单全部离场 ->
                                        静默快降。
                                        异常 (外来标签介入=非名单 EPC 累计读到
                                        3 次) -> 报警: 红灯
                                        50ms 亮/50ms 灭闪 + 蜂鸣 50ms/500ms,
                                        电机快速下行; 名单全部离场 -> 静默
                                        快降 (Round_020 裁决: 红闪蜂鸣仅
                                        非法标签); 外来稳定消失 (1500ms 未
                                        再读到) 且有效子集恢复稳定 -> 经红外门
                                        重新升起, 循环直至超窗。
                                        超窗时在顶 -> 回降 + ALL_OK + 蜂鸣 1s;
                                        未升起/异常回降后未恢复 -> TIMEOUT
                                        (黄慢闪)。CANCEL: 安全回降 + 全状态
                                        复原 + ABORTED。UHF 失联 5s -> UHF_LOST。
                                        阶段推送帧 (func 同 0x23^0xFF): 0x0F 受理
                                        (v3: 字节[2]=statusBits, 0x10=0x01 /
                                        0x11=0x05 消磁在线; winMs=tmo*1000) /
                                        0x0B 名单标签稳定确认
                                        (复用 0x0A 布局, 每次进入稳定态一帧) /
                                        0x0C 外来介入 (每事件一帧)。
                                        GET_PROGRESS 0x09 v3 布局 (statusBits
                                        单字节位图, 见上)。
                                        终帧 = 0x10 回显 (流程结束标志): [0x10,err,...]:
                                          err=0: [endReason,bitmap,rise(2 LE),lower(2 LE),
                                                  elapsed(2 LE 秒, 0xFFFF 封顶),
                                                  riseCnt,alarmCnt,fgnSeen]
                                                  endReason: 1 ALL_OK 超窗时在顶
                                                  (曾升起并保持) / 3 TIMEOUT 超窗
                                                  未达成升起 / 4 UHF_LOST /
                                                  6 ABORTED;  riseCnt=升至顶部
                                                  次数, alarmCnt=报警次数,
                                                  fgnSeen=是否出现过外来介入
                                          err=1 BUSY: [lockerState,uhfState,stepperState]
                                          err=3/4 UHF: [uhfRawErr]  err=7 HOMING: [switchErr]
                                          err=8 MOTOR_FAULT: [fault,diag1,diag2,steps(3),phase,retreat]
                                          err=9 MOTOR_TIMEOUT: [steps(3),phase,retreat]
                                          err=2 PARAM: 无诊断字段
                                        失败码沿用 UNLK_ERR_* (旧 0x0A 关闭后由本流程接管)。
                                        流程态内仅响应 CANCEL/GET_PROGRESS, 其余一律 BUSY。 */
#define LOCKER_SUB_UNLOCK_AM    0x11 /* data: [cmd,amCnt,tmoL,tmoH,rsv0,rsv1]  AM 标签
                                        解锁 (Round_013 新; Round_014 恢复 AM 控制逻辑,
                                        阻塞自治, App_LockerUnlock.c AmRun):
                                        受理 -> AM 切消磁模式 (SET_MODE mode0,
                                        回显确认, 重试 3 次全败按 AM_LINK 结账)
                                        -> tmo 秒计数窗: 每收到一条 cmd17 消磁帧
                                        立即入账一计 (Round_014 裁决 "收到一个
                                        消磁帧立马更新状态, 无需等待无需稳定,
                                        它只有一次" — 逐帧计数, 不再等 3s 突发
                                        静默结算; 每计推 0x0E + 绿闪) -> 达标
                                        (>=amCnt) 或超窗 -> AM 挂起切仅检测
                                        (SET_MODE mode1, 回显确认; 确认失败不
                                        阻塞结账, 终帧 amFail=1 如实上报 —
                                        解码器自行恢复消磁的风险无法根除, 客户
                                        已知情) -> 蜂鸣 1s + 绿三闪结账, rise/
                                        lower 恒 0 (全程不动电机, 2026-09-19 裁决
                                        维持)。
                                        amCnt: 0~255 (0=直接完成支路: 不耗窗不碰
                                        AM, 首查即达标结账 ALL_OK); tmo: u16 秒
                                        (>0); rsv0/rsv1 保留位 (设备现不解释,
                                        终帧原样回显)。
                                        CANCEL: 立即尽力挂起 + ABORTED (无机械段,
                                        无免疫窗口)。达标检查优先于窗满 (同拍
                                        边界); 逐帧即时入账, 无窗末宽限。
                                        阶段推送帧 (func 同 0x23^0xFF): 0x0F 受理
                                        (v3: 字节[2]=statusBits=0x05 消磁在线,
                                        旧 phase=5 义废止; winMs=tmo*1000) /
                                        0x0E 每解锁一张 [done, amCnt]。
                                        GET_PROGRESS 0x09 v3 布局 (statusBits):
                                        status 仅 BUSY|TAG(消磁在线; 挂起/结账
                                        瞬时态 TAG 熄灭), tag=已解锁数 (逐帧
                                        即时), riseCnt/alarmCnt/lastCycle 恒 0。
                                        终帧 = 0x11 回显 (流程结束标志): [0x11,err,...]:
                                          err=0: [endReason,rise(2 LE,恒0),lower(2 LE,恒0),elapsed(2 LE 秒,
                                                  0xFFFF 封顶),amCnt(回显),deactDone,
                                                  deactFail(1=挂起确认失败,解码器可能
                                                  仍在消磁; 其余恒0),rsv0,rsv1 原样回显]
                                                  endReason: 1 ALL_OK 计数达标 (蜂鸣+绿闪
                                                  结账, 电机未动) / 3 TIMEOUT 窗满未达标 /
                                                  6 ABORTED
                                          err=1 BUSY: [lockerState,uhfState,stepperState]
                                          err=2 PARAM / 10 AM_LINK (切消磁确认失败,
                                          电机/AM 均未动作): 无诊断字段
                                          (err 7/8/9 HOMING/MOTOR: 全程不动电机,
                                          不可达, 编码保留)
                                        失败码沿用 UNLK_ERR_*。流程态内仅响应
                                        CANCEL/GET_PROGRESS, 其余一律 BUSY。 */
/* 逻辑错误码 (data[1]) — 0x01~0x07 子命令用 */
#define LOCKER_ERR_OK         0
#define LOCKER_ERR_BUSY       1     /* 非空闲, 需先取消 */
#define LOCKER_ERR_PARAM      2     /* 参数非法/超上限 */
#define LOCKER_ERR_NO_EVENT   3     /* 无待取事件 */

/* ---- FC_MOTOR_CTRL (0x20) 子命令编码 (data[0]) ----
 * data: [0]=cmd, 后续参数随 cmd 而定. 响应 data[0]=cmd, data[1]=err(0=OK), 其余随 cmd. */
#define MOTOR_CMD_MOVE      0x01   /* data: [cmd,dir,stepsL,stepsM,stepsH]  dir=0/1, steps=24bit. 启动运动 */
#define MOTOR_CMD_STOP      0x02   /* 停止并关断输出 */
#define MOTOR_CMD_SPEED     0x03   /* data: [cmd,hzL,hzH]  设定微步/秒 (1~2000) */
#define MOTOR_CMD_TORQUE    0x04   /* data: [cmd,pct]  转矩百分比 (6~100) */
#define MOTOR_CMD_QUERY     0x05   /* 查询: 状态/故障/步数 */
#define MOTOR_CMD_CLEAR     0x06   /* 清除故障 */
#define MOTOR_CMD_TEST      0x07   /* 电机行程测试: data: [cmd,passes]. passes=往返次数(1~255).
                                       流程: 最高速正转->触点->立即反转->触点(完成1次)->重复passes次->停转.
                                       进行中再下发: 回 MOTOR_ERR_BUSY. */
#define MOTOR_CMD_HEALTH    0x08   /* 读健康/堵转监测: 回 [cmd,err,olovState,threshL,threshH,trqL,trqH,reason] */
#define MOTOR_CMD_STATS     0x09   /* 读运行统计: 回 [cmd,err,runSec(3),startCnt(2),lastReason] */
#define MOTOR_CMD_TRACE     0x0A   /* 电机黑匣子导出 (现场堵转排查, RAM 记录断电清零): data: [cmd].
                                       回 (全 LE): 头 20B
                                         [cmd,err,state,fault,diag1,diag2,olov,threshL,threshH,trqPct,
                                          dir,switchErr,speedHz(4),nSmp,nHist,intervalMs,rsv]
                                       + 历史环 nHist×19B (最新->最旧, 每次真实停机一条, 覆盖回零重试序列):
                                         [startCnt(4),steps(4),runMs(4),trqFinal(2),reason,fault,diag2,olov,rsv]
                                       + 采样环 nSmp×9B (最近一腿, 最新->最旧, 每次 Move 覆盖):
                                         [trq(2),ms(2),steps(4),flags]
                                       flags: bit0=KEY_UP按下 bit1=KEY_DOWN按下 bit2=过载降速中
                                       reason: 0=无 1=正常(触点/完成/外部停) 2=连续运行时限
                                               3=高负载停机或行程基准超限 4=DRV故障(OCP/UVLO等)
                                       判读: 末条历史 reason=3 且末段样本 steps 仍推进 -> 看末段 trq
                                       绝对值 (接近0=真失步堵转, 略低于阈值=监测误判);
                                       reason=4 按 fault 位查供电/过流/过温; 样本 flags KEY 位
                                       中途置位=行程开关误触发。 */
/* 运行错误码 */
#define MOTOR_ERR_OK            0
#define MOTOR_ERR_PARAM         1
#define MOTOR_ERR_FAULT         2
#define MOTOR_ERR_BUSY          3

/* ---- FC_UHF_CTRL (0x21) 子命令编码 (data[0]) ----
 * data: [0]=cmd, 后续参数随 cmd 而定. 响应 data[0]=cmd, data[1]=err(0=OK), 其余随 cmd. */
#define UHF_SUB_OPEN           0x01   /* data: [cmd]  上电 + 配置下发, 进入 READY */
#define UHF_SUB_CLOSE          0x02   /* data: [cmd]  停止并下电 */
#define UHF_SUB_INVENTORY      0x03   /* data: [cmd,timeoutL,timeoutH] 同步盘点: 内部完成
                                        0x22多标签盘存(timeout,缺省1s)->0x29取回, 标签内联在响应
                                        返回 [cmd,err,countL,countH,{rssi,epcLen,epc..}...], 无主动上报 */
#define UHF_SUB_READ_TAG       0x04   /* data: [cmd,epcLen,epc..,bank,addr,cnt]  读标签 */
#define UHF_SUB_WRITE_TAG      0x05   /* data: [cmd,epcLen,epc..,bank,addr,len,data..]  写标签 */
#define UHF_SUB_STOP           0x06   /* data: [cmd]  停止当前操作 */
#define UHF_SUB_QUERY          0x07   /* data: [cmd]  查询链路/状态 */
#define UHF_SUB_GET_CONFIG     0x08   /* data: [cmd]  读取当前配置 (含 band) */
#define UHF_SUB_SET_CONFIG     0x09   /* data: [cmd,powerDbm,antenna,checksumEn,session,target,q,(band)]  设置配置 */
#define UHF_SUB_GET_TAGS       0x0A   /* data: [cmd,(count)]  count=0 取全部; >0 取前 count 条 (数据面备用) */
#define UHF_SUB_GET_STATUS     0x0B   /* data: [cmd]  读取状态/错误监控 */
#define UHF_SUB_CHECK_ANT      0x0C   /* data: [cmd]  主动触发回波检测 */
#define UHF_SUB_SCAN_START     0x0D   /* data: [cmd,cycleL,cycleH] 启动自动扫描: 连续盘点入缓冲(不主动上报),
                                        主机用 GET_TAGS 拉取取走 (含 RSSI, 用于识别/测距);
                                        识别到标签即蜂鸣 50ms 提示; 扫描中 SET_CONFIG 延迟到下一轮盘点前下发 */
#define UHF_SUB_SCAN_STOP      0x0E   /* data: [cmd]  停止自动扫描并停止当前盘点 */
#define UHF_SUB_GET_DUMP       0x10   /* data: [cmd]  读取 0x22/0x29 原始字节诊断 (Round_050) */
#define UHF_SUB_INVENTORY_ONE  0x11   /* data: [cmd,tmoL,tmoH] 同步阻塞单标签盘点 (0x21, Round_013):
                                        Timeout 内读到 1 张立即返, EPC 内联在响应。tmo=0 缺省 1s。
                                        回 [cmd,err,epcLen,epc..,msL,msH]:
                                        ms = 设备侧发帧->收模块响应耗时 (LE u16, 耗时标定);
                                        err=5 NO_TAG (含伪短帧丢弃) / 6 TIMEOUT 时无 EPC 字段仍带 ms */
/* 运行错误码 (data[1]) */
#define UHF_ERR_OK             0
#define UHF_ERR_PARAM          1
#define UHF_ERR_BUSY           2
#define UHF_ERR_NOT_READY      3
#define UHF_ERR_LINK           4
#define UHF_ERR_NO_TAG         5
#define UHF_ERR_TIMEOUT        6

/* ---- FC_AM_CTRL (0x22) 子命令编码 (data[0]) ----
 * 解码器经 USART1 RS232 (PA9/PA10) 通信. 帧族按 decoderType 单选 (Round_021):
 * AM 消磁解码器 (2A A2) / RF 解码器 (B5 A1), 互斥不并存.
 * 响应 data[0]=cmd, data[1]=err(0=OK), 其余随 cmd.
 * 值用 16bit (高字节在后) 表示: (dataH<<8)|dataL.
 * 参数命令字为当前模式原生命令字透传 (AM: 0x11~0x67/0x50…; RF: 0x05~0x20/0x06…),
 * 双命令表并列见协议文档, 固件不做值域翻译。 */
#define AM_SUB_GET_CONFIG   0x01   /* data: [cmd]  读取当前配置 (AM 专属; RF 模式回 PARAM) */
#define AM_SUB_SET_CONFIG   0x02   /* data: [cmd, thrH,thrL, hitH,hitL, freq, delayH,delayL,
                                               len, invert, syncH,syncL, volt, mode, mains]  设置配置
                                        (AM 专属; RF 模式回 PARAM — RF 参数走 0x03/0x04 透传) */
#define AM_SUB_GET_PARAM    0x03   /* data: [cmd, amCmd]  读单个参数 (amCmd 为当前模式原生命令字:
                                        AM 经 0x63 真读回; RF 经 0x01 单帧查询后按命令序抽取) */
#define AM_SUB_SET_PARAM    0x04   /* data: [cmd, amCmd, valH, valL]  写单个参数 (回显式确认;
                                        RF 值仅低 8 位有效, 参数均为单字节) */
#define AM_SUB_QUERY        0x05   /* data: [cmd]  探测链路 (AM 0x63 总查询 / RF 0x01 查询) */
#define AM_SUB_GET_STATUS   0x06   /* data: [cmd]  读监控 (无 err 字段):
                                        [cmd,link,evt(4),lastEvtMs(4),deact,deactCnt(4),failCnt(4)]
                                        deact 0=空闲/1=消磁成功/2=消磁失败(判据证伪后不可达);
                                        AM cmd17 / RF 0x3A 突发静默超 3s 结算即一次成功
                                        (帧数不限, 2026-09-19 真机证伪 ">=2帧=失败"; 真失败
                                        持续连发永不结算; failCnt 恒 0 保留) */
#define AM_SUB_SET_MODE     0x07   /* data: [cmd, mode]  仅切工作模式 (0检测消磁/1仅检测/2待机;
                                        RF 对应 0检测解码/1只检测/2休眠, 值域一致), AM 模式持久化 */
#define AM_SUB_GET_WAVE     0x08   /* data: [cmd]  触发一次同步波形采集 (0x64, 阻塞~1s):
                                        回 [cmd,err,pointsH,pointsL] (AM 专属; RF 回 PARAM) */
#define AM_SUB_GET_WAVE_PAGE 0x09  /* data: [cmd, page]  取一页波形 (48 点/页): 回 [cmd,page,err,pointsH,pointsL,w(≤48)]
                                        (AM 专属; RF 回 PARAM) */
#define AM_SUB_GET_DECODER  0x0A   /* data: [cmd]  查询解码器类型 (本地读, 无总线动作):
                                        回 [cmd,err,type]  type: 0=AM 消磁解码器 1=RF 解码器 */
#define AM_SUB_SET_DECODER  0x0B   /* data: [cmd, type]  切换解码器类型 (type 0/1; 空闲立即生效,
                                        重建后端+帧头+持久化+联动探链; 解锁流程中回 BUSY):
                                        回 [cmd,err,type,link]  link=切换后新链路探测结果
                                        (接错外设=探链失败如实上报, 不自适应; 幂等: 同类型直回) */
/* 运行错误码 (data[1]) */
#define AM_ERR_OK           0
#define AM_ERR_PARAM        1
#define AM_ERR_BUSY         2     /* 解锁 0x10/0x11 流程进行中, 仅放行 AM_SUB_GET_STATUS */
#define AM_ERR_LINK         4
#define AM_ERR_TIMEOUT      6

/* Response FC = req_FC ^ 0xFF*/
#define FC_RSP(x)           ((x) ^ 0xFF)

/* Result codes*/
#define RESULT_OK               0

/* FC=0x02 results*/
#define ENTER_BOOT_PARAM_ERR    2

/* FC=0x03 results*/
#define UPG_NO_SPACE            1
#define UPG_ERASE_FAIL          2

/* FC=0x04 results*/
#define DATA_FLASH_FAIL         2
#define DATA_ADDR_ERR           3

/* FC=0x05 results — 三级校验*/
#define VERIFY_CRC_MISMATCH    1
#define VERIFY_SIZE_MISMATCH   2
#define VERIFY_VECTOR_INVALID  3
#define VERIFY_BIND_FAIL       4

/* Parser states*/
typedef enum {
    PROTO_STATE_IDLE = 0,
    PROTO_STATE_HEAD1,
    PROTO_STATE_DEV_ADDR,
    PROTO_STATE_DEV_RESERVED,
    PROTO_STATE_LEN
} ProtoState_t;

typedef struct {
    uint16_t header;
    uint8_t  devAddr;
    uint8_t  reserved;
    uint16_t length;
    uint8_t  func;
    uint8_t  data[PROTO_MAX_DATA];
    uint16_t dataLen;
    uint32_t crc32;
    uint8_t  channel;   /* 收帧所在通道 (Proto_Poll 写入), Tx 回复时据此路由*/
} ProtoFrame_t;

typedef struct {
    ProtoState_t state;
    uint8_t  buf[PROTO_MAX_DATA + 11];
    uint16_t pos;
    uint16_t lenTarget;
    uint16_t frameEnd;
    uint32_t timeoutMs;
} ProtoParser_t;

/* 帧回调类型: 协议层解析出完整帧后调用应用层*/
typedef void (*ProtoFrameCb_t)(ProtoFrame_t *frame);

/* ---- 传输层接口 (媒介无关) ----
 * UART/USB/NET 各自提供一个实体并通过 Proto_RegisterTransport 注册;
 * 协议层只通过函数指针调用, 不直接依赖任何具体传输。*/
typedef struct {
    uint16_t (*RxAvailable)(void);                      /* 可读字节数, 0=无*/
    uint16_t (*RxRead)(uint8_t *buf, uint16_t maxlen);   /* 拷出并清空, 返回拷出长度*/
    uint8_t  (*TxIsBusy)(void);                          /* 发送忙? 非0=忙*/
    void     (*TxDma)(const uint8_t *buf, uint16_t len); /* 启动发送*/
} ProtoTransport_t;

/* ---- 协议层 API (媒介无关) ----*/
void     Proto_Init(void);
void     Proto_SetDeviceAddr(uint8_t addr);   /* 设置本机地址, 用于收帧过滤*/
void     Proto_RegisterFrameCb(ProtoFrameCb_t cb);

/* 注册/默认传输通道: channel 取 PROTO_CH_UART/USB/NET*/
void     Proto_RegisterTransport(uint8_t channel, const ProtoTransport_t *t);
void     Proto_SetDefaultChannel(uint8_t channel);  /* Tx 通道为 NONE 时回落到此*/

void     ProtoParserInit(ProtoParser_t *p);
int      ProtoParseByte(ProtoParser_t *p, uint8_t byte, ProtoFrame_t *frame);

int      Proto_BuildFrame(uint8_t devAddr, uint8_t func, const uint8_t *data,
                          uint16_t dataLen, uint8_t *out, uint16_t *outLen);
int      Proto_BuildResponse(uint8_t devAddr, uint8_t reqFc, const uint8_t *data,
                             uint16_t dataLen, uint8_t *out, uint16_t *outLen);

/* 用本层帧缓冲组帧并经指定传输通道发出;
 * channel 取 PROTO_CH_UART/USB/NET, 传 PROTO_CH_NONE 则回落到默认通道。
 * 回复通常传收帧时的 frame->channel (从哪收就从哪回)。*/
void     Proto_TxFrame(uint8_t channel, uint8_t fc, const uint8_t *data, uint16_t len);
void     Proto_TxResponse(uint8_t channel, uint8_t reqFc, const uint8_t *data, uint16_t len);
/* 指定目标地址(回复给请求方)*/
void     Proto_TxResponseTo(uint8_t channel, uint8_t devAddr, uint8_t reqFc,
                            const uint8_t *data, uint16_t len);

/* 主循环调: 取传输层字节 -> 解析 -> 回调应用层*/
void     Proto_Poll(void);

uint32_t Proto_Crc32(const uint8_t *data, uint32_t len);

/* ---- 内存/字符串公共工具 (供 Param/Dispatch 复用) ----*/
void      Memcpy(void *dst, const void *src, uint32_t n);
void      Memset8(void *dst, uint8_t val, uint32_t n);
int       Strncmp(const char *s1, const char *s2, uint32_t n);

#endif /* __APP_CUSTOM_PROTOCOL_H*/
