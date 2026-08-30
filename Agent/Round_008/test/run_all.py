"""Run test cases in dependency order, print a table, produce report files.

Usage:
    python run_all.py [--phase P0..P7] [--only a,b,c] [--skip-hardware]
                      [--destructive] [--out PREFIX]
"""
import argparse
import importlib
import json
import sys
import time

sys.path.insert(0, ".")

from zlr.hid_link import HidLink
from zlr.result import Verdict

CASE_MODULES = [
    "layer_a_proto",   # A
    "layer_b_link",    # B
    "fc_handshake",    # C
    "fc_info",         # D
    "fc_rgb",          # J (safe, put early among functional)
    "fc_motor",        # G
    "fc_uhf",          # H
    "fc_locker",       # I
]
# destructive/reset cases run standalone (own connection) so an on-device
# reset mid-suite can't corrupt the deterministic sequential modules.
RESET_MODULES = ["fc_reset", "fc_boot"]  # E, F

PHASES = {
    "a": ["layer_a_proto"],
    "b": ["layer_b_link"],
    "c": ["fc_handshake"],
    "d": ["fc_info"],
    "e": ["fc_reset"],
    "f": ["fc_boot"],
    "g": ["fc_motor"],
    "h": ["fc_uhf"],
    "i": ["fc_locker"],
    "j": ["fc_rgb"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", default="all")
    ap.add_argument("--only")
    ap.add_argument("--destructive", action="store_true", help="include RESET/ENTER_BOOT")
    ap.add_argument("--skip-hardware", action="store_true",
                    help="skip motor/UHF/locker hardware-only cases (SKIP verdict)")
    ap.add_argument("--have-motor", action="store_true", default=None,
                    help="force motor present for TEST cases")
    ap.add_argument("--no-motor-test", action="store_true",
                    help="skip physical motor travel TEST cases (G12/G14)")
    ap.add_argument("--no-locker-motion", action="store_true",
                    help="skip physical locker magnet-block lift flow (I10/I11)")
    ap.add_argument("--boot-test", action="store_true",
                    help="include ENTER_BOOT/EXIT_BOOT round-trip (F1/F2)")
    ap.add_argument("--destructive-only", action="store_true",
                    help="run ONLY the reset/boot destructive modules (own connection)")
    ap.add_argument("--out", default="report")
    args = ap.parse_args()
    if args.have_motor is None:
        args.have_motor = not args.skip_hardware
    args.skip_hardware = args.skip_hardware or not args.have_motor

    # select module list
    if args.destructive_only:
        mods = list(RESET_MODULES)
    elif args.only:
        mods = [m for m in CASE_MODULES if m in args.only.split(",")]
    else:
        mods = list(CASE_MODULES)

    link = HidLink()
    try:
        link.open()
        print(f"[env] opened 0x{link.vid:04X}:0x{link.pid:04X}")
    except Exception as e:
        print(f"[env] FATAL cannot open device: {e}")
        return 1

    results = []
    ok = skipped = inconcl = failed = 0
    t0 = time.time()

    def ensure_link(force=False):
        if force or link.dev is None:
            try:
                if link.dev is not None:
                    link.close()
                link.open()
                return True
            except Exception:
                return False
        return True

    for m in mods:
        mod = importlib.import_module("cases." + m)
        # reconnect if a prior module reset/disconnected the device
        if not ensure_link():
            # try harder a few times (post-reset re-enum)
            import time as _t
            _t.sleep(0.5)
            if not ensure_link(force=True):
                print(f"[module {m}] cannot open device — skipping")
                continue
        try:
            before = set(id(r) for r in results)
            mod.run(results, link, args)
            # any Result without a verdict (module crashed mid-way) -> FAIL
            from zlr.result import Verdict
            for r in results:
                if r.verdict is None:
                    r.set(Verdict.FAIL, "case aborted by module exception")
        except Exception as e:
            import traceback
            traceback.print_exc()
            print(f"[module {m}] EXCEPTION: {e}")
            from zlr.result import Verdict
            for r in results:
                if r.verdict is None:
                    r.set(Verdict.FAIL, "case aborted by module exception")

    link.close()

    # tally
    for r in results:
        if r.verdict == Verdict.PASS:
            ok += 1
        elif r.verdict == Verdict.SKIP:
            skipped += 1
        elif r.verdict == Verdict.INCONCLUSIVE:
            inconcl += 1
        else:
            failed += 1

    print("\n==== results ====")
    for r in results:
        tag = {"PASS": "ok ", "FAIL": "FAIL", "SKIP": "skip", "INCONCLUSIVE": "INCON"}[r.verdict.value]
        print(f"[{tag}] {r.section:>3} {r.name:<6} {r.message}")
        if r.evidence:
            for e in r.evidence:
                print(f"        ev: {e}")

    elapsed = time.time() - t0
    print(f"\nTotal {len(results)} | PASS {ok} | FAIL {failed} | SKIP {skipped} | INCONCLUSIVE {inconcl} | {elapsed:.1f}s")

    # write report files
    payload = {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "phase": args.phase,
        "destructive": args.destructive,
        "total": len(results),
        "pass": ok,
        "fail": failed,
        "skip": skipped,
        "inconclusive": inconcl,
        "results": [r.to_dict() for r in results],
    }
    with open(args.out + ".json", "w") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
