#!/usr/bin/env python3
"""消磁模式实测监控 — AM_SET_MODE(0) 后轮询 GET_STATUS 5 分钟, 捕获 cmd17 上报.

用法: python3 demag_monitor.py [持续秒数, 默认300]
输出: 每条 cmd17 帧到达/消磁状态变化实时打印 (EVT 前缀), 结束给汇总。
"""
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr import const                     # noqa: E402

FC_AM = const.FC_AM_CTRL
AM_SET_MODE = 0x07
AM_GET_STATUS = 0x06
MODE_DEMAG = 0

DEACT = {0: "空闲", 1: "成功", 2: "失败"}

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
POLL_S = 0.15


def get_status(link):
    r, to = link.transaction(FC_AM, bytes([AM_GET_STATUS]), timeout_s=1.5)
    if to or not r:
        return None
    d = r["data"]
    evt = int.from_bytes(d[2:6], "little")
    last = int.from_bytes(d[6:10], "little")
    deact = d[10] if len(d) > 10 else -1
    return d[1], evt, last, deact


def main():
    link = HidLink()
    link.open()
    print("== 消磁模式实测监控 ==")

    # 1) 切换到消磁模式 (mode=0, 持久化)
    r, to = link.transaction(FC_AM, bytes([AM_SET_MODE, MODE_DEMAG]), timeout_s=3)
    if to or not r or r["data"][1] != 0:
        err = "无响应" if to else f"err={r['data'][1] if r else '?'}"
        print(f"!! 切换消磁模式失败: {err}")
        link.close()
        return 1
    print("EVT 模式切换: 消磁模式 (mode=0) OK")

    # 2) 初始状态
    st = get_status(link)
    if st is None:
        print("!! GET_STATUS 无响应")
        link.close()
        return 1
    link_st, evt0, last0, deact0 = st
    print(f"EVT 初始: link={link_st} evtCount={evt0} lastEvtMs={last0} deact={DEACT.get(deact0, deact0)}")

    t0 = time.time()
    arrivals = []          # 每个 cmd17 新帧的相对时刻
    deact_seen = {1: 0, 2: 0}
    last_evt = evt0
    last_deact = deact0
    last_link = link_st
    poll_fail = 0

    while time.time() - t0 < DURATION:
        st = get_status(link)
        if st is None:
            poll_fail += 1
            continue
        link_st, evt, last, deact = st
        now = time.time() - t0

        if link_st != last_link:
            print(f"EVT [{now:7.1f}s] 链路状态变化: {last_link} -> {link_st}")
            last_link = link_st

        if evt != last_evt:
            delta = evt - last_evt
            print(f"EVT [{now:7.1f}s] cmd17 上报 +{delta} 帧 (evtCount={evt}) "
                  f"deact={DEACT.get(deact, deact)} lastEvtMs={last}")
            arrivals.append(now)
            last_evt = evt
        if deact != last_deact and deact in (0, 1, 2):
            print(f"EVT [{now:7.1f}s] 消磁状态: {DEACT.get(last_deact, last_deact)} -> {DEACT.get(deact, deact)}")
            if deact in deact_seen:
                deact_seen[deact] += 1
            last_deact = deact

        time.sleep(POLL_S)

    # 3) 汇总
    frames = last_evt - evt0
    bursts = 0
    if arrivals:
        bursts = 1
        for i in range(1, len(arrivals)):
            if arrivals[i] - arrivals[i - 1] > 0.45:
                bursts += 1
    print("\n== 汇总 ==")
    print(f"EVT SUMMARY 监控时长={DURATION:.0f}s 轮询失败={poll_fail}次 链路终态={last_link}")
    print(f"EVT SUMMARY cmd17 新增帧数={frames} 估算突发轮次={bursts} "
          f"成功状态出现={deact_seen[1]}次 失败状态出现={deact_seen[2]}次")
    link.close()


if __name__ == "__main__":
    main()
