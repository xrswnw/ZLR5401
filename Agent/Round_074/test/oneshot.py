#!/usr/bin/env python3
"""LOCKER_SUB_ONE_SHOT (0x08) 单标签同步开锁闭环测试 — Round_074.

用法:
  python3 oneshot.py                # 自适应: 自动发现感应区标签, 全用例
  python3 oneshot.py --remove       # 保持期人工移除标签 (测 endReason=1 防抖回降)
  python3 oneshot.py --skip-motion   # 跳过真实升降用例 (不发会动磁块的命令)
"""
import argparse
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr import const                     # noqa: E402

FC = const.FC_LOCKER_CTRL
SUB = 0x08
ERR_NAME = {0: "OK", 1: "BUSY", 2: "PARAM", 3: "UHF_OPEN", 4: "UHF_LINK",
            5: "NO_TAG", 6: "MISMATCH", 7: "HOMING", 8: "MOTOR_FAULT", 9: "MOTOR_TIMEOUT",
            10: "AM_LINK"}
END_NAME = {1: "TAG_REMOVED", 2: "HOLD_TIMEOUT", 3: "TAG_CHANGED", 4: "UHF_LOST",
            5: "DEMAG_DONE"}
RETREAT = {0: "回退成功", 1: "回退失败", 2: "未尝试"}

results = []


def record(name, ok, msg):
    results.append((name, ok, msg))
    print(f"  [{'PASS' if ok else 'FAIL' if ok is False else 'SKIP/记录'}] {name}: {msg}")


def tx(link, data, t=2.0):
    t0 = time.time()
    resp, to = link.transaction(FC, bytes(data), timeout_s=t)
    return resp, to, time.time() - t0


