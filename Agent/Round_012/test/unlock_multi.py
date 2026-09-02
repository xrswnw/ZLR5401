#!/usr/bin/env python3
"""LOCKER_SUB_UNLOCK_MULTI (0x0A) 多标签同步解锁闭环测试 — Round_012.

对应 Plan.html M1~M9 台架用例 (M2 双真标/M9 软标需专用台架条件, 缺条件自动降级记录)。

用法:
  python3 unlock_multi.py               # 全用例 (自适应发现感应区标签)
  python3 unlock_multi.py --skip-motion # 跳过会动磁块的用例
"""
import argparse
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr import const                     # noqa: E402

FC = const.FC_LOCKER_CTRL
RSP = FC ^ 0xFF
SUB = 0x0A

ERR_NAME = {0: "OK", 1: "BUSY", 2: "PARAM", 3: "UHF_OPEN", 4: "UHF_LINK",
            7: "HOMING", 8: "MOTOR_FAULT", 9: "MOTOR_TIMEOUT", 10: "AM_LINK", 11: "NO_IR"}
END_NAME = {1: "ALL_OK", 2: "PARTIAL_TIMEOUT", 4: "UHF_LOST", 6: "ABORTED"}
PH_NAME = {1: "WAIT_TAG", 2: "VERIFY", 3: "RISE", 4: "LOWER", 5: "SOFT", 6: "DONE"}

results = []


def record(name, ok, msg):
    results.append((name, ok, msg))
    tag = "PASS" if ok else ("FAIL" if ok is False else "SKIP/记录")
    print(f"  [{tag}] {name}: {msg}")


def tx(link, data, t=3.0):
    t0 = time.time()
    resp, to = link.transaction(FC, bytes(data), timeout_s=t)
    return resp, to, time.time() - t0


def tx_sub(link, sub, data, t=3.0):
    """流程中交互: 发送后跳过异步推送帧 (data[0]=0x0B~0x0F), 收回显 sub 的响应帧。
    transaction 会把管道里的推帧当响应误配对, 流程运行期一律用本函数。"""
    t0 = time.time()
    link.send_frame(const.DEV_ADDR, FC, bytes(data))
    deadline = time.time() + t
    while time.time() < deadline:
        f = link.recv_frame(timeout_s=max(0.2, deadline - time.time()))
        if f is None or f["func"] != RSP:
            continue
        if f["data"] and f["data"][0] == sub:
            return f, False, time.time() - t0
    return None, True, time.time() - t0


def discover_epc(link, tries=3):
    """UHF 同步盘点取感应区标签列表 (单轮 ~1/4 漏读, 多轮合并去重)。"""
    seen = []
    for _ in range(tries):
        link.transaction(const.FC_UHF_CTRL, bytes([const.UHF_OPEN]), timeout_s=4)
        ri, _ = link.transaction(const.FC_UHF_CTRL,
                                 bytes([const.UHF_INVENTORY, 0xE8, 0x03]), timeout_s=6)
        if ri and ri["data"][1] == const.UHF_ERR_OK:
            cnt = ri["data"][2] | (ri["data"][3] << 8)
            d = ri["data"]
            pos = 4
            for _ in range(min(cnt, 8)):
                if pos + 2 > len(d):
                    break
                elen = d[pos + 1]
                if pos + 2 + elen > len(d):
                    break
                e = bytes(d[pos + 2:pos + 2 + elen])
                if e not in seen:
                    seen.append(e)
                pos += 2 + elen
    return seen


def start_unlock(link, payload):
    """异步下发 0x0A (不读 — 后续事件/终帧由 drain 统一收取)。"""
    link.send_frame(const.DEV_ADDR, FC, bytes(payload))


def drain_terminal(link, timeout_s, quiet_s=2.0):
    """收取 0x0A 全流程推送事件流直至终帧 (data[0]=0x0A)。
    返回 (events, terminal_frame|None); events = [(sub, bytes), ...]。"""
    events = []
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        f = link.recv_frame(timeout_s=min(quiet_s, max(0.2, deadline - time.time())))
        if f is None:
            continue
        if f["func"] != RSP:
            events.append(("?", bytes(f["data"])))
            continue
        d = f["data"]
        if d and d[0] == SUB:
            return events, f
        events.append((d[0] if d else -1, bytes(d)))
    return events, None


