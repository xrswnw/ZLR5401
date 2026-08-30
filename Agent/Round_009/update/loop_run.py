"""闭环更新驱动器/基线.

用法:
  python -m Agent.Round_009.update.loop_run [--smoke] [--rounds N] [--inject] [--fault F1,U3] [--verbose]

阶段:
  precheck : 连接 + 载入固件 + CRC 自检 + 读取当前 upgradeCount
  smoke    : 单次完整更新 (--smoke)
  inject   : 故障/USB 注入用例 (--inject 或 --fault 指定)
  baseline : N 轮完整更新 (--rounds, 默认 20)
退出码 0 = 全 PASS。
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "Round_008", "test"))

from . import conf
from .zlr_updater import Updater


def precheck(up):
    print("\n== precheck ==")
    up.load_fw()
    d = up.read_count()
    if d is None:
        print("FATAL: no device (HID not enumerated or layer unknown)")
        return False
    count, layer = d[0], d[1]
    print(f"  device present: layer={layer} (0=Boot,1=App) upgradeCount={count}")
    return True


def ensure_in_boot(up):
    """确保进入 Boot 状态；已在 Boot 则不动 (保留 IAP 上下文)。"""
    up.load_fw()
    d = up.read_count()
    if d is not None and d[1] == 1:
        up.enter_boot()


def run_smoke(up):
    print("\n== smoke: single full update ==")
    ensure_in_boot(up)
    before = up.read_count()
    res = up.full_update(before_count=before)
    print("  smoke result:", res)
    return res.get("ok", False)


def get_in_updater(up):
    """确保当前处于 Boot (App 则先进 boot)。"""
    ensure_in_boot(up)
    up.wait_layer(0)


def run_inject(up, only=None):
    from . import fault_inject as fi
    print("\n== fault / USB instability injection ==")
    results = []
    for name, desc, fn in fi.CASES:
        if only and name not in only:
            continue
        print(f"\n  --- {name} {desc} ---")
        try:
            get_in_updater(up)
            verdict, detail = fn(up)
        except Exception as ex:
            import traceback
            traceback.print_exc()
            verdict, detail = "FAIL", f"exception: {ex}"
        results.append({"case": name, "desc": desc, "verdict": verdict, "detail": detail})
        print(f"  [{name}] {verdict}: {detail}")
    return results


def run_baseline(up, rounds):
    print(f"\n== baseline: {rounds} rounds full update ==")
    seq = []
    for i in range(1, rounds + 1):
        up.load_fw()
        before = up.read_count()
        if before is None:
            seq.append({"round": i, "ok": False, "detail": "no-device"})
            print(f"  [round {i}] FAIL no-device")
            continue
        # 确保从 App 开始
        cur_layer = before[1]
        if cur_layer == 0:
            # 在 Boot: 用 EXIT_BOOT 回 App (前提 App 完好, 无须重传)
            up.cmd(conf.FC_EXIT_BOOT)
            up.reopen()
            up.load_fw()
            before = up.read_count()
            cur_layer = before[1] if before else -1
        t0 = time.time()
        try:
            if cur_layer == 1:
                up.enter_boot()
            res = up.full_update(before_count=before)
        except Exception as ex:
            res = {"ok": False, "detail": f"exception: {ex}"}
        dt = time.time() - t0
        ok = res.get("ok", False)
        seq.append({"round": i, "ok": ok, "detail": res, "sec": round(dt, 1)})
        mark = "PASS" if ok else "FAIL"
        print(f"  [round {i}/{rounds}] {mark} {res} ({dt:.1f}s)")
        if not ok:
            print("  ABORT baseline on failure")
            break
    return seq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--rounds", type=int, default=0)
    ap.add_argument("--inject", action="store_true")
    ap.add_argument("--fault", default="")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--out", default="update_report.json")
    args = ap.parse_args()

    up = Updater(verbose=args.verbose)
    if not up.open_best_effort():
        print("FATAL: cannot open HID device")
        sys.exit(2)
    if not precheck(up):
        sys.exit(2)

    report = {"precheck": "PASS", "smoke": None, "inject": [], "baseline": []}
    overall_ok = True

    if args.smoke:
        ok = run_smoke(up)
        report["smoke"] = ok
        overall_ok = overall_ok and ok

    if args.fault or args.inject:
        only = set(args.fault.split(",")) if args.fault else None
        inj = run_inject(up, only)
        report["inject"] = inj
        overall_ok = overall_ok and all(x["verdict"] == "PASS" for x in inj)

    if args.rounds:
        base = run_baseline(up, args.rounds)
        report["baseline"] = base
        overall_ok = overall_ok and all(x["ok"] for x in base)

    # 结束前把设备留在 App
    up.load_fw()
    d = up.read_count()
    if d is not None and d[1] == 0:
        try:
            resp, to = up.cmd(conf.FC_EXIT_BOOT)
            up.reopen()
            up.wait_layer(1)
        except Exception:
            pass

    report["overall"] = "PASS" if overall_ok else "FAIL"
    out_path = os.path.join(os.path.dirname(__file__), os.path.basename(args.out))
    with open(out_path, "w") as fh:
        json.dump(report, fh, indent=2, ensure_ascii=False, default=str)
    print("report ->", out_path)
    print("\n== overall:", report["overall"], "==")
    sys.exit(0 if overall_ok else 1)


if __name__ == "__main__":
    main()
