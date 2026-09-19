"""0x21 单标签盘点耗时标定 (标签在位) — Round_013 EPC 解锁切单标签指令前置测试。

UHF_SUB_INVENTORY_ONE 0x11: [0x11, tmoL, tmoH] -> [0x11, err, (epcLen, epc..), msL, msH]
ms = 设备侧发帧 -> 收模块响应耗时 (真模块往返); 主机侧 wall time 另计 (含 USB HID)。

用法: python3 uhf_one_tag.py [tmo_ms] [rounds]   (缺省 100ms × 30 轮)
"""
import sys, time, statistics
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink
from motor_trace import tx   # 严格配对事务 (排空陈旧回显 + func/echo 匹配)

FC_UHF = 0x21
SUB_OPEN, SUB_QUERY, SUB_INV_ONE = 0x01, 0x07, 0x11
ERR_NAMES = {0: 'OK', 1: 'PARAM', 2: 'BUSY', 3: 'NOT_READY', 4: 'LINK', 5: 'NO_TAG', 6: 'TIMEOUT'}


def u16le(b, o):
    return b[o] | (b[o + 1] << 8)


def main():
    tmo = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 30

    lnk = HidLink()
    lnk.open()

    # 模块上电 + 就绪 (幂等)
    r = tx(lnk, FC_UHF, bytes([SUB_OPEN]))
    if r is None or r[1] != 0:
        print(f"UHF_OPEN 失败: {r.hex() if r else '无响应'}")
        sys.exit(1)
    print("UHF_OPEN: OK")

    hits, dev_ms, host_ms, epcs = 0, [], [], set()
    print(f"--- 0x21 单标签盘点 × {rounds} 轮, tmo={tmo}ms, 标签在位 ---")
    for i in range(rounds):
        t0 = time.time()
        d = tx(lnk, FC_UHF, bytes([SUB_INV_ONE, tmo & 0xFF, tmo >> 8]))
        dt = (time.time() - t0) * 1000.0
        if d is None:
            print(f"  #{i + 1:02d} 无响应 (超时)")
            continue
        err = d[1]
        if err == 0:
            el = d[2]
            epc = bytes(d[3:3 + el]).hex()
            ms = u16le(d, 3 + el)
            hits += 1
            dev_ms.append(ms)
            host_ms.append(dt)
            epcs.add(epc)
            mark = ''
        else:
            # NO_TAG/TIMEOUT: [sub, err, msL, msH]
            ms = u16le(d, 2) if len(d) >= 4 else None
            dev_ms.append(ms)
            mark = f" <- {ERR_NAMES.get(err, err)}"
        print(f"  #{i + 1:02d} err={err}{mark} devMs={ms} hostMs={dt:.1f}")

    print(f"\n命中 {hits}/{rounds} ({hits * 100 // rounds}%), EPC 去重 {len(epcs)} 张")
    for e in epcs:
        print(f"  EPC: {e}")
    ok_ms = [m for m in dev_ms if m is not None]
    if dev_ms:
        print(f"devMs  全轮: min={min(ok_ms)} avg={statistics.mean(ok_ms):.1f} "
              f"max={max(ok_ms)} (n={len(ok_ms)})")
    if host_ms:
        print(f"hostMs 命中轮: min={min(host_ms):.1f} avg={statistics.mean(host_ms):.1f} "
              f"max={max(host_ms):.1f}")
    if hits == rounds:
        print("结论: 全命中 — 0x21 单标签轮询可行, 采样粒度≈avg(devMs)")
    lnk.close()


if __name__ == '__main__':
    main()
