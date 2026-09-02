"""P9 — Round_098 优化项 #3/4/8/9/11/15/20/21 固件回归.

前置: 真标签 33553463a4000158eb7e7507 在天线场内; JLink 已拔除干扰.
覆盖:
  O1  IO_DIAG (0x10) 新命令字段基线 (#8)
  O2  UHF CLOSE -> QUERY=NOT_READY(3) + IO_DIAG 联动 (#15/#8)
  O3  TEST 互斥/完成/二次 TEST (#3/#4)
  O4  QUERY state 重映射: 测试中 state=1, 完成后回落步进态 (#4)
  O5  0x08 单标签流程废除 — 任意帧形状一律 PARAM (Round_011)
  O6  S2 会话下 Locker 全周期 x2 (#21)
  O7  复位后后台回零: UHF 先就绪(#9) + 回零中 MOVE=BUSY(#3) + READY(#11)
"""
import sys
import time

sys.path.insert(0, '.')
sys.path.insert(0, '../../Round_008/test')
from lib import Recorder, open_link, tx, txm, reopen_until_alive, drain, C, load_hw

FCM = C.FC_MOTOR_CTRL
FCU = C.FC_UHF_CTRL
FCL = C.FC_LOCKER_CTRL
FC_IO_DIAG = 0x10

MT_IDLE, MT_RUN, MT_DONE, MT_FAULT = 0, 1, 2, 3
BUSY = 3
NOT_READY = 3
ONE, PROG, CANCEL, CFG, ADD, START, QUERY_L, CONSUME, GETEV = (
    0x08, 0x09, 0x04, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07)
ONE_ERR_PARAM, ONE_ERR_NO_IR = 2, 11
EV_MATCH = C.LOCKER_EVT_MATCH_OK
EV_SOFT_USED = C.LOCKER_EVT_SOFT_USED
EV_DONE = C.LOCKER_EVT_DONE

hw = load_hw()
REAL_EPC = bytes.fromhex(hw["uhf_tags"][0]) if hw.get("uhf_tags") else None

rec = Recorder("P9-OptFix")
lk = open_link()


def mquery(t=2):
    d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_QUERY], timeout_s=t)
    return d if not to else None


def iod(t=2):
    d, to = tx(lk, FC_IO_DIAG, [], timeout_s=t)
    return d if not to else None


def uhf(sub, data=None, t=4):
    d, to = txm(lk, FCU, [sub] + (data or []), timeout_s=t)
    return d


# ---- O1 IO_DIAG 基线 (#8) ----
d = iod(3)
ok = d is not None and len(d) == 10
rec.check("O1a", "FC_IO_DIAG(0x10) 返回 10 字段帧", ok,
          f"ir={d[1]} keyUp={d[2]} keyDn={d[3]} uhfPwr={d[4]} ant={d[5]} "
          f"am={d[6]} homing={d[7]} locker={d[8]} test={d[9]}" if ok else f"d={d.hex() if d else 'TO'}",
          f"d={d.hex() if d else 'TO'} len={len(d) if d else 0}")
if ok:
    rec.check("O1b", "IO_DIAG 上电基线 (POST 后 UHF 已上电/天线 OK/已回零)",
              d[4] == 1 and d[5] == 1 and d[7] == 2 and d[8] == 0 and d[9] == 0,
              f"uhfPwr={d[4]} ant={d[5]} homing={d[7]}", f"d={d.hex()}")
    rec.check("O1c", "IO_DIAG 行程开关读数与状态自洽 (顶部 KeyUp=1)",
              d[2] in (0, 1) and d[3] in (0, 1), f"keyUp={d[2]} keyDn={d[3]}", "")

# ---- O2 CLOSE -> NOT_READY + IO_DIAG 联动 (#15/#8) ----
d = uhf(C.UHF_CLOSE, t=4)
rec.check("O2a", "UHF CLOSE", d is not None and d[1] == 0, "err=0", f"d={d.hex() if d else 'TO'}")
time.sleep(1.5)
d = uhf(C.UHF_QUERY, t=4)
rec.check("O2b", "CLOSE 后 UHF QUERY -> NOT_READY(3)", d is not None and d[1] == NOT_READY,
          f"err={d[1]} state={d[2] if d else '?'}" if d else "TO", f"d={d.hex() if d else 'TO'}")
