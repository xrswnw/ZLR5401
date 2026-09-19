#!/usr/bin/env python3
"""motor_trace — 电机黑匣子现场排查工具 (上电自检/回零途中最中途停转/堵转定位).

用法:
  python3 motor_trace.py             # 事后导出: 设备保持上电, 直接拉黑匣子 + 判读
  python3 motor_trace.py --watch     # 复位设备并实时盯完整场"上电自检+回零", 结束后自动导出判读
  python3 motor_trace.py --watch --keep 60   # 盯 60s (默认 40s)

固件依赖: App 侧 MOTOR_CMD_TRACE (FC 0x20 / sub 0x0A, Round_013 现场调试新增).
原理: 上电自检/回零发生在主机接入之前, 现场堵转"看不见"; 固件侧每 100ms
记录 TRQ_COUNT/累计步数/行程开关原始电平, 每次真实停机记终态快照 (含 FAULT
寄存器冻结值), 存 RAM 黑匣子, 本工具事后取回判读根因:
  1) 末条历史 reason=3(高负载) 且末段样本 steps 仍推进:
     - 末段 trq 接近 0 -> 转子失步真堵转 (脉冲在走, 转子没跟: 转矩不足/机构卡)
     - 末段 trq 只是略低于阈值 -> 过载监测误判 (阈值不随转矩档缩放, 40% 档基线低)
  2) reason=4 + fault 位 -> 器件级: UVLO=供电跌落, OCP=过流, TF=过温, OL=开路
  3) reason=1(正常停) 但回零失败 -> 行程开关误触发 (样本 flags KEY 位中途置位)
  4) 历史环各次 reason=3 停机 steps 相近 -> 同位置机械卡点; 随机 -> 转矩/负载
"""
import argparse
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink  # noqa: E402

FC_MOTOR = 0x20
FC_IO_DIAG = 0x26
FC_RESET = 0x08
SUB_QUERY, SUB_HEALTH, SUB_TRACE = 0x05, 0x08, 0x0A

STEPPER_STATE = {0: "IDLE", 1: "RUN", 2: "FAULT"}
REASON = {0: "无", 1: "正常(触点/完成/外部停)", 2: "运行时限60s", 3: "高负载停机/行程超限", 4: "DRV故障"}
OLOV = {0: "正常", 1: "持续高负载", 2: "已降速", 3: "降速后停机"}
FLT_BITS = [(0x80, "FAULT"), (0x40, "SPI_ERROR"), (0x20, "UVLO(供电跌)"),
            (0x10, "CPUV(电荷泵)"), (0x08, "OCP(过流)"), (0x04, "STL(失速)"),
            (0x02, "TF(过温)"), (0x01, "OL(开路)")]
HOMING = {0: "未启动", 1: "回零中", 2: "已就绪", 3: "失败"}


def tx(link, fc, data, t=3.0, echo=None):
    """严格配对事务: 先排空陈旧回显 (POST 期积压的请求会连发应答), 再发送,
    只接受 func 匹配 (+可选 data[0] 回显匹配) 的响应, 其余丢弃."""
    for _ in range(8):                     # 排空积压
        try:
            if link.recv_frame(0.05) is None:
                break
        except Exception:
            break
    link.send_frame(1, fc, bytes(data))
    deadline = time.time() + t
    while time.time() < deadline:
        f = link.recv_frame(max(0.05, min(0.5, deadline - time.time())))
        if f and f["func"] == (fc ^ 0xFF):
            if echo is None or (len(f["data"]) > 0 and f["data"][0] == echo):
                return f["data"]
    return None


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def fault_str(fault):
    if not fault:
        return "-"
    names = [n for m, n in FLT_BITS if fault & m]
    return "0x%02X(%s)" % (fault, "|".join(names))


