#!/usr/bin/env python3
"""M9c 真实软标解码闭环 — Round_012 续 (2026-09-03 台架: 双硬标 + 1 软标)。

前置: 感应区放真硬标 (自动发现), 软标置于 AM 天线区。
下发 0x0A m=发现数, softCnt=1:
  期望 双0x0B -> 升起(同时开AM消磁, 裁决4) -> 0x0E[done=1] -> 0x0D -> 回降+AM恢复检测
  终帧 end=ALL_OK(1), softDone=1/1, AM deactCnt +1, mode 回 1 (裁决1)。
对照: M9b (--soft-timeout) 已验无软标 5min 窗 -> end=7 SOFT_TIMEOUT。

用法: python3 soft_real.py
"""
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr import const                     # noqa: E402

from unlock_multi import (                # noqa: E402
    tx, discover_epc, start_unlock, build_cmd, drain_terminal,
    expect_push, parse_terminal, record, results,
    ERR_NAME, END_NAME, FC, RSP,
)

FC_AM = const.FC_AM_CTRL
AM_GET_STATUS = 0x06
AM_GET_PARAM = 0x03
AM_CMD_MODE = 0x50


def am_tx(link, data, t=4.0):
    """AM 通道请求 (tx() 固定走 LOCKER FC, 不能复用)。"""
    r, to = link.transaction(FC_AM, bytes(data), timeout_s=t)
    return r, to


def am_status(link):
    r, to = am_tx(link, [AM_GET_STATUS], t=4)
    if to or not r or len(r["data"]) < 19:
        return None
    d = r["data"]
    return {
        "link": d[1], "deact": d[10],
        "deactCnt": int.from_bytes(d[11:15], "little"),
        "failCnt": int.from_bytes(d[15:19], "little"),
    }


def am_mode(link):
    r, to = am_tx(link, [AM_GET_PARAM, AM_CMD_MODE], t=6)
    if to or not r:
        return None
    d = r["data"]
    return (d[3] << 8) | d[4] if len(d) >= 5 else None


def main():
    link = HidLink()
    link.open()
    print("== M9c 真实软标解码闭环 (双硬标 + 1 软标) ==")
    tx(link, [const.LOCKER_CANCEL])

    tags = discover_epc(link)
    print(f"  感应区硬标 {len(tags)} 张: {[t.hex() for t in tags]}")
    if not tags:
        record("M9c 前置", False, "感应区无真标")
        link.close()
        return

    base = am_status(link)
    if base is None:
        record("M9c 前置", False, "AM GET_STATUS 无响应")
        link.close()
        return
    print(f"  AM 基线: deactCnt={base['deactCnt']} failCnt={base['failCnt']} link={base['link']}")
    record("M9c 前置 AM 基线", True,
           f"deactCnt={base['deactCnt']} failCnt={base['failCnt']}")

    epcs = tags[:4]
    print(f"\n-- M9c 全流程 (m={len(epcs)}, softCnt=1, W=30s: 硬标勿取走, 软标勿取走) --")
    t0 = time.time()
    start_unlock(link, build_cmd(1000, 30000, 1, epcs))
    events, term = drain_terminal(link, 330)   # 覆盖成功(~15s)与失败(5min窗)两路
    dt = time.time() - t0

    if term is None:
        record("M9c 全流程", False, f"终帧超时 ({dt:.1f}s)")
    else:
        t = term["data"]
        if t[1] != 0:
            extra = " ".join(f"{b:02X}" for b in t[2:])
            record("M9c 全流程", False, f"err={t[1]}({ERR_NAME.get(t[1])}) diag=[{extra}]")
        else:
            p = parse_terminal(t)
            confs = expect_push(events, 0x0B)
            softs = expect_push(events, 0x0E)
            hards = expect_push(events, 0x0D)
            seqs = sorted(c[1][1] for c in confs)
            ok = (p["end"] == 1 and p["softCnt"] == 1 and p["softDone"] == 1
                  and len(softs) >= 1 and len(confs) == len(epcs)
                  and seqs == list(range(1, len(epcs) + 1))
                  and p["rise"] > 0 and p["lower"] > 0
                  and p["confirmed"] == len(epcs))
            soft_desc = ";".join(f"done={s[1][1]} softCnt={s[1][2]}" for s in softs)
            record("M9c 全流程 结账完成", ok,
                   f"end={END_NAME.get(p['end'])} confirmed={p['confirmed']}/{p['total']} "
                   f"softDone={p['softDone']}/{p['softCnt']} 0x0E帧={len(softs)}[{soft_desc}] "
                   f"rise/lower={p['rise']}/{p['lower']} elapsed={p['elapsed']}ms 总耗时{dt:.1f}s")

            # 裁决4 旁证: 升起即开消磁, 软标可在硬标段内先行解码 (0x0E 早于 0x0D)
            ih = next((i for i, e in enumerate(events) if e[0] == 0x0D), None)
            isf = next((i for i, e in enumerate(events) if e[0] == 0x0E), None)
            record("M9c-并行 消磁先于硬标段结束 (裁决4)", None,
                   f"事件序: 0x0E@{isf} vs 0x0D@{ih} "
                   f"({'并行解码成立' if (isf is not None and ih is not None and isf < ih) else '顺序完成 (软标较慢, 仍合规)'})")

    # 事后: AM 消磁计数 +1, 模式回检测 (裁决1), locker/motor 回 IDLE
    time.sleep(1)
    post = am_status(link)
    if post is None:
        record("M9c-事后 AM deactCnt", False, "GET_STATUS 无响应")
    else:
        ok = (post["deactCnt"] >= base["deactCnt"] + 1
              and post["failCnt"] == base["failCnt"])
        record("M9c-事后 AM deactCnt +1 / failCnt 不变", ok,
               f"deactCnt {base['deactCnt']}->{post['deactCnt']} "
               f"failCnt {base['failCnt']}->{post['failCnt']}")

    mode = am_mode(link)
    record("M9c-事后 AM mode 回 1(仅检测, 裁决1)", mode == 1, f"mode={mode}")

    q, _ = link.transaction(FC, bytes([const.LOCKER_QUERY]), timeout_s=3)
    motor = link.transaction(const.FC_MOTOR_CTRL, bytes([const.MOTOR_QUERY]), timeout_s=3)[0]
    ls = q["data"][2] if q else -1
    ms = motor["data"][2] if motor else -1
    record("M9c-事后 locker/motor IDLE", ls == 0 and ms == 0, f"locker={ls} motor={ms}")

    link.close()
    print("\n== 结果汇总 ==")
    ok = sum(1 for _, o, _ in results if o is True)
    bad = sum(1 for _, o, _ in results if o is False)
    print(f"PASS {ok} / FAIL {bad} / 记录 {len(results) - ok - bad}")


if __name__ == "__main__":
    main()