def expect_push(events, sub):
    return [e for e in events if e[0] == sub]


def parse_terminal(d):
    """err=0 终帧: [0x0A,0,end,bitmap,confirmed,total,rise(2),lower(2),softDone,softCnt,elapsed(2)]"""
    return {
        "end": d[2], "bitmap": d[3], "confirmed": d[4], "total": d[5],
        "rise": d[6] | (d[7] << 8), "lower": d[8] | (d[9] << 8),
        "softDone": d[10], "softCnt": d[11],
        "elapsed": d[12] | (d[13] << 8) if len(d) > 13 else 0,
    }


def build_cmd(tmo_ms, hold_ms, soft_cnt, epcs):
    """[cmd, tmo(2), hold(2), softCnt, epcCnt, epcLen, epc..]"""
    elen = len(epcs[0])
    d = [SUB, tmo_ms & 0xFF, tmo_ms >> 8, hold_ms & 0xFF, hold_ms >> 8,
         soft_cnt, len(epcs), elen]
    for e in epcs:
        d += list(e)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-motion", action="store_true", help="跳过升降用例")
    args = ap.parse_args()

    link = HidLink()
    link.open()
    print("== LOCKER_SUB_UNLOCK_MULTI (0x0A) 多标签解锁闭环测试 ==")
    tx(link, [const.LOCKER_CANCEL])

    # ---- P1 参数非法 (不动磁块, 立即回) ----
    for name, payload in [
        ("P1a epcCnt=0", [SUB, 0xE8, 0x03, 0x00, 0x00, 0, 0, 6] + [0xAA] * 6),
        ("P1b epcCnt=5", [SUB, 0xE8, 0x03, 0x00, 0x00, 0, 5, 6] + [0xAA] * 30),
        ("P1c epcLen=0", [SUB, 0xE8, 0x03, 0x00, 0x00, 0, 1, 0]),
        ("P1d epcLen=13", [SUB, 0xE8, 0x03, 0x00, 0x00, 0, 1, 13] + [0xAA] * 13),
        ("P1e 短帧", [SUB, 0xE8, 0x03]),
    ]:
        resp, to, dt = tx(link, payload)
        if to:
            record(name, False, "无响应")
        else:
            e = resp["data"][1]
            record(name, e == 2, f"err={e}({ERR_NAME.get(e)}) 耗时{dt:.3f}s")

    print("\n-- 感应区标签发现 --")
    tags = discover_epc(link) if not args.skip_motion else []
    print(f"  发现 {len(tags)} 张: {[t.hex() for t in tags]}")

    if tags:
        epc = tags[0]

        # ---- M1 单张全流程 (m=1, soft=0, hold=10s) ----
        if not args.skip_motion:
            print("\n-- M1 单张全流程 (m=1, W=30s: 开始后请放标/遮挡光电并保持) --")
            t0 = time.time()
            start_unlock(link, build_cmd(1000, 30000, 0, [epc]))
            events, term = drain_terminal(link, 30 + 40)
            dt = time.time() - t0
            if term is None:
                record("M1 单张全流程", False, f"终帧超时 ({dt:.1f}s), events={len(events)}")
            else:
                acks = expect_push(events, 0x0F)
                confs = expect_push(events, 0x0B)
                hards = expect_push(events, 0x0D)
                t = term["data"]
                if t[1] != 0:
                    extra = " ".join(f"{b:02X}" for b in t[2:])
                    record("M1 单张全流程", False,
                           f"err={t[1]}({ERR_NAME.get(t[1])}) diag=[{extra}] 耗时{dt:.1f}s")
                else:
                    p = parse_terminal(t)
                    ack_ok = len(acks) == 1 and acks[0][1][2] == 1 and len(acks[0][1]) == 6
                    conf_ok = (len(confs) == 1 and confs[0][1][1] == 1
                               and bytes(confs[0][1][3:3 + confs[0][1][2]]) == epc
                               and confs[0][1][3 + confs[0][1][2]] == 1
                               and confs[0][1][4 + confs[0][1][2]] == 1)
                    hard_ok = len(hards) == 1 and hards[0][1][1] == p["end"]
                    ok = (p["end"] == 1 and p["bitmap"] == 1 and p["confirmed"] == 1
                          and p["total"] == 1 and p["rise"] > 0 and p["lower"] > 0
                          and p["softCnt"] == 0 and ack_ok and conf_ok and hard_ok)
                    c = confs[0][1] if confs else b""
                    record("M1 单张全流程", ok,
                           f"end={END_NAME.get(p['end'])} bitmap={p['bitmap']:02X} "
                           f"rise/lower={p['rise']}/{p['lower']} elapsed={p['elapsed']}ms "
                           f"ACK={len(acks)} 确认帧={len(confs)} 硬标帧={len(hards)} "
                           f"总耗时{dt:.1f}s")
                    if confs:
                        cl = confs[0][1]
                        el = cl[2]
                        j1 = cl[5 + el] | (cl[6 + el] << 8) if len(cl) > 6 + el else 0
                        j2 = cl[7 + el] | (cl[8 + el] << 8) if len(cl) > 8 + el else 0
                        record("M1-细节 确认帧字段", 3000 <= j1 <= 30000,
                               f"seq=1 判据耗时={j1}ms (双门限A=3000ms起) 流程耗时={j2}ms")
                    record("M8 蜂鸣听感", None,
                           "人工确认: 确认短鸣 200ms 自动停 + 完成长鸣 300ms (无长鸣粘连)")
            time.sleep(1)

        # ---- M4 部分超时 (m=2: 1 真 + 1 假, 短窗) ----
        if not args.skip_motion:
            print("\n-- M4 部分超时 (m=2 只放真标, W=12s: 请放标/遮挡光电) --")
            t0 = time.time()
            start_unlock(link, build_cmd(1000, 12000, 0, [epc, b"\xAA" * len(epc)]))
            events, term = drain_terminal(link, 12 + 40)
            dt = time.time() - t0
            if term is None:
                record("M4 部分超时", False, f"终帧超时 ({dt:.1f}s)")
            else:
                t = term["data"]
                if t[1] == 0:
                    p = parse_terminal(t)
                    confs = expect_push(events, 0x0B)
                    hards = expect_push(events, 0x0D)
                    ok = (p["end"] == 2 and p["confirmed"] == 1 and p["total"] == 2
                          and p["bitmap"] == 0x01 and len(confs) == 1 and len(hards) == 1)
                    record("M4 部分超时", ok,
                           f"end={END_NAME.get(p['end'])} confirmed={p['confirmed']}/{p['total']} "
                           f"bitmap={p['bitmap']:02X} (bit0=真标已确认, bit1=假标未确认) "
                           f"硬标帧={len(hards)} 耗时{dt:.1f}s")
                else:
                    extra = " ".join(f"{b:02X}" for b in t[2:])
                    record("M4 部分超时", False, f"err={t[1]} diag=[{extra}]")
            time.sleep(1)

        # ---- M5 失配不终止 (m=1 假 EPC, 真标在场) ----
        if not args.skip_motion:
            print("\n-- M5 失配不终止 (m=1 假 EPC, 真标在场, W=12s: 请放标/遮挡光电) --")
            t0 = time.time()
            start_unlock(link, build_cmd(1000, 12000, 0, [b"\x55" * len(epc)]))
            events, term = drain_terminal(link, 12 + 40)
            dt = time.time() - t0
            if term is None:
                record("M5 失配不终止", False, f"终帧超时 ({dt:.1f}s)")
            else:
                t = term["data"]
                mm = expect_push(events, 0x0C)
                confs = expect_push(events, 0x0B)
                if t[1] == 0:
                    p = parse_terminal(t)
                    ok = (len(mm) >= 1 and len(confs) == 0 and p["confirmed"] == 0
                          and p["end"] == 2 and p["bitmap"] == 0 and p["rise"] == 0)
                    mframe = mm[0][1] if mm else b""
                    record("M5 失配不终止", ok,
                           f"失配帧={len(mm)} 确认帧={len(confs)} (应为0) "
                           f"end={END_NAME.get(p['end'])} rise={p['rise']}(未升) "
                           f"末外来EPC={mframe[2:2+mframe[1]].hex() if mm else '-'} 耗时{dt:.1f}s")
                else:
                    extra = " ".join(f"{b:02X}" for b in t[2:])
                    record("M5 失配不终止", False, f"err={t[1]} diag=[{extra}]")
            time.sleep(1)

        # ---- M2 两张真标 ----
        if not args.skip_motion and len(tags) >= 2:
            print(f"\n-- M2 两张同时放 (m=2, 请 {tags[0].hex()} + {tags[1].hex()} 同时放) --")
            t0 = time.time()
            start_unlock(link, build_cmd(1000, 15000, 0, tags[:2]))
            events, term = drain_terminal(link, 15 + 40)
            dt = time.time() - t0
            if term is None:
                record("M2 两张全流程", False, f"终帧超时 ({dt:.1f}s)")
            else:
                t = term["data"]
                if t[1] == 0:
                    p = parse_terminal(t)
                    confs = expect_push(events, 0x0B)
                    seqs = [c[1][1] for c in confs]
                    ok = (p["end"] == 1 and p["confirmed"] == 2 and p["total"] == 2
                          and p["bitmap"] == 0x03 and sorted(seqs) == [1, 2])
                    record("M2 两张全流程", ok,
                           f"end={END_NAME.get(p['end'])} confirmed={p['confirmed']}/{p['total']} "
                           f"bitmap={p['bitmap']:02X} 确认seq={seqs} 耗时{dt:.1f}s")
                else:
                    record("M2 两张全流程", False, f"err={t[1]}")
        else:
            record("M2 两张全流程", None, "SKIP: 感应区不足 2 张真标 (需双标签台架)")

        # ---- M3 逐张放取 (确认后取走再放第二张) ----
        record("M3 逐张放取/MASKED", None,
               "SKIP: 需人工配合时序 (确认后取走 A 再放 B), 参照 M1/M2 脚本手动执行")

        # ---- M6/M7 解锁态响应矩阵 + CANCEL ----
        print("\n-- M6/M7 解锁态响应矩阵 (假 EPC 长窗 -> WAIT_TAG 内交互) --")
        start_unlock(link, build_cmd(1000, 60000, 0, [b"\x33" * 6]))
        time.sleep(1.0)

        # 0x08 单标签 -> BUSY
        r08, to, _ = tx_sub(link, 0x08, [0x08, 0xE8, 0x03, 0x00, 0x00, 6] + [0xBB] * 6, t=4)
        if to:
            record("M6a 0x08->BUSY", False, "无响应")
        else:
            d = r08["data"]
            record("M6a 0x08->BUSY", d[1] == 1,
                   f"err={d[1]}({ERR_NAME.get(d[1])}) 三态={[d[2],d[3],d[4]] if len(d)>4 else '?'}")

        # CONFIGURE -> BUSY
        rc, to, _ = tx_sub(link, const.LOCKER_CONFIGURE,
                           [const.LOCKER_CONFIGURE, 1, 0, 0, 0], t=3)
        record("M6b CONFIGURE->BUSY", (not to) and rc["data"][1] == 1,
               f"err={rc['data'][1] if rc else '超时'}")

        # MOTOR MOVE -> BUSY
        rm, to, _ = tx_sub(link, const.MOTOR_MOVE,
                           [const.MOTOR_MOVE if hasattr(const, 'MOTOR_MOVE') else 0x01,
                            0, 10, 0, 0], t=3)
        record("M6c MOTOR->BUSY", (not to) and rm["data"][1] != 0,
               f"err={rm['data'][1] if rm else '超时'} (MOTOR 错误码)")

        # UHF OPEN -> BUSY
        ru, to, _ = tx_sub(link, const.UHF_OPEN, [const.UHF_OPEN], t=3)
        record("M6d UHF->BUSY", (not to) and ru["data"][1] != 0,
               f"err={ru['data'][1] if ru else '超时'} (UHF 错误码)")

        # GET_PROGRESS -> 多标签布局 (phase=WAIT_TAG)
        rp, to, _ = tx_sub(link, 0x09, [0x09], t=3)
        if to or not rp:
            record("M6e GET_PROGRESS 多标签布局", False, "无响应")
        else:
            d = rp["data"]
            ok = (d[1] == 0 and len(d) == 10 and d[2] in (1, 2) and d[5] == 1
                  and d[6] == 0 and d[7] == 0)
            record("M6e GET_PROGRESS 多标签布局", ok,
                   f"err={d[1]} phase={PH_NAME.get(d[2], d[2]) if len(d)>2 else '?'} "
                   f"(W=1或2: 标签在场时光电即时触发)"
                   f"holdMs={d[3]|(d[4]<<8) if len(d)>4 else '?'} "
                   f"total={d[5] if len(d)>5 else '?'} confirmed={d[6] if len(d)>6 else '?'} "
                   f"len={len(d)} (应为 10B 多标签布局)")

        # QUERY -> 正常放行
        rq, to, _ = tx_sub(link, const.LOCKER_QUERY, [const.LOCKER_QUERY], t=3)
        record("M6f QUERY 放行", (not to) and rq and rq["data"][1] == 0,
               f"err={rq['data'][1] if rq else '超时'}")

        # CANCEL -> 立即 OK; 流程安全结束以 ABORTED 终帧 (WAIT_TAG 未动磁块)
        rc2, to, _ = tx_sub(link, const.LOCKER_CANCEL, [const.LOCKER_CANCEL], t=3)
        cancel_ok = (not to) and rc2 and rc2["data"][1] == 0
        events, term = drain_terminal(link, 10)
        if term is None:
            record("M7 CANCEL(WAIT_TAG)->ABORTED", False,
                   f"OK回帧={'是' if cancel_ok else '否'} 终帧超时")
        else:
            t = term["data"]
            if t[1] == 0:
                p = parse_terminal(t)
                ok = cancel_ok and p["end"] == 6 and p["rise"] == 0 and p["lower"] == 0
                record("M7 CANCEL(WAIT_TAG)->ABORTED", ok,
                       f"CANCEL回OK={cancel_ok} end={END_NAME.get(p['end'])} "
                       f"rise/lower={p['rise']}/{p['lower']}(未动磁块)")
            else:
                record("M7 CANCEL(WAIT_TAG)->ABORTED", False, f"err={t[1]}({ERR_NAME.get(t[1])})")
        record("M7-补充 升起中/回降中打断", None,
               "SKIP: 时序难自动构造 — 升起中 CANCEL 应免疫回降后 ABORTED (参照 0x08 abort.py 手动)")
    else:
        record("M1/M4/M5/M2", None, "SKIP: 感应区无真标 (0x0A 可跑参数/响应矩阵用例见上)")

    # ---- M9 软标 ----
    record("M9 软标计数", None,
           "SKIP: 需 AM 消磁器接回设备侧 (softCnt>0: 0x0E 解码帧+softDone 结账)")

    # ---- R1 事后恢复 ----
    time.sleep(1)
    q, _ = link.transaction(FC, bytes([const.LOCKER_QUERY]), timeout_s=3)
    motor = link.transaction(const.FC_MOTOR_CTRL, bytes([const.MOTOR_QUERY]), timeout_s=3)[0]
    lstate = q["data"][2] if q else -1
    mstate = motor["data"][2] if motor else -1
    record("R1 事后恢复", lstate == 0 and mstate == 0, f"locker={lstate} motor={mstate}")

    link.close()
    print("\n== 结果汇总 ==")
    ok = sum(1 for _, o, _ in results if o is True)
    bad = sum(1 for _, o, _ in results if o is False)
    print(f"PASS {ok} / FAIL {bad} / 记录 {len(results) - ok - bad}")


if __name__ == "__main__":
    main()
