#!/usr/bin/env python3
"""App_Locker 多状态流闭环测试 — 约束六条业务状态机 (Round_078 修复版).

覆盖: CONFIGURED→首匹配→UNLOCK_HOLD(寻触升起)→m==n→SOFT_DECODE→
CONSUME_SOFT→DONE→(5s)→LOWERING→IDLE;
MISMATCH(非清单EPC)不升起; 已解锁标签重复读不误报;
CANCEL 升起中途打断→LOWERING→IDLE; 纯软标任务直入 SOFT;
START 重复激活回 BUSY; ADD 在 START 后回 BUSY。

用法: python3 locker_sm.py   (感应区需有真实标签, 全程勿移动)
"""
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr import const                     # noqa: E402

FC = const.FC_LOCKER_CTRL
ST = {0: "IDLE", 1: "CONFIGURED", 2: "UNLOCK_HOLD", 3: "SOFT_DECODE",
      4: "DONE", 5: "FAULT", 6: "LOWERING"}
EV = {0: "MATCH_OK", 1: "MISMATCH", 2: "HARD_DONE", 3: "SOFT_USED",
      4: "TIMEOUT", 5: "DONE", 6: "FAULT"}

results = []


def record(name, ok, msg):
    results.append((name, ok, msg))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")


def tx(link, data, t=3.0):
    return link.transaction(FC, bytes(data), timeout_s=t)


def query(link):
    r, _ = tx(link, [const.LOCKER_QUERY])
    if not r or r["data"][1] != 0:
        return None
    d = r["data"]
    return {"state": d[2],
            "hm": d[3] | (d[4] << 8),
            "sc": d[5] | (d[6] << 8),
            "su": d[7] | (d[8] << 8)}


def drain_events(link, tmo_s=1.0):
    """拉空事件队列, 返回 [事件码,...]"""
    evs = []
    t0 = time.time()
    while time.time() - t0 < tmo_s:
        r, _ = tx(link, [const.LOCKER_GET_EVENT], t=1.5)
        if not r:
            break
        d = r["data"]
        if d[1] == const.LOCKER_ERR_NO_EVENT:
            break
        evs.append(d[2])
    return evs


def wait_state(link, want, tmo_s=10.0):
    """轮询 QUERY 直到 state==want, 返回 (state|最后态, bool)"""
    t0 = time.time()
    last = None
    while time.time() - t0 < tmo_s:
        q = query(link)
        if q:
            if q["state"] != last:
                print(f"    state {last}->{q['state']}({ST.get(q['state'])}) @{time.time()-t0:.1f}s")
                last = q["state"]
            if q["state"] == want:
                return q["state"], True
        time.sleep(0.25)
    return (last if last is not None else -1), False


def motor_state(link):
    r, _ = link.transaction(const.FC_MOTOR_CTRL, bytes([const.MOTOR_QUERY]), timeout_s=2)
    return r["data"][2] if r and len(r["data"]) > 2 else -1


def discover_epc(link, tries=3):
    """UHF 同步盘点一轮, 返回第一张真实标签 EPC (无则 None).
    单轮 1s 盘点偶发漏读在场标签 (模块 0x22 时序), 连试多轮取首轮命中。"""
    for _ in range(tries):
        link.transaction(const.FC_UHF_CTRL, bytes([const.UHF_OPEN]), timeout_s=4)
        ri, _ = link.transaction(const.FC_UHF_CTRL,
                                 bytes([const.UHF_INVENTORY, 0xE8, 0x03]), timeout_s=6)
        if ri and ri["data"][1] == 0:
            cnt = ri["data"][2] | (ri["data"][3] << 8)
            if cnt and len(ri["data"]) >= 6:
                elen = ri["data"][5]
                return bytes(ri["data"][6:6 + elen])
    return None


def configure(link, soft, add_epc=None):
    """CONFIGURE(清清单+设软标) + 可选 ADD 真实 EPC"""
    r, _ = tx(link, [const.LOCKER_CONFIGURE, 0, 0, soft & 0xFF, soft >> 8])
    if not r or r["data"][1] != 0:
        return f"CONFIGURE err={r['data'][1] if r else '无响应'}"
    if add_epc is not None:
        r, _ = tx(link, [const.LOCKER_ADD, len(add_epc)] + list(add_epc))
        if not r or r["data"][1] != 0:
            return f"ADD err={r['data'][1] if r else '无响应'}"
    return None


