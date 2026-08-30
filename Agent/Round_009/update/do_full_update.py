#!/usr/bin/env python3
"""通过 Boot IAP 重新执行一次升级流程, 让 Boot 用 EXEC 记录正确 CRC 并跳转到已烧好的 App.
设备当前停在 Boot(layer=0). 直接复用 Round_009 updater 全流程.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zlr_updater import Updater

def main():
    up = Updater(verbose=True)
    if not up.open_best_effort():
        print("FATAL: cannot open HID")
        return 2
    up.load_fw()
    r = up.full_update()
    print("full_update ->", r)
    return 0 if (r.get("ok") or r.get("stage") in ("app_layer","count_after")) else 1

if __name__ == "__main__":
    sys.exit(main())
