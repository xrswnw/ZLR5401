#!/usr/bin/env python3
"""flow_monitor — 0x10 循环窗实测监控台: 发起流程 + 实时时间线 (用户物理实测用)。

用法: python3 flow_monitor.py [timeoutSec=300]
后台发起 rsv0=timeoutSec 秒循环模式 0x10 (epc=台架标签), 之后每 0.5s 拉
GET_PROGRESS(0x09); phase/cycles/ok/lastCycle/confirmed/bm 任一变化即打
时间线; 0x0F 受理 / 0x0B 周期确认推帧全收; 终帧 (0x10 回显) 即汇总。
HID 收发异常 (设备意外复位) 记 FAIL 事件并重开。日志同步落 live_monitor.log。
phase: 1=等放标 2=校对 3=升起 4=回降 7=顶部保持
lastCycle: 0无/进行中 1成功 2移除 3更换
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER = 0x23
EPC = "33553463a4000158eb7e7a50"
TMO, WIN, HOLD = 100, 20000, 3000
PH = {0: "无", 1: "等放标", 2: "校对", 3: "升起", 4: "回降", 5: "软标", 7: "顶部保持"}
LASTCYC = {0: "进行中/无", 1: "成功", 2: "移除", 3: "更换"}
ENDR = {1: "ALL_OK", 2: "EPC_LOST", 3: "窗满", 4: "UHF_LOST", 6: "ABORTED"}
LOG = open("/Users/swnw/Documents/Software/ZLR5401/Agent/Round_013/test/live_monitor.log", "a")


def say(s):
    line = "[%6.1fs] %s" % (time.time() - T0, s)
    print(line, flush=True)
    LOG.write(line + "\n"); LOG.flush()


def drain(lnk, quiet=0.05):
    """收帧分类: 返回 (progress|terminal|push, data) 或 None。"""
    try:
        f = lnk.recv_frame(quiet)
    except Exception:
        return "LINK", None
    if not f or f["func"] != (FC_LOCKER ^ 0xFF) or len(f["data"]) < 2:
        return None, None
    d = f["data"]
    if d[0] == 0x10:
        return "TERM", d
    if d[0] == 0x09 and len(d) >= 12:
        return "PROG", d
    if d[0] in (0x0F, 0x0B, 0x0C):
        return "PUSH", d
    return None, None


def main():
    global T0
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 255
    if dur > 255:
        print("(rsv0 为单字节, 循环窗上限 255s — 已钳位)"); dur = 255
    lnk = HidLink(); lnk.open()
    epc = bytes.fromhex(EPC)
    req = bytes([0x10, len(epc), TMO & 0xFF, TMO >> 8, WIN & 0xFF, WIN >> 8,
                 HOLD & 0xFF, HOLD >> 8, dur, 0, 0, 0]) + epc
    T0 = time.time()
    say("== 发起 0x10 循环窗 %ds: epc=%s tmo=%d win=%d holdTop=%d ==" % (dur, EPC, TMO, WIN, HOLD))
    lnk.send_frame(1, FC_LOCKER, req)

    last = None
    terminal = None
    resets = 0
    while time.time() - T0 < dur + 30:
        try:
            lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
        except Exception:
            resets += 1
            say("!! HID 发送失败 — 设备疑似复位, 尝试重开 #%d" % resets)
            try:
                lnk.close()
            except Exception:
                pass
            time.sleep(2.0)
            try:
                lnk = HidLink(); lnk.open()
                say("   已重连 (流程已随复位终止 — 视为 FAIL 事件)")
                break
            except Exception:
                continue
        for _ in range(4):
            kind, d = drain(lnk)
            if kind == "LINK":
                resets += 1
                say("!! HID 收包异常 #%d" % resets)
                break
            if kind == "TERM":
                terminal = d
                break
            if kind == "PUSH":
                if d[0] == 0x0F:
                    say("0x0F 受理: phase=%d winMs=%d" % (d[2], d[3] | (d[4] << 8) | (d[5] << 16)))
                elif d[0] == 0x0B:
                    jt = d[15] | (d[16] << 8)
                    say("0x0B 周期确认: 判据耗时=%dms (epcLen=%d)" % (jt, d[2]))
                else:
                    say("0x%02X 推帧: %s" % (d[0], d.hex()))
                continue
            if kind == "PROG":
                cur = (d[2], d[6], d[7], d[8], d[9], d[10], d[11])
                if cur != last:
                    say("相位=%s holdMs=%d total=%d confirmed=%d bm=%02X 周期=%d ok=%d lastCycle=%s" % (
                        PH.get(d[2], d[2]), int.from_bytes(d[3:6], "little"),
                        d[6], d[7], d[8], d[9], d[10], LASTCYC.get(d[11], d[11])))
                    last = cur
                break
        if terminal:
            break
        time.sleep(0.5)

    if terminal is None:
        say("!! 窗 +%ds 内未见终帧 (流程疑似滞留)" % 30)
        LOG.close()
        return 1
    err = terminal[1]
    say("== 终帧: err=%d ==" % err)
    if err == 0:
        endr, bm = terminal[2], terminal[3]
        rise = terminal[4] | (terminal[5] << 8)
        lower = terminal[6] | (terminal[7] << 8)
        el = terminal[8] | (terminal[9] << 8)
        cyc, okc, failc = terminal[14], terminal[15], terminal[16]
        say("   endReason=%d(%s) bm=%02X rise=%d lower=%d elapsed=%ds" % (
            endr, ENDR.get(endr, "?"), bm, rise, lower, el))
        say("   周期: cycles=%d ok=%d fail=%d  (ok+fail=%s)" % (
            cyc, okc, failc, "自洽" if cyc == okc + failc else "不自洽!"))
    else:
        say("   诊断: %s" % terminal.hex())
    say("== 监控结束, 复位事件=%d ==" % resets)
    LOG.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