def main():
    link = HidLink()
    link.open()
    print("== App_Locker 多状态流闭环测试 ==")

    tx(link, [const.LOCKER_CANCEL])           # 干净起点 (升起中则经 LOWERING ~2.2s)
    wait_state(link, 0, tmo_s=8.0)
    time.sleep(0.3)
    drain_events(link, 0.5)

    epc = discover_epc(link)
    print(f"感应区 EPC: {epc.hex() if epc else '无'}")
    if not epc:
        record("前置", False, "无标签, 需真实标签在场")
        return summary()

    # ---- T1 主流程: 匹配->升起->HARD_DONE->SOFT->CONSUME*2->DONE->LOWERING->IDLE ----
    print("\n-- T1 主流程 (单硬标签 m=1, soft=2) --")
    e = configure(link, 2, epc)
    if e:
        record("T1a 配置", False, e)
    else:
        q = query(link)
        record("T1a 配置+ADD", q and q["state"] == 1 and q["hm"] == 0,
               f"state={ST.get(q['state'])} hm={q['hm']} sc={q['sc']}")

    r, _ = tx(link, [const.LOCKER_START])
    record("T1b START", r and r["data"][1] == 0, f"err={r['data'][1] if r else '无响应'}")

    # ADD 在 START 后 -> BUSY (清单门控)
    r, _ = tx(link, [const.LOCKER_ADD, len(epc)] + list(epc))
    record("T1c START后ADD->BUSY", r and r["data"][1] == 1,
           f"err={r['data'][1] if r else '无响应'}(期望1)")
    # START 重复激活 -> BUSY
    r, _ = tx(link, [const.LOCKER_START])
    record("T1d 重复START->BUSY", r and r["data"][1] == 1,
           f"err={r['data'][1] if r else '无响应'}(期望1)")

    # 等首匹配 -> UNLOCK_HOLD (寻触升起 ~2.2s) -> m==n -> SOFT_DECODE
    st, ok = wait_state(link, 3, tmo_s=12.0)
    record("T1e 匹配->SOFT_DECODE (经UNLOCK_HOLD)", ok and st == 3,
           f"state={ST.get(st, st)}")
    evs = drain_events(link, 2.0)
    record("T1f 事件序 MATCH_OK/HARD_DONE", 0 in evs and 2 in evs and 1 not in evs,
           f"[{', '.join(EV.get(e, str(e)) for e in evs)}]")
    q = query(link)
    record("T1g 计数", q and q["hm"] == 1 and q["su"] == 0,
           f"hm={q['hm']}(期望1) su={q['su']}(期望0)")

    # 消耗软标 x2 -> DONE
    r, _ = tx(link, [const.LOCKER_CONSUME_SOFT])
    r2, _ = tx(link, [const.LOCKER_CONSUME_SOFT])
    st, ok = wait_state(link, 4, tmo_s=5.0)
    evs2 = drain_events(link, 2.0)
    record("T1h CONSUME*2->DONE", ok and r["data"][1] == 0 and r2["data"][1] == 0,
           f"state={ST.get(st, st)} err={r['data'][1]}/{r2['data'][1]} "
           f"事件[{', '.join(EV.get(e, str(e)) for e in evs2)}]")

    # DONE 5s -> LOWERING -> IDLE
    st, ok = wait_state(link, 0, tmo_s=15.0)
    time.sleep(0.5)
    ms = motor_state(link)
    record("T1i DONE->LOWERING->IDLE", ok and st == 0,
           f"终态 state={ST.get(st, st)} motorState={ms}(期望0 IDLE)")

    # ---- T2 MISMATCH: 非清单 EPC 不升起 ----
    print("\n-- T2 MISMATCH (配置假EPC, 真标签在场) --")
    e = configure(link, 1, b"\xAA" * 12)
    if e:
        record("T2a 配置", False, e)
    else:
        tx(link, [const.LOCKER_START])
        t0 = time.time()
        evs = []
        while time.time() - t0 < 8.0 and 1 not in evs:
            evs.extend(drain_events(link, 1.0))
        q = query(link)
        record("T2b MISMATCH事件+不升起", 1 in evs and q and q["state"] in (1, 2)
               and q["hm"] == 0,
               f"事件[{', '.join(EV.get(x, str(x)) for x in evs)}] "
               f"state={ST.get(q['state']) if q else '?'} hm={q['hm'] if q else '?'}(期望0)")
        tx(link, [const.LOCKER_CANCEL])
        st, _ = wait_state(link, 0, tmo_s=5.0)
        record("T2c CANCEL回IDLE(未升起直达)", st == 0, f"state={ST.get(st, st)}")

    # ---- T3 CANCEL 升起中途打断 -> LOWERING ----
    print("\n-- T3 CANCEL 升起中途 -> LOWERING 安全回降 --")
    e = configure(link, 1, epc)
    if e:
        record("T3a 配置", False, e)
    else:
        tx(link, [const.LOCKER_START])
        # m=1 时 UNLOCK_HOLD 与 SOFT_DECODE 同拍切换, 轮询只见 3;
        # SOFT 出现时磁块仍在寻触上升 (~2.2s), 即为"升起中途"
        st, ok = wait_state(link, 3, tmo_s=10.0)
        if ok:
            r, _ = tx(link, [const.LOCKER_CANCEL], t=3)
            seen6 = False
            t0 = time.time()
            while time.time() - t0 < 12.0:
                q = query(link)
                if q and q["state"] == 6:
                    seen6 = True
                if q and q["state"] == 0:
                    break
                time.sleep(0.1)
            ms = motor_state(link)
            record("T3b 中途CANCEL->LOWERING->IDLE", seen6 and ms == 0,
                   f"CANCEL err={r['data'][1]} 观察到LOWERING={seen6} "
                   f"终态 motor={ms}")
            drain_events(link, 0.5)
        else:
            record("T3b 中途CANCEL->LOWERING->IDLE", False, f"未观察到SOFT (state={st})")

    # ---- T4 纯软标任务: hardCount=0 直入 SOFT_DECODE ----
    print("\n-- T4 纯软标 (CONFIGURE soft=1, 无ADD) --")
    e = configure(link, 1)
    if e:
        record("T4a 配置", False, e)
    else:
        r, _ = tx(link, [const.LOCKER_START])
        st, ok = wait_state(link, 3, tmo_s=3.0)
        record("T4b START直入SOFT_DECODE", ok and st == 3 and r["data"][1] == 0,
               f"err={r['data'][1]} state={ST.get(st, st)}(期望3)")
        tx(link, [const.LOCKER_CONSUME_SOFT])
        st, ok = wait_state(link, 0, tmo_s=15.0)
        evs = drain_events(link, 2.0)
        record("T4c 消耗完->DONE->LOWERING->IDLE", ok and st == 0,
               f"终态={ST.get(st, st)} 事件[{', '.join(EV.get(e, str(e)) for e in evs)}]")
        record("T4d 事件序 SOFT_USED/DONE", 3 in evs and 5 in evs,
               f"[{', '.join(EV.get(e, str(e)) for e in evs)}]")

    # ---- T5 事后恢复 ----
    print("\n-- T5 事后恢复 --")
    time.sleep(0.3)
    q = query(link)
    ms = motor_state(link)
    evs = drain_events(link, 1.0)
    record("T5a Locker IDLE + 电机 IDLE", q and q["state"] == 0 and ms == 0,
           f"locker={ST.get(q['state']) if q else '?'} motor={ms}")
    record("T5b 遗留事件", len(evs) == 0,
           f"[{', '.join(EV.get(e, str(e)) for e in evs)}]")

    return summary()


def summary():
    print("\n== 结果汇总 ==")
    npass = sum(1 for _, ok, _ in results if ok)
    for name, ok, msg in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}: {msg}")
    print(f"== {npass}/{len(results)} PASS ==")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
