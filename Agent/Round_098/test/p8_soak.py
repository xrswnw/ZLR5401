"""P8: 稳态压力 — S1 30min 混合浸泡 / S2 复位循环×5 / S3 Locker 全周期×10"""
import random
import sys
import time

from lib import (Recorder, open_link, tx, txm, drain, reopen_until_alive, C,
                 load_hw, locker_idle, parse_selftest, bits_str,
                 FC_SELFTEST_CTRL, AM_QUERY)

rec = Recorder("P8-Soak")
hw = load_hw()
REAL_EPC = bytes.fromhex(hw["uhf_tags"][0]) if hw.get("uhf_tags") else None

FC = C.FC_LOCKER_CTRL
CFG, ADD, START, CANCEL, QUERY, CONSUME, GETEV = (
    C.LOCKER_CONFIGURE, C.LOCKER_ADD, C.LOCKER_START, C.LOCKER_CANCEL,
    C.LOCKER_QUERY, C.LOCKER_CONSUME_SOFT, C.LOCKER_GET_EVENT)
ST_IDLE, ST_CFG, ST_SOFT, ST_DONE = (
    C.LOCKER_STATE_IDLE, C.LOCKER_STATE_CONFIGURED, C.LOCKER_STATE_SOFT_DECODE,
    C.LOCKER_STATE_DONE)
ST_LOWER = 6   # LOWERING (zlr/const 未定义)
EV_MATCH, EV_HARD_DONE, EV_SOFT_USED, EV_DONE = (
    C.LOCKER_EVT_MATCH_OK, C.LOCKER_EVT_HARD_DONE, C.LOCKER_EVT_SOFT_USED,
    C.LOCKER_EVT_DONE)

SELFTEST_SUB_QUERY = 0x01


def a(lk, sub, data=None, t=6):
    d, to = txm(lk, FC, [sub] + (data or []), timeout_s=t)
    return d


def q(lk):
    d, _ = tx(lk, FC, [QUERY], timeout_s=2)
    if d and len(d) >= 8:
        return (d[2], d[3] | (d[4] << 8), d[5] | (d[6] << 8), d[7] | (d[8] << 8))
    return None


def mq(lk):
    d, _ = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_QUERY], timeout_s=2)
    return d[2] if d and len(d) > 2 else None


def selftest(lk):
    d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_QUERY], timeout_s=3)
    return parse_selftest(d if not to else None)


# ============ S1 混合浸泡 (30 min) ============
SOAK_MIN = 30.0
lk = open_link()


def op_handshake():
    d, _ = tx(lk, C.FC_HANDSHAKE, [], timeout_s=2)
    if d is None:
        return "fail"
    return "ok" if len(d) > 19 and d[19] == 1 else "drift"


def op_motor():
    d, _ = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_QUERY], timeout_s=2)
    return "ok" if d is not None else "fail"


def op_selftest():
    st = selftest(lk)
    if st is None:
        return "fail"
    return "ok" if st["errBits"] == 0 else "dirty"


def op_uhf():
    d, _ = tx(lk, C.FC_UHF_CTRL, [C.UHF_QUERY], timeout_s=3)
    return "ok" if d is not None else "fail"   # 关态回 LINK/NOT_READY 均算存活


def op_am():
    d, to = txm(lk, C.FC_AM_CTRL, [AM_QUERY], timeout_s=3)
    return "ok" if d is not None else "fail"


def op_locker():
    d, _ = tx(lk, FC, [QUERY], timeout_s=2)
    return "ok" if d is not None else "fail"


def op_device_info():
    d, _ = tx(lk, C.FC_DEVICE_INFO, [], timeout_s=2)
    return "ok" if d is not None else "fail"


OPS = [(op_handshake, 20), (op_motor, 15), (op_selftest, 8), (op_uhf, 8),
       (op_am, 8), (op_locker, 12), (op_device_info, 10)]
_weighted = [f for f, w in OPS for _ in range(w)]

stats = {"ok": 0, "fail": 0, "drift": 0, "dirty": 0}
lat_max = 0.0
t_soak = time.time()
n_ops = 0
print(f"S1 浸泡开始: {SOAK_MIN} min 混合负载")
while time.time() - t_soak < SOAK_MIN * 60:
    op = random.choice(_weighted)
    t0 = time.time()
    try:
        r = op()
    except Exception:
        r = "fail"
    lat = time.time() - t0
    lat_max = max(lat_max, lat)
    stats[r] = stats.get(r, 0) + 1
    n_ops += 1
    if n_ops % 200 == 0:
        el = (time.time() - t_soak) / 60
        print(f"  浸泡 {el:.1f}/{SOAK_MIN}min ops={n_ops} {stats} maxLat={lat_max:.2f}s", flush=True)
    time.sleep(random.uniform(0.2, 1.2))