def parse_trace(d):
    """按 App_CustomProtocol.h MOTOR_CMD_TRACE 布局解析 (全 LE)."""
    t = {
        "state": d[2], "fault": d[3], "diag1": d[4], "diag2": d[5],
        "olov": d[6], "thresh": u16(d, 7), "trqPct": d[9], "dir": d[10],
        "switchErr": d[11], "speedHz": u32(d, 12), "nSmp": d[16],
        "nHist": d[17], "interval": d[18],
    }
    hist, p = [], 20
    for _ in range(t["nHist"]):
        hist.append({
            "startCnt": u32(d, p), "steps": u32(d, p + 4), "runMs": u32(d, p + 8),
            "trqFinal": u16(d, p + 12), "reason": d[p + 14], "fault": d[p + 15],
            "diag2": d[p + 16], "olov": d[p + 17],
        })
        p += 19
    smp = []
    for _ in range(t["nSmp"]):
        smp.append({
            "trq": u16(d, p), "ms": u16(d, p + 2), "steps": u32(d, p + 4),
            "flags": d[p + 8],
        })
        p += 9
    t["hist"], t["smp"] = hist, smp  # 均最新->最旧
    return t


def print_trace(t):
    print("=" * 78)
    print("实时态: stepper=%s fault=%s diag1=0x%02X diag2=0x%02X olov=%s 阈值=%d "
          "转矩=%d%% 速度=%dHz dir=%d switchErr=0x%02X" % (
              STEPPER_STATE.get(t["state"], t["state"]), fault_str(t["fault"]),
              t["diag1"], t["diag2"], OLOV.get(t["olov"], t["olov"]), t["thresh"],
              t["trqPct"], t["speedHz"], t["dir"], t["switchErr"]))
    print("-" * 78)
    print("停机历史 (最新->最旧, 含回零重试序列):")
    if not t["hist"]:
        print("  (无 — 本次上电后电机未停机或无记录)")
    for i, h in enumerate(t["hist"]):
        print("  #%d start=%d reason=%d(%s) steps=%d runMs=%dms trq=%d fault=%s olov=%s" % (
            i, h["startCnt"], h["reason"], REASON.get(h["reason"], "?"),
            h["steps"], h["runMs"], h["trqFinal"], fault_str(h["fault"]),
            OLOV.get(h["olov"], h["olov"])))
    print("-" * 78)
    print("最近一腿采样 (时间正序; 每拍 %dms; flags: U=上行程按下 D=下行程按下 R=降速中):" % t["interval"])
    if not t["smp"]:
        print("  (无 — 本腿未运行过 100ms 以上)")
    smp = list(reversed(t["smp"]))  # 转时间正序
    prev = None
    for s in smp:
        ds = "" if prev is None else "Δ%+d" % (s["steps"] - prev)
        fl = "".join(c for c, b in (("U", 1), ("D", 2), ("R", 4)) if s["flags"] & b)
        mark = " <-- 低于阈值" if s["trq"] < t["thresh"] else ""
        print("  %5dms steps=%-6d %-7s trq=%-4d %s%s" % (
            s["ms"], s["steps"], ds, s["trq"], fl or "-", mark))
        prev = s["steps"]
    print("=" * 78)


def verdict(t):
    """根因判读: 按 App_Stepper.c 黑匣子注释的判读法逐条对号."""
    print("判读:")
    notes = []
    hist, smp = t["hist"], list(reversed(t["smp"]))
    if not hist:
        notes.append("  - 无停机记录: 黑匣子随上电清零, 需在堵转发生后不 断电 取数; "
                     "或用 --watch 复位后整场盯。")
        for n in notes:
            print(n)
        return
    last = hist[0]
    tail = smp[-10:] if smp else []
    ol3 = [h for h in hist if h["reason"] == 3]

    if last["reason"] == 3:
        adv = all((tail[i + 1]["steps"] - tail[i]["steps"]) > 0
                  for i in range(len(tail) - 1)) if len(tail) >= 2 else None
        avg = sum(s["trq"] for s in tail) / len(tail) if tail else None
        if avg is not None and avg < 100:
            notes.append("  - 末段 TRQ 均值 %.0f (接近失速区): 脉冲在走、转子没跟 -> 真失步堵转"
                         " (转矩不足或机构卡; 开环步数照走, 3s 保护停机)." % avg)
        elif avg is not None and avg < t["thresh"]:
            notes.append("  - 末段 TRQ 均值 %.0f 只是略低于阈值 %d: 疑似过载监测误判 "
                         "(阈值不随 %d%% 转矩档缩放); 建议提高回零转矩复测或校准阈值."
                         % (avg, t["thresh"], t["trqPct"]))
        if len(ol3) >= 2:
            st = sorted(h["steps"] for h in ol3)
            spread = st[-1] - st[0]
            if spread < 300:
                notes.append("  - %d 次高负载停机 steps=%s~%d (相近): 同一位置机械卡点, "
                             "查机构." % (len(ol3), st[0], st[-1]))
            else:
                notes.append("  - %d 次高负载停机位置分散 (steps %d~%d): 非固定卡点, "
                             "更像转矩/负载裕量不足." % (len(ol3), st[0], st[-1]))
    elif last["reason"] == 4:
        notes.append("  - DRV 器件级停机, fault=%s: 按位查 供电压降(UVLO)/过流(OCP)/"
                     "过温(TF)/接线(OL)." % fault_str(last["fault"]))
    elif last["reason"] == 1:
        notes.append("  - 正常停机路径 (触点判定/外部停): 若机构当时并未到触点, "
                     "查样本 flags 的 KEY 位 (行程开关误触发, 回零 hit 无防抖).")
    key_hits = [s for s in smp if (s["flags"] & 3) and s["ms"] > 200]
    if len(key_hits) > 0 and (not smp or smp[-1]["steps"] < 3000):
        notes.append("  - 中途(>200ms)出现行程开关按下标志 %d 拍: 若机构未到触点, "
                     "为开关信号误触发/抖动 (PC8/PC9)." % len(key_hits))
    if not notes:
        notes.append("  - 未见异常特征; 结合实时态与 HISTORY 人工判读.")
    for n in notes:
        print(n)


