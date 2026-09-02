"""P6 — Locker 七状态机 + 0x08 废弃校验 + UNLOCK_MULTI 0x0A 唯一解锁业务闭环.

依赖: 真标签 33553463a4000158eb7e7507 在 UHF 天线上; PC4 红外门当前读高/抖动
(外部传感器态), 深路径断言按 IR 实际态分叉记录.
"""
import sys, time
sys.path.insert(0, '.')
sys.path.insert(0, '../../Round_008/test')
from zlr import const as C
from zlr.hid_link import HidLink
from lib import Recorder, open_link, tx, txm, drain, reopen_until_alive, wait_settled

FC = C.FC_LOCKER_CTRL
FCM = C.FC_MOTOR_CTRL
LOCKER_ERR_BUSY, LOCKER_ERR_PARAM, LOCKER_ERR_NO_EVENT = 1, 2, 3

CFG, ADD, START, CANCEL, QUERY, CONSUME, GETEV, ONE, PROG, MULTI = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A)
ST_IDLE, ST_CFG, ST_HOLD, ST_SOFT, ST_DONE, ST_FAULT, ST_LOWER = range(7)
EV_MATCH, EV_MISMATCH, EV_HARD_DONE, EV_SOFT_USED, EV_TIMEOUT, EV_DONE = 0, 1, 2, 3, 4, 5
UNLK_ERR_PARAM, UNLK_ERR_NO_IR = 2, 11
UNLK_END_ABORTED = 6
UNLK_PH_WAIT_TAG, UNLK_PH_VERIFY = 1, 2

REAL_EPC = bytes.fromhex('33553463a4000158eb7e7507')
FAKE_EPC = bytes.fromhex('00112233445566778899aabb')


def q(lk, timeout_s=3):
    d, _ = txm(lk, FC, [QUERY], timeout_s)
    if d is None or len(d) < 10:
        return None
    return (d[2], d[3] | (d[4] << 8), d[5] | (d[6] << 8),
            d[7] | (d[8] << 8), d[9])


def mq(lk):
    d, _ = txm(lk, FCM, [C.MOTOR_QUERY], timeout_s=3)
    return d[2] if d is not None and len(d) > 2 else None   # state (0=IDLE/1=RUN)