d = iod(2)
rec.check("O2c", "IO_DIAG 反映 UHF 下电 (uhfPwr=0)", d is not None and d[4] == 0,
          f"uhfPwr={d[4]}" if d else "TO", f"d={d.hex() if d else 'TO'}")
d = uhf(C.UHF_OPEN, t=10)
rec.check("O2d", "UHF OPEN 恢复", d is not None and d[1] == 0, "err=0", f"d={d.hex() if d else 'TO'}")
d = iod(2)
rec.check("O2e", "IO_DIAG 恢复 uhfPwr=1", d is not None and d[4] == 1,
          f"uhfPwr={d[4]}" if d else "TO", "")

# ---- O3 TEST 互斥/完成/二次 TEST (#3/#4) ----
d = uhf(C.UHF_QUERY, t=4)   # 确保 UHF 就绪无干扰
d, to = tx(lk, FCM, [C.MOTOR_TEST, 1], timeout_s=3)
rec.check("O3a", "TEST 受理", d is not None and d[1] == 0, "err=0",
          f"d={d.hex() if d else 'TO'}")
d, to = tx(lk, FCM, [C.MOTOR_MOVE, 0x00, 100, 0, 0], timeout_s=2)
rec.check("O3b", "TEST 中 MOVE -> BUSY(3)", d is not None and d[1] == BUSY,
          f"err={d[1]}" if d else "TO", f"d={d.hex() if d else 'TO'}")
d, to = tx(lk, FCM, [C.MOTOR_TEST, 1], timeout_s=2)
rec.check("O3c", "TEST 中二次 TEST -> BUSY(3)", d is not None and d[1] == BUSY,
          f"err={d[1]}" if d else "TO", f"d={d.hex() if d else 'TO'}")
# 等完成: state 回 IDLE 且 testState=DONE
t0 = time.time()
done = None
while time.time() - t0 < 15:
    done = mquery()
    if done and done[2] == MT_IDLE and done[9] in (MT_DONE, MT_FAULT):
        break
    time.sleep(0.25)
dt = time.time() - t0
ok = done is not None and done[2] == MT_IDLE and done[9] == MT_DONE
rec.check("O3d", "TEST 完成: state 回 IDLE(0), testState=DONE(2)",
          ok, f"{dt:.1f}s state={done[2] if done else '?'} testState={done[9] if done else '?'}",
          f"q={done.hex() if done else 'TO'}")
rec.check("O3e", "完成无故障 (mtReason=0, fault=0)",
          done is not None and done[4] == 0 and done[3] == 0,
          f"mtReason={done[4]} fault={done[3]}" if done else "TO", "")
d, to = tx(lk, FCM, [C.MOTOR_TEST, 1], timeout_s=3)
rec.check("O3f", "完成后立即二次 TEST -> err=0 (无 0703 残留)",
          d is not None and d[1] == 0, "err=0", f"d={d.hex() if d else 'TO'}")
t0 = time.time()
done2 = None
while time.time() - t0 < 15:
    done2 = mquery()
    if done2 and done2[2] == MT_IDLE and done2[9] in (MT_DONE, MT_FAULT):
        break
    time.sleep(0.25)
rec.check("O3g", "二次 TEST 亦完成", done2 is not None and done2[9] == MT_DONE,
          f"testState={done2[9] if done2 else '?'}", f"q={done2.hex() if done2 else 'TO'}")

# ---- O4 QUERY state 重映射: 测试进行中 state=RUN (#4) ----
tx(lk, FCM, [C.MOTOR_TEST, 1])
time.sleep(0.6)
d = mquery()
rec.check("O4a", "TEST 进行中 QUERY: state=1(RUN) + testState=1",
          d is not None and d[2] == MT_RUN and d[9] == MT_RUN,
          f"state={d[2]} testState={d[9]}" if d else "TO", f"q={d.hex() if d else 'TO'}")
t0 = time.time()
while time.time() - t0 < 15:
    d = mquery()
    if d and d[2] == MT_IDLE and d[9] in (MT_DONE, MT_FAULT):
        break
    time.sleep(0.25)
rec.check("O4b", "TEST 结束 QUERY: state 回 IDLE, DONE 不再占位",
          d is not None and d[2] == MT_IDLE and d[9] == MT_DONE,
          f"state={d[2] if d else '?'} testState={d[9] if d else '?'}",
          f"q={d.hex() if d else 'TO'}")