def io_diag(link):
    d = tx(link, FC_IO_DIAG, [])
    if not d or len(d) < 10 or d[0] != 0:
        return None
    return {"keyUp": d[2], "keyDown": d[3], "homingStat": d[7]}


def dump(link):
    d = tx(link, FC_MOTOR, [SUB_TRACE], t=5.0, echo=SUB_TRACE)
    if not d or len(d) < 20 or d[0] != SUB_TRACE:
        print("TRACE 取回失败 (len=%s): 固件需含 MOTOR_CMD_TRACE 支持且设备已枚举"
              % (len(d) if d else None))
        return 1
    t = parse_trace(d)
    io = io_diag(link)
    if io:
        print("IO: keyUp=%d(1=释放) keyDown=%d(1=释放) 回零=%s" % (
            io["keyUp"], io["keyDown"], HOMING.get(io["homingStat"], io["homingStat"])))
    print_trace(t)
    verdict(t)
    return 0


def reopen(link, timeout_s=15):
    """USB 重枚举后旧句柄会写失败 (device not responding), 重开直到成功."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            link.close()
            link.open()
            return True
        except Exception:
            time.sleep(0.2)
    return False


def watch(seconds):
    link = HidLink()
    link.open()
    print("发送 FC_RESET 复位设备, 等待重新枚举...")
    link.transaction(FC_RESET, b"")
    link.close()
    time.sleep(1.0)                       # 等设备摘除, 再等重枚举
    if not reopen(link):
        print("设备未重新枚举")
        return 1
    print("已重连. 盯上电自检+回零 (%ds)..." % seconds)
    t0 = time.time()
    last = None
    while time.time() - t0 < seconds:
        try:
            q = tx(link, FC_MOTOR, [SUB_QUERY], echo=SUB_QUERY)
            io = io_diag(link)
        except Exception:                 # 枚举窗口抖动: 重连续跑
            if not reopen(link):
                time.sleep(0.3)
            continue
        if q and len(q) >= 10 and q[1] == 0 and io:
            cur = (q[2], io["homingStat"], q[6] | (q[7] << 8), io["keyUp"], io["keyDown"])
            if cur != last:
                print("[%5.1fs] stepper=%s 回零=%s steps=%d keyUp=%d keyDown=%d" % (
                    time.time() - t0, STEPPER_STATE.get(cur[0], cur[0]),
                    HOMING.get(cur[1], cur[1]), cur[2], cur[3], cur[4]))
                last = cur
                if cur[1] in (2, 3):  # 回零结束(就绪/失败)
                    break
        time.sleep(0.2)
    time.sleep(0.5)
    return dump(link)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--watch", action="store_true", help="复位设备并整场盯自检+回零")
    ap.add_argument("--keep", type=int, default=40, help="--watch 盯的秒数 (默认 40)")
    a = ap.parse_args()
    if a.watch:
        sys.exit(watch(a.keep))
    link = HidLink()
    link.open()
    sys.exit(dump(link))


if __name__ == "__main__":
    main()