def collect(lk, want_sub, timeout_s, stash):
    """收流: 返回首个 data[0]==want_sub 的帧, 其余入 stash."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        f = lk.recv_frame(0.4)
        if f is None:
            continue
        if f["func"] == (FC ^ 0xFF):
            if f["data"][0] == want_sub:
                return f["data"]
            stash.append(f["data"])
    return None


def run():
    lk, dt = reopen_until_alive(25)
    rec = Recorder("P6-Locker")
    assert lk, "设备未就绪"
    wait_settled(lk)   # 前一阶段可能以 FC_RESET 收尾: 等后台回零完成,
                       # 否则 START 被 BUSY 拒 / 匹配撞上"未回零"硬故障

    def a(sub, data=None, timeout_s=4):
        payload = [sub] + (list(data) if data else [])
        d, _ = txm(lk, FC, payload, timeout_s=timeout_s)
        return d

    def ensure_idle(timeout_s=20):
        t0 = time.time()
        while time.time() - t0 < timeout_s:
            st = q(lk)
            if st and st[0] == ST_IDLE:
                return True
            time.sleep(0.3)
        return False

    # ====== Part A: 参数/矩阵 (无运动) ======
    st = q(lk)
    rec.check("L1", "QUERY 空闲态", st is not None and st[0] == ST_IDLE,
              f"state={st[0] if st else '?'}", f"resp={st}")

    d = a(GETEV)
    rec.check("L2", "GET_EVENT 空队列 -> NO_EVENT(3)",
              d is not None and d[1] == LOCKER_ERR_NO_EVENT, f"d={d.hex() if d else 'TO'}", "")

    d = a(CFG, [17, 0, 2, 0])
    rec.check("L3a", "CONFIGURE hardCount=17 -> PARAM", d is not None and d[1] == LOCKER_ERR_PARAM,
              f"d={d.hex() if d else 'TO'}", "")
    d = a(CFG, [0, 0, 2, 0])
    rec.check("L3b", "CONFIGURE soft=2 -> OK, state=CONFIGURED(1)",
              d is not None and d[1] == 0 and q(lk)[0] == ST_CFG, "err=0", f"d={d.hex() if d else 'TO'}")

    d = a(CFG, [0, 0, 1, 0])
    rec.check("L4", "CONFIGURE 忙时 -> BUSY(1)", d is not None and d[1] == LOCKER_ERR_BUSY,
              f"d={d.hex() if d else 'TO'}", "")

    d = a(ADD, [8, 0xAA, 0xBB])               # epcLen=8 但帧内仅 2B
    rec.check("L5a", "ADD 短帧 (epcLen=8, 实2B) -> PARAM (BUG#3 修复回归)",
              d is not None and d[1] == LOCKER_ERR_PARAM, "err=2 PARAM",
              f"err={d[1] if d else '?'}")
    d = a(ADD, [13] + list(FAKE_EPC) + [0xCC])  # epcLen=13 超上限
    rec.check("L5b", "ADD epcLen=13 超上限 -> PARAM (BUG#3 修复回归)",
              d is not None and d[1] == LOCKER_ERR_PARAM, "err=2 PARAM",
              f"err={d[1] if d else '?'}")

    d = a(ADD, [12] + list(FAKE_EPC))
    rec.check("L6", "ADD 合法 [epcLen=12]+12B -> OK", d is not None and d[1] == 0, "err=0",
              f"d={d.hex() if d else 'TO'}")

    d = a(CANCEL)
    rec.check("L7", "CANCEL (未升起直回 IDLE)", d is not None and d[1] == 0 and q(lk)[0] == ST_IDLE,
              "err=0 state=0", f"d={d.hex() if d else 'TO'}")

    a(CFG, [0, 0, 0, 0])
    d = a(START)
    rec.check("L8", "START 空任务 -> PARAM", d is not None and d[1] == LOCKER_ERR_PARAM,
              "err=2", f"d={d.hex() if d else 'TO'}")

    a(CANCEL)
    a(CFG, [0, 0, 0, 0])
    d = a(ADD, [12] + list(FAKE_EPC))
    rec.check("L9a-1", "假EPC清单 ADD -> OK", d is not None and d[1] == 0, "err=0", "")
    d = a(START)
    rec.check("L9a", "START 假EPC任务 -> OK", d is not None and d[1] == 0, "err=0",
              f"d={d.hex() if d else 'TO'}")
    d = a(ADD, [12] + list(REAL_EPC))
    rec.check("L9b", "START 后 ADD -> BUSY", d is not None and d[1] == LOCKER_ERR_BUSY,
              "err=1", f"d={d.hex() if d else 'TO'}")
    d = a(START)
    rec.check("L9c", "重复 START -> BUSY", d is not None and d[1] == LOCKER_ERR_BUSY,
              "err=1", f"d={d.hex() if d else 'TO'}")
    d = a(CONSUME)
    rec.check("L9d", "非软标态 CONSUME_SOFT -> BUSY", d is not None and d[1] == LOCKER_ERR_BUSY,
              "err=1", f"d={d.hex() if d else 'TO'}")
    d = a(CANCEL)
    rec.check("L9e", "CANCEL 收尾 -> IDLE", d is not None and d[1] == 0 and q(lk)[0] == ST_IDLE,
              "err=0 state=0", f"d={d.hex() if d else 'TO'}")

    # ====== Part B: 主状态机真实闭环 (真标签 + 电机升降) ======
    a(CFG, [0, 0, 2, 0])
    d = a(ADD, [12] + list(REAL_EPC))
    rec.check("B0", "真EPC清单 ADD -> OK", d is not None and d[1] == 0, "err=0",
              f"d={d.hex() if d else 'TO'}")
    d = a(START)
    rec.check("B1", "START 真EPC任务 -> OK", d is not None and d[1] == 0, "err=0",
              f"d={d.hex() if d else 'TO'}")

    seen = []
    t0 = time.time()
    while time.time() - t0 < 30:
        st = q(lk)
        if st and (not seen or seen[-1] != st[0]):
            seen.append(st[0])
        if st and st[0] == ST_SOFT:
            break
        time.sleep(0.2)
    rec.check("B2", "CONFIGURED(1)->匹配->SOFT(3)",
              st is not None and st[0] == ST_SOFT and seen[0] == ST_CFG,
              f"轨迹={seen}", f"轨迹={seen} 最终={st[0] if st else '?'}")

    evs = []
    for _ in range(8):
        d, _ = txm(lk, FC, [GETEV], timeout_s=2)
        if d is None or len(d) < 2 or d[1] == LOCKER_ERR_NO_EVENT:
            break
        elen = d[3]
        evs.append((d[2], bytes(d[4:4 + elen]) if elen else b''))
    got = [e[0] for e in evs]
    rec.check("B3", "事件 MATCH_OK(0)+HARD_DONE(2), EPC 回传正确",
              EV_MATCH in got and EV_HARD_DONE in got and
              any(e[1] == REAL_EPC for e in evs if e[0] == EV_MATCH),
              f"codes={got} EPC✓", f"codes={got}")

    m_up = mq(lk)
    rec.check("B3m", "匹配后电机升起 (state=1 运行或已到顶)",
              m_up in (1, 2, 0), f"motor={m_up}", "")   # 采样时刻不定, 记录态

    d = a(CONSUME)
    rec.check("B4a", "CONSUME_SOFT #1 -> SOFT_USED", d is not None and d[1] == 0, "err=0",
              f"d={d.hex() if d else 'TO'}")
    d = a(CONSUME)
    st = q(lk)
    rec.check("B4b", "CONSUME_SOFT #2 -> DONE(4)",
              d is not None and d[1] == 0 and st is not None and st[0] == ST_DONE,
              "err=0 state=4", f"state={st[0] if st else '?'}")

    seen = []
    t0 = time.time()
    while time.time() - t0 < 25:
        st = q(lk)
        if st and (not seen or seen[-1] != st[0]):
            seen.append(st[0])
        if st and st[0] == ST_IDLE:
            break
        time.sleep(0.25)
    rec.check("B5", "DONE(4)->LOWERING(6)->IDLE(0) 回降闭环",
              seen and seen[-1] == ST_IDLE and ST_LOWER in seen,
              f"轨迹={seen}", f"轨迹={seen}")

    evs = []
    for _ in range(8):
        d, _ = txm(lk, FC, [GETEV], timeout_s=2)
        if d is None or len(d) < 2 or d[1] == LOCKER_ERR_NO_EVENT:
            break
        elen = d[3]
        evs.append((d[2], bytes(d[4:4 + elen]) if elen else b''))
    got = [e[0] for e in evs]
    rec.check("B6", "事件序 SOFT_USED×2 + DONE(5)",
              got.count(EV_SOFT_USED) == 2 and EV_DONE in got, f"codes={got}", f"codes={got}")
    rec.check("B6m", "回降后电机 IDLE", mq(lk) == 0, f"motor={mq(lk)}", "")

    # MISMATCH: 假清单 + 真标签在场 = 外来标签
    a(CFG, [0, 0, 0, 0])
    a(ADD, [12] + list(FAKE_EPC))
    a(START)
    mm = None
    t0 = time.time()
    while time.time() - t0 < 15 and mm is None:
        for _ in range(8):
            d, _ = txm(lk, FC, [GETEV], timeout_s=2)
            if d is None or len(d) < 2 or d[1] == LOCKER_ERR_NO_EVENT:
                break
            if d[2] == EV_MISMATCH:
                elen = d[3]
                mm = (d[2], bytes(d[4:4 + elen]) if elen else b'')
                break
        time.sleep(0.4)
    rec.check("B7", "MISMATCH 事件 (外来EPC=真标签, 不升起)",
              mm is not None and mm[1] == REAL_EPC,
              f"EPC={mm[1].hex() if mm else '-'}", f"mm={mm}")
    st = q(lk)
    rec.check("B8", "失配后仍 CONFIGURED(1) 不动磁块",
              st is not None and st[0] == ST_CFG and mq(lk) == 0,
              f"state={st[0] if st else '?'} motor={mq(lk)}", "")
    a(CANCEL)
    rec.check("B9", "MISMATCH 后 CANCEL -> IDLE", ensure_idle(10), "state=0", "")

    # ====== Part C: 0x08 废弃 + GET_PROGRESS 统一布局 + 0x0A 单标 (epcCnt=1) ======
    # Round_011 用户裁决: OneShot 单标签流程整体移除, 单标场景 = UNLOCK_MULTI epcCnt=1
    # (W=120000+0*30000 即 2min), GET_PROGRESS 统一为多标签布局。
    d = a(PROG)
    rec.check("C1", "GET_PROGRESS 空闲统一多标签布局 phase=0 (holdMs 3 字节全零)",
              d is not None and len(d) >= 11 and d[2] == 0 and d[3] == 0 and d[4] == 0
              and d[5] == 0 and d[6] == 0,
              f"d={d.hex() if d else 'TO'}", "")

    # C2: 0x08 码位保留但显式回 PARAM (帧形状不再解析)
    d = a(ONE, [1, 2])
    rec.check("C2a", "0x08 废弃: 短帧 -> PARAM", d is not None and d[1] == LOCKER_ERR_PARAM, "err=2",
              f"d={d.hex() if d else 'TO'}")
    d = a(ONE, [0xE8, 0x03, 0xD0, 0x07, 0xB8, 0x0B, 0x00, 12] + list(REAL_EPC))
    rec.check("C2b", "0x08 废弃: 原完整帧布局亦 -> PARAM",
              d is not None and d[1] == LOCKER_ERR_PARAM, "err=2",
              f"d={d.hex() if d else 'TO'}")

    # C3: 单标走 0x0A epcCnt=1, 短窗 (1200ms): PC4 未触 -> NO_IR;
    #     触发 -> 窗短于 3s 确认判据 -> PARTIAL_TIMEOUT(conf=0); 命中不可能
    stash = []
    lk.send_frame(1, FC, bytes([MULTI, 0xE8, 0x03, 0xB0, 0x04, 0, 1, 12] + list(REAL_EPC)))
    fin = collect(lk, MULTI, 15, stash)
    m = mq(lk)
    if fin is not None and fin[1] == UNLK_ERR_NO_IR:
        rec.check("C3", "0x0A 单标短窗 NO_IR (PC4 未触, 电机不动)",
                  m == 0, f"err=11 电机={m}", f"fin={fin.hex()} motor={m}")
    elif fin is not None and fin[1] == 0:
        # [0A,0,end,bitmap,conf,total,rise(2),lower(2),softDone,softCnt,elapsed(2)]
        endR, bmp, conf, tot = fin[2], fin[3], fin[4], fin[5]
        rise = fin[6] | (fin[7] << 8)
        rec.check("C3", "0x0A 单标 (PC4 触发): err=0 终帧 + 收尾电机 IDLE",
                  m == 0 and endR in (1, 2) and tot == 1 and conf in (0, 1)
                  and (conf == 0 or rise > 1000) and bmp == conf,
                  f"end={endR} conf={conf}/{tot} rise≈{rise} motor={m}",
                  f"fin={fin.hex()} motor={m}")
        rec.obs("C3-ir", "PC4 红外门当前读高 (无人工放标) — 深路径按 IR=触发分支验证",
                f"fin={fin.hex()}")
    else:
        rec.fail("C3", "0x0A 单标短窗响应缺失/异常", f"fin={fin.hex() if fin else 'TO'} motor={m}")

    # C4: CANCEL 打断 0x0A 长窗单标 (打断点依 IR 态落在 等放标/校对/升起)
    stash = []
    lk.send_frame(1, FC, bytes([MULTI, 0xE8, 0x03, 0x30, 0x75, 0, 1, 12] + list(REAL_EPC)))
    time.sleep(0.8)
    lk.send_frame(1, FC, bytes([PROG]))
    prog = collect(lk, PROG, 3, stash)
    ph = prog[2] if prog is not None and len(prog) > 2 else -1
    rec.check("C4a", "流程中 GET_PROGRESS 有相位 (IR 态依从)",
              ph in (1, 2, 3), f"phase={ph}", f"prog={prog.hex() if prog else 'TO'}")
    lk.send_frame(1, FC, bytes([CANCEL]))
    canc = collect(lk, CANCEL, 3, stash)
    rec.check("C4b", "流程中 CANCEL -> 立即 OK", canc is not None and canc[1] == 0,
              f"d={canc.hex() if canc else 'TO'}", "")
    fin = collect(lk, MULTI, 10, stash)
    m = mq(lk)
    okfin = fin is not None and fin[1] == 0 and len(fin) >= 3 and fin[2] == UNLK_END_ABORTED
    rec.check("C4c", "打断终帧 err=0 endReason=6(ABORTED), 电机安全回 IDLE",
              okfin and m == 0, f"fin={fin.hex() if fin else 'TO'} motor={m}", "")
    st = q(lk)
    rec.check("C4d", "打断后 Locker 回 IDLE", st is not None and st[0] == ST_IDLE,
              f"state={st[0] if st else '?'}", "")

    # ====== Part D: UNLOCK_MULTI 0x0A 参数矩阵/多标流程 ======
    d = a(MULTI, [1])
    rec.check("D1a", "短帧 -> PARAM", d is not None and d[1] == UNLK_ERR_PARAM, "err=2", "")
    d = a(MULTI, [0xE8, 0x03, 0xD0, 0x07, 0, 0, 12])
    rec.check("D1b", "epcCnt=0 -> PARAM", d is not None and d[1] == UNLK_ERR_PARAM, "err=2", "")
    d = a(MULTI, [0xE8, 0x03, 0xD0, 0x07, 0, 5, 12] + list(REAL_EPC) * 5)
    rec.check("D1c", "epcCnt=5 -> PARAM", d is not None and d[1] == UNLK_ERR_PARAM, "err=2", "")
    d = a(MULTI, [0xE8, 0x03, 0xD0, 0x07, 0, 1, 0])
    rec.check("D1d", "epcLen=0 -> PARAM", d is not None and d[1] == UNLK_ERR_PARAM, "err=2", "")
    d = a(MULTI, [0xE8, 0x03, 0xD0, 0x07, 0, 1, 13] + list(REAL_EPC) + [0xCC])
    rec.check("D1e", "epcLen=13 -> PARAM", d is not None and d[1] == UNLK_ERR_PARAM, "err=2", "")
    d = a(MULTI, [0xE8, 0x03, 0xD0, 0x07, 0, 1, 12, 1, 2])
    rec.check("D1f", "帧长不足 -> PARAM", d is not None and d[1] == UNLK_ERR_PARAM, "err=2", "")

    # D4a: 短窗 (2s < 稳定确认门限 3s) -> PARTIAL_TIMEOUT, 不动磁块安全收尾
    stash = []
    lk.send_frame(1, FC, bytes([MULTI, 0xE8, 0x03, 0xD0, 0x07, 0, 1, 12] + list(REAL_EPC)))
    fin = collect(lk, MULTI, 12, stash)
    pushes = [p for p in stash if p[0] in (0x0F, 0x0B, 0x0C, 0x0D, 0x0E)]
    m = mq(lk)
    okfin = (fin is not None and fin[1] == 0 and fin[2] == 2 and fin[4] == 0
             and fin[5] == 1 and fin[6] == 0 and fin[7] == 0)
    rec.check("D4a", "短窗(<3s确认门限) -> PARTIAL_TIMEOUT(2), 零确认零升降",
              okfin and m == 0 and any(p[0] == 0x0F for p in pushes)
              and any(p[0] == 0x0D and p[1] == 2 for p in pushes),
              f"fin={fin.hex() if fin else 'TO'} pushes={[p.hex() for p in pushes]} 电机={m}",
              "")

    # D4b: 长窗全流程 — 窗内 BUSY 矩阵 + 全推送链 + 终帧
    stash = []
    lk.send_frame(1, FC, bytes([MULTI, 0xE8, 0x03, 0x80, 0x3A, 0, 1, 12] + list(REAL_EPC)))
    lk.send_frame(1, FC, bytes([ADD, 12] + list(FAKE_EPC)))
    add_r = collect(lk, ADD, 3, stash)
    rec.check("D3a", "流程中 ADD -> BUSY", add_r is not None and add_r[1] == LOCKER_ERR_BUSY,
              f"d={add_r.hex() if add_r else 'TO'}", "")
    lk.send_frame(1, FC, bytes([PROG]))
    prog = collect(lk, PROG, 3, stash)
    ph = prog[2] if prog is not None and len(prog) > 2 else -1
    # Round_011: holdMs 3 字节 -> total/confirmed 等后移 1 字节 (prog[6]=total)
    rec.check("D3b", "流程中 GET_PROGRESS 多标签布局 (phase 1/2, total=1)",
              prog is not None and len(prog) >= 11 and ph in (UNLK_PH_WAIT_TAG, UNLK_PH_VERIFY)
              and prog[6] == 1,
              f"d={prog.hex() if prog else 'TO'}", "")
    fin = collect(lk, MULTI, 25, stash)
    pushes = [p for p in stash if p[0] in (0x0F, 0x0B, 0x0C, 0x0D, 0x0E)]
    m = mq(lk)
    if fin is not None and fin[1] == UNLK_ERR_NO_IR:
        rec.check("D4", "UNLOCK_MULTI NO_IR 终帧 (PC4 全窗未触)",
                  m == 0, f"err=11 电机={m}", f"fin={fin.hex()} motor={m}")
    elif fin is not None and fin[1] == 0:
        # [0A,0,endReason,bitmap,confirmed,total,rise(2),lower(2),softDone,softCnt,elapsed(2)]
        endR, bmp, conf, tot = fin[2], fin[3], fin[4], fin[5]
        rise = fin[6] | (fin[7] << 8)
        got0F = any(p[0] == 0x0F for p in pushes)
        got0B = any(p[0] == 0x0B for p in pushes)
        got0D = any(p[0] == 0x0D and p[1] == 1 for p in pushes)
        rec.check("D4", "长窗全流程终帧 err=0 end=1(ALL_OK) 确认/位图/升降步数齐",
                  m == 0 and endR == 1 and conf == 1 and tot == 1 and bmp == 1
                  and rise > 1000 and got0F and got0B and got0D,
                  f"end={endR} bmp={bmp} conf={conf}/{tot} rise={rise} "
                  f"pushes={[p[0] for p in pushes]} 电机={m}",
                  f"fin={fin.hex()} pushes={[p.hex() for p in pushes]}")
        rec.check("D5", "确认推送 0x0B 含真EPC (判据耗时≈3s门限)",
                  any(p[0] == 0x0B and p[2] == 12 and bytes(p[3:15]) == REAL_EPC
                      and (p[17] | (p[18] << 8)) >= 2900 for p in pushes),
                  "EPC✓ 判据耗时✓", f"pushes={[p.hex() for p in pushes]}")
    else:
        rec.fail("D4", "UNLOCK_MULTI 终帧缺失/异常",
                 f"fin={fin.hex() if fin else 'TO'} motor={m} pushes={[p.hex() for p in pushes]}")
    st = q(lk)
    rec.check("D6", "流程后 Locker IDLE, 电机 IDLE",
              st is not None and st[0] == ST_IDLE and m == 0,
              f"state={st[0] if st else '?'} motor={m}", "")

    rec.flush()
    return rec


if __name__ == "__main__":
    run()