# CLEAR 复位 testState
d, to = tx(lk, FCM, [C.MOTOR_CLEAR], timeout_s=2)
d = mquery()
rec.check("O4c", "CLEAR 后 testState=0", d is not None and d[9] == 0,
          f"testState={d[9]}" if d else "TO", "")

# ---- O5 0x08 单标签流程废除 (Round_011 用户裁决: 单标统一 0x0A epcCnt=1) ----
# 码位保留但不再解析帧形状, 一律显式回 PARAM (区别于未知子命令)。
_epc = list(REAL_EPC) if REAL_EPC else [0xAA] * 12
for name, desc, payload in (
    ("O5a", "旧单窗布局帧", [ONE, 0xE8, 0x03, 0xB8, 0x0B, 0x00]),
    ("O5b", "双窗布局帧 (原 irWait/hold 拆分)", [ONE, 0xE8, 0x03, 0xB0, 0x04, 0xC0, 0x07, 12] + _epc),
    ("O5c", "含 demagCnt 尾字节帧", [ONE, 0xE8, 0x03, 0xB0, 0x04, 0xC0, 0x07, 12] + _epc + [0]),
):
    d, to = tx(lk, FCL, payload, timeout_s=4)
    rec.check(name, f"0x08 废弃: {desc} -> PARAM", d is not None and d[1] == ONE_ERR_PARAM,
              "err=2", f"d={d.hex() if d else 'TO'}")

# ---- O6 S2 会话下 Locker 全周期 x2 (#21) ----
if REAL_EPC:
    d = uhf(C.UHF_GET_CONFIG, t=3)
    cfg_save = bytes(d) if d else None
    ok = cfg_save is not None and len(cfg_save) >= 9
    rec.check("O6a", "GET_CONFIG 基线 (存档待还原)", ok,
              f"sess={cfg_save[5] if ok else '?'}", f"d={d.hex() if d else 'TO'}")
    if ok:
        # session=2 (S2 flag 跨场脉冲保持 — 连续重扫杀手), 其余域保持
        d = uhf(C.UHF_SET_CONFIG,
                [cfg_save[2], cfg_save[3], cfg_save[4], 2, cfg_save[6], cfg_save[7], cfg_save[8]], t=6)
        d = uhf(C.UHF_GET_CONFIG, t=3)
        rec.check("O6b", "SET_CONFIG session=2 生效", d is not None and d[5] == 2,
                  f"sess={d[5]}" if d else "TO", f"d={d.hex() if d else 'TO'}")
        # 标签 Gen2 盘存标志 (含 S0) 仅在标签断电一段时间后清除: 异步 0x22
        # 轮背靠背保持场强, 前置手动盘点会把标志置位 -> 首轮不读. 留 3s
        # 场静默期, 保证每周期首轮读到标签 (业务语义: 匹配首轮即锁存)
        time.sleep(3.0)
        # 两个完整周期: S2 下若未做 ScanSession S0 临时切换, 第 2 周期盘点
        # 将因标签 flag 不复位而静默失败 (P8-S3 曾 1/10 的根因)
        cyc_ok = 0
        for ci in range(2):
            try:
                time.sleep(2.5)   # 周期间场静默: 标签 flag 清除, 首轮必读
                txm(lk, FCL, [CANCEL], timeout_s=4)
                d, _ = txm(lk, FCL, [CFG, 0, 0, 2, 0], timeout_s=4)
                d, _ = txm(lk, FCL, [ADD, 12] + list(REAL_EPC), timeout_s=4)
                d, _ = txm(lk, FCL, [START], timeout_s=4)
                st = None
                t0 = time.time()
                while time.time() - t0 < 45:
                    d, _ = tx(lk, FCL, [QUERY_L], timeout_s=2)
                    if d and len(d) >= 3:
                        st = d[2]
                        if st in (3, 4, 5):
                            break
                    time.sleep(0.25)
                got_match = st in (3, 4)
                if got_match:
                    txm(lk, FCL, [CONSUME], timeout_s=4)
                    txm(lk, FCL, [CONSUME], timeout_s=4)
                    t0 = time.time()
                    while time.time() - t0 < 30:
                        d, _ = tx(lk, FCL, [QUERY_L], timeout_s=2)
                        if d and len(d) >= 3 and d[2] == 0:
                            break
                        time.sleep(0.25)
                    d, _ = tx(lk, FCL, [QUERY_L], timeout_s=2)
                    got_match = got_match and d is not None and d[2] == 0
                if got_match:
                    cyc_ok += 1
                print(f"  S2 周期#{ci+1}: {'OK' if got_match else f'FAIL state={st}'}", flush=True)
            except Exception as e:
                print(f"  S2 周期#{ci+1}: EXC {e}", flush=True)
            txm(lk, FCL, [CANCEL], timeout_s=4)
        rec.check("O6c", "S2 会话下连续 2 周期均完成匹配闭环 (ScanSession S0 生效)",
                  cyc_ok == 2, f"{cyc_ok}/2", "")
        # 还原配置: 先等 locker 完全回 IDLE — teardown 异步, ScanSessionEnd
        # 在状态机收尾时才还原 session, 抢跑 SET 会被模块忙 (LINK) 吞掉持久化
        t0 = time.time()
        while time.time() - t0 < 10:
            d, _ = tx(lk, FCL, [QUERY_L], timeout_s=2)
            if d and len(d) >= 3 and d[2] == 0:
                break
            time.sleep(0.25)
        if cfg_save:
            rs = None
            for _ in range(3):
                rs = uhf(C.UHF_SET_CONFIG,
                         [cfg_save[2], cfg_save[3], cfg_save[4], cfg_save[5],
                          cfg_save[6], cfg_save[7], cfg_save[8]], t=6)
                if rs is not None and rs[1] == 0:
                    break
                time.sleep(0.5)
            d = uhf(C.UHF_GET_CONFIG, t=3)
            rec.check("O6d", "配置还原 (SET err=0)", d is not None and d[5] == cfg_save[5],
                      f"sess={d[5]} setErr={rs[1] if rs else 'TO'}" if d else "TO", "")