def discover_epc(link, tries=3):
    """UHF 同步盘点一轮, 返回第一张真实标签 EPC (无则 None).
    单轮 1s 盘点偶发漏读在场标签 (模块 0x22 时序), 连试多轮取首轮命中。"""
    for _ in range(tries):
        link.transaction(const.FC_UHF_CTRL, bytes([const.UHF_OPEN]), timeout_s=4)
        ri, to = link.transaction(const.FC_UHF_CTRL,
                                  bytes([const.UHF_INVENTORY, 0xE8, 0x03]), timeout_s=6)
        if ri and ri["data"][1] == const.UHF_ERR_OK:
            cnt = ri["data"][2] | (ri["data"][3] << 8)
            if cnt and len(ri["data"]) >= 6:
                elen = ri["data"][5]
                return bytes(ri["data"][6:6 + elen])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--remove", action="store_true", help="保持期人工移除标签")
    ap.add_argument("--skip-motion", action="store_true", help="跳过升降用例")
    ap.add_argument("--hold-ms", type=int, default=4000)
    args = ap.parse_args()

    link = HidLink()
    link.open()
    print("== LOCKER_SUB_ONE_SHOT (0x08) 闭环测试 ==")

    # 干净起点: 取消 Locker 到 IDLE
    tx(link, [const.LOCKER_CANCEL])

    # P1/P2/P3 参数非法 (epcLen=0 / 13 / 短帧) — 立即回, 不动磁块
    for name, payload in [
        ("P1 epcLen=0", [SUB, 0xE8, 0x03, 0x00, 0x00, 0x00]),
        ("P2 epcLen=13", [SUB, 0xE8, 0x03, 0x00, 0x00, 13] + [0xAA] * 13),
        ("P3 短帧", [SUB, 0xE8, 0x03]),
    ]:
        resp, to, dt = tx(link, payload)
        if to:
            record(name, False, "无响应")
        else:
            e = resp["data"][1]
            record(name, e == 2, f"err={e}({ERR_NAME.get(e)}) 耗时{dt:.3f}s")

    # 发现真实标签
    print("\n-- 感应区标签发现 --")
    epc = discover_epc(link)
    print(f"  发现 EPC: {epc.hex() if epc else '无'}")

    if not epc:
        # N1 无标签: tmo=1000 -> NO_TAG, 耗时 ≈1.5s
        resp, to, dt = tx(link, [SUB, 0xE8, 0x03, 0x00, 0x00, 6] + [0xAA] * 6, t=8)
        if to:
            record("N1 无标签->NO_TAG", False, "无响应")
        else:
            d = resp["data"]
            record("N1 无标签->NO_TAG",
                   d[1] == 5,
                   f"err={d[1]}({ERR_NAME.get(d[1])}) rawErr={d[2] if len(d)>2 else '?'} 耗时{dt:.3f}s")
    else:
        # M1 错误 EPC (有真标签在场): -> MISMATCH + 回传读到的 EPC
        resp, to, dt = tx(link, [SUB, 0xE8, 0x03, 0x00, 0x00, 12] + [0xAA] * 12, t=8)
        if to:
            record("M1 错误EPC->MISMATCH", False, "无响应")
        else:
            d = resp["data"]
            tags = d[2] if len(d) > 2 else 0
            elen = d[3] if len(d) > 3 else 0
            got = d[4:4 + elen].hex() if elen else "-"
            record("M1 错误EPC->MISMATCH",
                   d[1] == 6 and elen > 0,
                   f"err={d[1]}({ERR_NAME.get(d[1])}) tagsFound={tags} 读到EPC={got} 耗时{dt:.3f}s")

        if not args.skip_motion:
            # M2 全流程 (不带 demagCnt 字节, 缺省 0=跳过消磁):
            # 升 KEY_UP -> 保持 holdMs (标签在场 -> HOLD_TIMEOUT) -> 回降 KEY_DOWN
            hold = args.hold_ms if not args.remove else 15000
            print(f"\n-- 全流程 (hold={hold}ms, demagCnt 缺省0=跳过消磁)"
                  f"{', 请在磁块升起后【取走标签】' if args.remove else ''} --")
            resp, to, dt = tx(link, [SUB, 0xE8, 0x03, hold & 0xFF, hold >> 8, len(epc)] + list(epc),
                              t=hold / 1000 + 30)
            if to:
                record("M2 全流程(跳过消磁)", False, "无响应 (超时)")
            else:
                d = resp["data"]
                if d[1] == 0:
                    endr = d[2]
                    elen = d[3]
                    rise = d[4 + elen] | (d[5 + elen] << 8)
                    lower = d[6 + elen] | (d[7 + elen] << 8)
                    demag = d[8 + elen] if len(d) > 8 + elen else 0
                    record("M2 全流程(跳过消磁)", True,
                           f"err=OK end={END_NAME.get(endr)} rise={rise} lower={lower} "
                           f"demagDone={demag} 总耗时{dt:.1f}s")
                else:
                    extra = " ".join(f"{b:02X}" for b in d[2:])
                    record("M2 全流程(跳过消磁)", False,
                           f"err={d[1]}({ERR_NAME.get(d[1])}) diag=[{extra}] 耗时{dt:.1f}s")

            # P4 显式 demagCnt=0: EPC 后追加 0 字节, 应与 M2 同样正常 (参数位不破坏流程)
            resp, to, dt = tx(link, [SUB, 0xE8, 0x03, 500 & 0xFF, 500 >> 8, len(epc)] + list(epc) + [0],
                              t=40)
            if to:
                record("P4 显式demagCnt=0", False, "无响应 (超时)")
            else:
                d = resp["data"]
                if d[1] == 0:
                    endr = d[2]
                    elen = d[3]
                    rise = d[4 + elen] | (d[5 + elen] << 8)
                    lower = d[6 + elen] | (d[7 + elen] << 8)
                    demag = d[8 + elen] if len(d) > 8 + elen else 0
                    record("P4 显式demagCnt=0", endr in (2, 1) and demag == 0,
                           f"err=OK end={END_NAME.get(endr)} rise={rise} lower={lower} "
                           f"demagDone={demag} 耗时{dt:.1f}s")
                else:
                    extra = " ".join(f"{b:02X}" for b in d[2:])
                    record("P4 显式demagCnt=0", False,
                           f"err={d[1]}({ERR_NAME.get(d[1])}) diag=[{extra}] 耗时{dt:.1f}s")

    # B1 忙互斥: Locker CONFIGURED(+START) 中下发 0x08 -> BUSY 带三态
    print("\n-- 忙互斥 --")
    tx(link, [const.LOCKER_CONFIGURE, 1, 0, 0, 0])
    tx(link, [const.LOCKER_ADD, 6, 1, 2, 3, 4, 5, 6])
    tx(link, [const.LOCKER_START])
    resp, to, dt = tx(link, [SUB, 0xE8, 0x03, 0x00, 0x00, 6] + [0xAA] * 6, t=8)
    if to:
        record("B1 Locker忙->BUSY", False, "无响应")
    else:
        d = resp["data"]
        st = (d[2], d[3], d[4]) if len(d) >= 5 else ("?", "?", "?")
        record("B1 Locker忙->BUSY",
               d[1] == 1 and d[2] == 1,
               f"err={d[1]}({ERR_NAME.get(d[1])}) locker/uhf/stepper={st} 耗时{dt:.3f}s")
    tx(link, [const.LOCKER_CANCEL])

    # R1 事后恢复: 设备响应 + Locker IDLE + 电机 IDLE
    resp, to, dt = tx(link, [const.LOCKER_QUERY])
    q = tx(link, [const.LOCKER_QUERY])[0]
    motor = link.transaction(const.FC_MOTOR_CTRL, bytes([const.MOTOR_QUERY]), timeout_s=3)[0]
    lstate = q["data"][2] if q else -1
    mstate = motor["data"][2] if motor else -1
    record("R1 事后恢复", lstate == 0 and mstate == 0,
          f"locker={lstate} motor={mstate}")

    link.close()
    print("\n== 结果汇总 ==")
    ok = sum(1 for _, o, _ in results if o is True)
    bad = sum(1 for _, o, _ in results if o is False)
    print(f"PASS {ok} / FAIL {bad} / 记录 {len(results) - ok - bad}")


if __name__ == "__main__":
    main()