soak_ok = stats["fail"] == 0 and stats["drift"] == 0 and stats["dirty"] == 0
rec.check("S1", f"浸泡 {SOAK_MIN}min 混合负载 ({n_ops} 笔)",
          soak_ok,
          f"ok={stats['ok']} fail={stats['fail']} drift={stats['drift']} "
          f"selftest脏={stats['dirty']} maxLat={lat_max:.2f}s",
          f"{stats}")
lk.close()

# ============ S2 复位循环 ×5 ============
print("S2 复位循环 ×5")
for i in range(5):
    lk = open_link()
    d, to = tx(lk, C.FC_RESET, [], timeout_s=2)
    lk.close()
    lk2, dt = reopen_until_alive(30)
    ok = lk2 is not None
    st_ok = mq_ok = False
    if lk2:
        for _ in range(20):          # 等回零完成
            if mq(lk2) == 0:
                mq_ok = True
                break
            time.sleep(1.0)
        st = selftest(lk2)
        st_ok = st is not None and st["errBits"] == 0
        lk = lk2
    rec.check(f"S2r{i+1}", f"复位#{i+1}: 重连{dt:.1f}s/自检干净/电机IDLE",
              ok and mq_ok and st_ok,
              f"重连{dt:.1f}s motor={'IDLE' if mq_ok else 'BUSY'} "
              f"selftest={'clean' if st_ok else 'dirty'}",
              f"重连{'失败' if not ok else f'{dt:.1f}s'}")
    if lk2:
        lk2.close()

# ============ S3 Locker 全周期 ×10 ============
if REAL_EPC is None:
    rec.skip("S3", "Locker 全周期×10", "无在场标签, SKIP")
    sys.exit(0)
print("S3 Locker 全周期 ×10 (真标签+电机升降)")
cycle_ok = 0
fail_notes = []
for i in range(10):
    lk = open_link()
    locker_idle(lk)
    ok_cycle = True
    note = ""
    try:
        d = a(lk, CFG, [0, 0, 2, 0])
        d = a(lk, ADD, [12] + list(REAL_EPC))
        if not (d is not None and d[1] == 0):
            ok_cycle, note = False, f"ADD err={d[1] if d else 'TO'}"
        d = a(lk, START)
        if ok_cycle and not (d is not None and d[1] == 0):
            ok_cycle, note = False, f"START err={d[1] if d else 'TO'}"
        # 等 SOFT_DECODE (IR 门控 + 匹配; 台面 PC4 噪声容忍 45s)
        st = None
        t0 = time.time()
        while time.time() - t0 < 45:
            st = q(lk)
            if st and st[0] in (ST_SOFT, ST_DONE):
                break
            if st and st[0] == 5:    # FAULT
                break
            time.sleep(0.25)
        if ok_cycle and not (st and st[0] == ST_SOFT):
            ok_cycle, note = False, f"状态={st[0] if st else 'TO'} (期望 SOFT=3)"
        # 消费×2 -> DONE
        if ok_cycle:
            a(lk, CONSUME)
            a(lk, CONSUME)
            t0 = time.time()
            while time.time() - t0 < 25:
                st = q(lk)
                if st and st[0] in (ST_DONE, ST_LOWER, ST_IDLE):
                    break
                time.sleep(0.25)
            if not (st and st[0] in (ST_DONE, ST_LOWER, ST_IDLE)):
                ok_cycle, note = False, f"消费后状态={st[0] if st else 'TO'}"
        # 等回 IDLE (含回降)
        t0 = time.time()
        while time.time() - t0 < 30:
            st = q(lk)
            if st and st[0] == ST_IDLE:
                break
            time.sleep(0.25)
        # 事件收割
        evs = []
        for _ in range(8):
            d, _ = txm(lk, FC, [GETEV], timeout_s=2)
            if d is None or len(d) < 2 or d[1] == C.LOCKER_ERR_NO_EVENT:
                break
            evs.append(d[2])
        if ok_cycle and not (evs.count(EV_SOFT_USED) == 2 and EV_DONE in evs
                             and EV_MATCH in evs):
            ok_cycle, note = False, f"事件={evs}"
        if ok_cycle and mq(lk) != 0:
            ok_cycle, note = False, "结束后电机非IDLE"
    except Exception as e:
        ok_cycle, note = False, f"异常 {e}"
    if ok_cycle:
        cycle_ok += 1
    else:
        fail_notes.append(f"#{i+1}:{note}")
    locker_idle(lk)
    lk.close()
    print(f"  周期#{i+1}: {'OK' if ok_cycle else 'FAIL ' + note}", flush=True)
    time.sleep(1.0)

rec.check("S3", f"Locker 全周期×10 (真标签+电机)",
          cycle_ok == 10,
          f"{cycle_ok}/10 完整闭环", "; ".join(fail_notes) or "全绿")

rec.flush()
print("== P8-Soak 完成 ==")
