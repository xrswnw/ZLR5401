#!/usr/bin/env python3
"""Round_013 协议重构冒烟: FC 0x20~0x26 新码全通道 + 旧码拒绝 + 示例帧 CRC 回验 + 实机响应捕获.

新码常量内嵌 (不复用 Round_008 旧 const, 旧脚本保持历史原样).
输出所有示例帧/响应帧原始十六进制, 供 App_Protocol.html 尾部示例帧章节回填.
"""
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr.frame import build_frame, parse_frame, rsp_func  # noqa: E402

# ---- 新 FC 码 (Round_013 重映射) ----
FC_HANDSHAKE = 0x01
FC_MOTOR = 0x20
FC_UHF = 0x21
FC_AM = 0x22
FC_LOCKER = 0x23
FC_RGB = 0x24
FC_SELFTEST = 0x25
FC_IO_DIAG = 0x26

DEV_ADDR = 0x01

results = []


def record(name, ok, msg):
    results.append((name, ok, msg))
    tag = "PASS" if ok is True else ("FAIL" if ok is False else "记录")
    print(f"  [{tag}] {name}: {msg}")


def tx(link, fc, data, t=3.0):
    t0 = time.time()
    resp, to = link.transaction(fc, bytes(data), timeout_s=t)
    return resp, to, time.time() - t0


def tx_sub(link, fc, sub, data, t=3.0):
    """流程中交互: 跳过异步推帧 (0x0B~0x0F), 收回显 sub 的响应。"""
    t0 = time.time()
    link.send_frame(DEV_ADDR, fc, bytes(data))
    deadline = time.time() + t
    while time.time() < deadline:
        f = link.recv_frame(timeout_s=max(0.2, deadline - time.time()))
        if f is None or f["func"] != (fc ^ 0xFF):
            continue
        if f["data"] and f["data"][0] == sub:
            return f, False, time.time() - t0
    return None, True, time.time() - t0


def drain_terminal(link, fc, timeout_s):
    """收事件流直至终帧 (data[0]=0x0A), 返回 (events, terminal|None)."""
    events = []
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        f = link.recv_frame(timeout_s=min(2.0, max(0.2, deadline - time.time())))
        if f is None:
            continue
        if f["func"] != (fc ^ 0xFF):
            continue
        d = f["data"]
        if d and d[0] == 0x0A:
            return events, f
        events.append((d[0] if d else -1, f))
    return events, None


def unlock_cmd(tmo_ms, hold_ms, soft, epcs):
    elen = len(epcs[0])
    d = [0x0A, tmo_ms & 0xFF, tmo_ms >> 8, hold_ms & 0xFF, hold_ms >> 8,
         soft, len(epcs), elen]
    for e in epcs:
        d += list(e)
    return d


EPC1 = b"\xAA" * 6
EPC2 = b"\xBB" * 6
EPC3 = b"\x55" * 6