else:
    rec.skip("O6", "S2 会话 Locker 周期", "无在场标签")

# ---- O7 复位后后台回零 (#9/#11/#3) ----
d, to = tx(lk, C.FC_RESET, [], timeout_s=2)
lk.close()
lk, el = reopen_until_alive(30)
rec.check("O7a", "复位后 App 重连 (≤12s)", lk is not None and el <= 12.0,
          f"{el:.1f}s" if lk else "TO", f"{el:.1f}s")
if lk:
    d = iod(2)
    rec.check("O7b", "重连即 UHF 已上电 (#9: POST 先于 USB 就绪完成恢复)",
              d is not None and d[4] == 1, f"uhfPwr={d[4]}" if d else "TO",
              f"d={d.hex() if d else 'TO'}")
    hom_first = d[7] if d else None
    if hom_first == 1:
        d, to = tx(lk, FCM, [C.MOTOR_MOVE, 0x00, 100, 0, 0], timeout_s=2)
        rec.check("O7c", "回零进行中 MOVE -> BUSY(3) (#3 互斥)",
                  d is not None and d[1] == BUSY, f"err={d[1]}" if d else "TO",
                  f"d={d.hex() if d else 'TO'}")
    else:
        rec.obs("O7c", "重连时回零已完成/未运行, 跳过 MOVE-BUSY 采样",
                f"homing={hom_first}")
    t0 = time.time()
    ready = False
    while time.time() - t0 < 20:
        d = iod(2)
        if d and d[7] == 2:
            ready = True
            break
        time.sleep(0.5)
    rec.check("O7d", "后台回零达 READY(2) (#11)", ready,
              f"{time.time()-t0:.1f}s" if ready else "20s 未 READY", "")
    if ready:
        d, to = tx(lk, FCM, [C.MOTOR_MOVE, 0x00, 100, 0, 0], timeout_s=2)
        ok_mv = d is not None and d[1] == 0
        rec.check("O7e", "READY 后 MOVE 恢复受理", ok_mv, "err=0",
                  f"d={d.hex() if d else 'TO'}")
        if ok_mv:
            t0 = time.time()
            while time.time() - t0 < 5:
                d = mquery()
                if d and d[2] == MT_IDLE:
                    break
                time.sleep(0.2)
    lk.close()

n = rec.flush()
print("== P9-OptFix 完成 ==")
sys.exit(0 if n["FAIL"] == 0 else 1)
