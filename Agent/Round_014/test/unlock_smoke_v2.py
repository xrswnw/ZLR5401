#!/usr/bin/env python3
"""Round_014 真机冒烟: 新 0x10 (多标签名单+报警状态机) / 0x11 (AM 控制恢复) /
0x09 v2 进度布局 (motorPh+flags)。无设备时不可运行; 供实测/联调用法:

  python3 unlock_smoke_v2.py epc <tmoSec> <epcHex> [<epcHex>...]   # 0x10 全程 + 进度跟踪
  python3 unlock_smoke_v2.py epc_cancel <tmoSec> <epcHex>         # 升起后 CANCEL
  python3 unlock_smoke_v2.py am <amCnt> <tmoSec>                  # 0x11 消磁流程
  python3 unlock_smoke_v2.py progress                             # 空闲时拉 0x09 (全零)

0x09 v2 布局: [09, err, phase, holdMs(3 LE), total, confirmed, bitmap,
              softCnt(=riseCnt), softDone(=alarmCnt), lastCycle, motorPh, flags]
flags: bit0 有效子集稳定 bit1 外来介入 bit2 报警中 bit3 红外遮挡 bit4 消磁中
motorPh: 0 静止 / 1 上行 / 2 在顶 / 3 下行
0x10 终帧: [10, err, endReason, bitmap, rise(2), lower(2), elapsed(2 秒),
           riseCnt, alarmCnt, fgnSeen]
0x11 终帧: [11, err, endReason, rise(2,恒0), lower(2,恒0), elapsed(2 秒),
           amCnt, deactDone, deactFail(1=挂起确认失败), rsv0, rsv1]
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER = 0x23
RESP = 0xFF
PH = {0: "NONE", 1: "WAIT", 3: "RISE", 7: "HOLD/UP", 4: "LOWER", 5: "AM", 6: "DONE"}
MPH = {0: "静置", 1: "上行", 2: "在顶", 3: "下行"}
ENDR = {1: "ALL_OK", 3: "TIMEOUT", 4: "UHF_LOST", 6: "ABORTED"}


def req_epc(tmo_sec, epcs):
    body = bytes([0x10, tmo_sec & 0xFF, tmo_sec >> 8, len(epcs)])
    for e in epcs:
        e = bytes.fromhex(e)
        body += bytes([len(e)]) + e
    return body


def req_am(am_cnt, tmo_sec):
    return bytes([0x11, am_cnt, tmo_sec & 0xFF, tmo_sec >> 8, 0, 0])


def flags_str(fl):
    s = ""
    if fl & 1: s += "有效子集 "
    if fl & 2: s += "外来介入 "
    if fl & 4: s += "报警中 "
    if fl & 8: s += "红外遮挡 "
    if fl & 0x10: s += "消磁中 "
    return s.strip() or "-"


def run_flow(lnk, body, term_sub, tmo_wall, want_cancel=False, cancel_at=None):
    lnk.send_frame(1, FC_LOCKER, body)
    t0 = time.time()
    terminal, cancel_sent, next_poll, ev = None, False, 0.0, []
    while time.time() - t0 < tmo_wall:
        now = time.time()
        f = lnk.recv_frame(0.05)
        if f and f["func"] == (FC_LOCKER ^ RESP):
            d = f["data"]
            if d[0] == term_sub:
                terminal = d
                break
            if d[0] == 0x0F:
                ev.append(f"[{now - t0:5.2f}s] 受理 phase={PH.get(d[2], d[2])} "
                          f"win={d[3] | (d[4] << 8) | (d[5] << 16)}ms")
            elif d[0] == 0x0B:
                ev.append(f"[{now - t0:5.2f}s] 确认 EPC={d[3:3 + d[2]].hex()} "
                          f"{d[3 + d[2]]}/{d[4 + d[2]]}")
            elif d[0] == 0x0C:
                ev.append(f"[{now - t0:5.2f}s] !! 外来介入 EPC={d[2:2 + d[1]].hex()}")
            elif d[0] == 0x0E:
                ev.append(f"[{now - t0:5.2f}s] 消磁 +1 done={d[1]}/{d[2]}")
            elif d[0] == 0x09 and len(d) >= 14:
                key = (d[2], d[10], d[12])
                if not hasattr(run_flow, "_last") or run_flow._last != key:
                    run_flow._last = key
                    ev.append(f"[{now - t0:5.2f}s] 进度 phase={PH.get(d[2], d[2])} "
                              f"motor={MPH.get(d[10], d[10])} [{flags_str(d[12])}] "
                              f"valid/confirmed={d[6]}/{d[5]}")
        if now >= next_poll:
            next_poll = now + 0.5
            lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
        if want_cancel and not cancel_sent and now - t0 >= cancel_at:
            lnk.send_frame(1, FC_LOCKER, bytes([0x04]))
            cancel_sent = True
            print(f"  [{now - t0:5.2f}s] 已发 CANCEL")
    for e in ev:
        print("  " + e)
    if terminal is None:
        print("  !! 无终帧 (超时退出)")
        return None
    return terminal


def show_epc_terminal(d):
    err, endr = d[1], d[2]
    rise = d[4] | (d[5] << 8)
    lower = d[6] | (d[7] << 8)
    el = d[8] | (d[9] << 8)
    print(f"  终帧: err={err} endReason={endr} ({ENDR.get(endr, '?')}) bitmap={d[3]:#04x} "
          f"rise={rise} lower={lower} elapsed={el}s "
          f"riseCnt={d[10]} alarmCnt={d[11]} fgnSeen={d[12]}")
    return err == 0


def show_am_terminal(d):
    err, endr = d[1], d[2]
    el = d[7] | (d[8] << 8)
    print(f"  终帧: err={err} endReason={endr} ({ENDR.get(endr, '?')}) elapsed={el}s "
          f"amCnt={d[9]} deactDone={d[10]} deactFail={d[11]}"
          + ("  !! 挂起确认失败, 解码器可能仍在消磁" if d[11] else ""))
    return err == 0


def poll_idle(lnk):
    lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
    f = lnk.recv_frame(0.5)
    if f and f["func"] == (FC_LOCKER ^ RESP) and f["data"][0] == 0x09:
        d = f["data"]
        print(f"0x09 (v2, {len(d)}B): phase={PH.get(d[2], d[2])} motorPh={MPH.get(d[10], d[10])} "
              f"flags={flags_str(d[12])} 全零={d[2] == 0}")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "progress"
    lnk = HidLink()
    lnk.open()
    try:
        if mode == "epc":
            tmo = int(sys.argv[2])
            d = run_flow(lnk, req_epc(tmo, sys.argv[3:]), 0x10, tmo + 30)
            if d: ok = show_epc_terminal(d)
        elif mode == "epc_cancel":
            tmo = int(sys.argv[2])
            d = run_flow(lnk, req_epc(tmo, sys.argv[3:]), 0x10, tmo + 30,
                         want_cancel=True, cancel_at=8.0)
            if d: ok = show_epc_terminal(d)
        elif mode == "am":
            cnt, tmo = int(sys.argv[2]), int(sys.argv[3])
            d = run_flow(lnk, req_am(cnt, tmo), 0x11, tmo + 20)
            if d: ok = show_am_terminal(d)
        else:
            poll_idle(lnk)
    finally:
        lnk.close()


if __name__ == "__main__":
    main()