def main():
    link = HidLink()
    link.open()
    print("== Round_013 协议重构冒烟: FC 0x20~0x26 ==")
    time.sleep(1)

    # ---- T2 握手: protoVer 必须为 3 ----
    hs, to, dt = tx(link, FC_HANDSHAKE, [])
    if to:
        record("T2 HANDSHAKE", False, "无响应")
        link.close()
        return
    print(f"  握手响应 raw: {hs['_raw'].hex().upper()}")
    ok = hs["data"][1] == 3
    record("T2 HANDSHAKE protoVer=3", ok,
           f"protoVer={hs['data'][1]} status={hs['data'][2]} "
           f"layer={hs['data'][19]} (响应 {len(hs['data'])}B)")

    # ---- T3 新码全通道冒烟 ----
    r, to, _ = tx(link, FC_MOTOR, [0x05])
    record("T3a MOTOR QUERY(0x20)", (not to) and r["data"][1] == 0,
           f"err={r['data'][1] if r else '超时'} state={r['data'][2] if r else '?'}")

    r, to, _ = tx(link, FC_UHF, [0x01], t=6)
    uhf_open_ok = (not to) and r["data"][1] == 0
    record("T3b UHF OPEN(0x21)", uhf_open_ok, f"err={r['data'][1] if r else '超时'}")

    inv, to, _ = tx(link, FC_UHF, [0x03, 0xE8, 0x03], t=8)
    if not to:
        cnt = inv["data"][2] | (inv["data"][3] << 8)
        record("T3c UHF INVENTORY(0x21/0x03)", inv["data"][1] == 0,
               f"err={inv['data'][1]} tags={cnt}")
        print(f"  盘点响应 raw ({len(inv['_raw'])}B): {inv['_raw'].hex().upper()}")
    else:
        record("T3c UHF INVENTORY(0x21/0x03)", False, "超时")

    r, to, _ = tx(link, FC_AM, [0x06])
    record("T3d AM GET_STATUS(0x22)", not to,
           f"link={r['data'][2] if r and len(r['data']) > 2 else '?'} "
           f"deact={r['data'][10] if r and len(r['data']) > 10 else '?'}"
           " (link=0 链路正常, deact 字段如实反映消磁状态)")

    r, to, _ = tx(link, FC_LOCKER, [0x05])
    record("T3e LOCKER QUERY(0x23)", (not to) and r["data"][1] == 0,
           f"err={r['data'][1] if r else '超时'} state={r['data'][2] if r else '?'}")

    r, to, _ = tx(link, FC_RGB, [0x01, 0x00, 0x00])
    record("T3f RGB SET mask=0(0x24)", (not to) and r["data"][1] == 0,
           f"err={r['data'][1] if r else '超时'}")

    r, to, _ = tx(link, FC_SELFTEST, [0x01])
    record("T3g SELFTEST QUERY(0x25)", (not to) and r["data"][1] == 0,
           f"err={r['data'][1] if r else '超时'} "
           f"errBits={r['data'][2] | (r['data'][3] << 8) if r and len(r['data']) > 3 else '?'}")

    r, to, _ = tx(link, FC_IO_DIAG, [])
    record("T3h IO_DIAG(0x26)", (not to) and r["data"][0] == 0,
           f"io 快照 {len(r['data'])}B: {list(r['data']) if r else '?'}")

    # ---- T4 旧码拒绝 (0x0A/0x0D/0x10 已让位) ----
    for old_fc, name in [(0x0A, "0x0A MOTOR 旧码"), (0x0D, "0x0D LOCKER 旧码"),
                         (0x10, "0x10 IO_DIAG 旧码")]:
        r, to, _ = tx(link, old_fc, [0x05], t=1.5)
        record(f"T4 {name}", to, "无响应 (已废码位, 与未知 FC 一致)" if to
               else f"意外响应 err={r['data'][1]}")

    # ---- T5 解锁通道冒烟 (0x23/0x0A) ----
    r, to, _ = tx(link, FC_LOCKER, [0x0A, 0xE8, 0x03])
    record("T5a P1 短帧->PARAM", (not to) and r["data"][1] == 2,
           f"err={r['data'][1] if r else '超时'}")

    # 假 EPC + 短窗 3s: 磁块不动; 感应区有真标则光电触发 -> 校对窗满
    # PARTIAL_TIMEOUT (0 确认); 无标则 NO_IR。两者均为正确收尾。
    t0 = time.time()
    link.send_frame(DEV_ADDR, FC_LOCKER, bytes(unlock_cmd(1000, 3000, 0, [EPC1])))
    events, term = drain_terminal(link, FC_LOCKER, 3 + 10)
    dt = time.time() - t0
    if term is None:
        record("T5b 短窗到期收尾 (W=3s)", False, f"终帧超时 ({dt:.1f}s)")
    else:
        acks = [e for e in events if e[0] == 0x0F]
        print(f"  受理帧 raw: {acks[0][1]['_raw'].hex().upper()}" if acks else "  受理帧缺失!")
        print(f"  终帧 raw:   {term['_raw'].hex().upper()}")
        end = term["data"][2]
        conf = term["data"][4]
        rise = term["data"][6] | (term["data"][7] << 8)
        w_ok = (len(acks) == 1 and acks[0][1]["data"][2] == 1
                and (acks[0][1]["data"][3] | (acks[0][1]["data"][4] << 8)
                     | (acks[0][1]["data"][5] << 16)) == 3000)
        ok = (term["data"][1] == 0 and end in (2, 11) and w_ok and rise == 0)
        record("T5b 短窗到期收尾 (W=3s)", ok,
               f"err=0 end={'PARTIAL_TIMEOUT(标签在场,0确认)' if end == 2 else 'NO_IR(无标签)'} "
               f"confirmed={conf} rise=0 磁块未动 耗时{dt:.1f}s")

    # ---- T6 示例帧 CRC 回验 + 捕获 ----
    print("\n-- T6 示例帧 (将写入协议尾部, 实机原样回验) --")
    examples = [
        ("握手请求", FC_HANDSHAKE, b""),
        ("UHF OPEN 请求", FC_UHF, bytes([0x01])),
        ("UHF 盘点请求 (tmo=1000ms)", FC_UHF, bytes([0x03, 0xE8, 0x03])),
        ("解锁 1 标签请求 (epcCnt=1, W=120s)",
         FC_LOCKER, bytes(unlock_cmd(1000, 0, 0, [EPC1]))),
        ("解锁 2 标签请求 (epcCnt=2, W=150s)",
         FC_LOCKER, bytes(unlock_cmd(1000, 0, 0, [EPC1, EPC2]))),
        ("解锁 3 标签请求 (epcCnt=3, W=180s)",
         FC_LOCKER, bytes(unlock_cmd(1000, 0, 0, [EPC1, EPC2, EPC3]))),
    ]
    for name, fc, data in examples:
        raw = build_frame(DEV_ADDR, fc, data)
        print(f"  {name}: {raw.hex().upper()}")
        # 原样字节回验 (绕过 send_frame 组帧, 直接发文档字节)
        link.send_bytes(raw)
        f = link.recv_frame(timeout_s=4.0)
        if fc == FC_LOCKER:
            # 解锁请求会开流程: 收 0x0F 受理帧后立即 CANCEL
            while f is not None and not (f["func"] == (FC_LOCKER ^ 0xFF)
                                         and f["data"][0] == 0x0F):
                f = link.recv_frame(timeout_s=2.0)
            rc, to, _ = tx_sub(link, FC_LOCKER, 0x04, [0x04])
            events, term = drain_terminal(link, FC_LOCKER, 10)
            okc = (not to) and term is not None and term["data"][2] == 6
            print(f"      -> 受理后 CANCEL 回 OK={not to} 终帧 endReason="
                  f"{term['data'][2] if term else '?'} (6=ABORTED)")
        else:
            while f is not None and f["func"] != (fc ^ 0xFF):
                f = link.recv_frame(timeout_s=2.0)
        ok = f is not None
        record(f"T6 {name} 字节回验", ok, "设备受理" if ok else "设备未受理 (CRC/帧错误)")

    # 坏 CRC 变体: 设备应静默丢弃
    raw = bytearray(build_frame(DEV_ADDR, FC_HANDSHAKE, []))
    raw[-1] ^= 0xFF
    link.send_bytes(bytes(raw))
    f = link.recv_frame(timeout_s=1.5)
    record("T6 坏 CRC 静默丢弃", f is None,
           "无响应 (正确)" if f is None else "意外响应")

    # ---- R1 事后恢复 ----
    time.sleep(0.5)
    q, to = link.transaction(FC_LOCKER, bytes([0x05]), timeout_s=3)
    record("R1 LOCKER 恢复", (not to) and q["data"][2] == 0,
           f"locker={q['data'][2] if q else '超时'}")

    link.close()
    print("\n== 结果汇总 ==")
    ok = sum(1 for _, o, _ in results if o is True)
    bad = sum(1 for _, o, _ in results if o is False)
    print(f"PASS {ok} / FAIL {bad} / 记录 {len(results) - ok - bad}")


if __name__ == "__main__":
    main()
