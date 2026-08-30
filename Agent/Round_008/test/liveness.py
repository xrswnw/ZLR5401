#!/usr/bin/env python3
"""最小存活检查 + boot 恢复. 设备可能停在 bootloader (层=0)."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zlr import const
from zlr.hid_link import HidLink

def handshake(link, t=1.0):
    resp, to = link.transaction(const.FC_HANDSHAKE, timeout_s=t)
    if to or resp is None:
        return None
    return resp["data"]

def main():
    link = HidLink()
    link.open()
    try:
        d = handshake(link)
        if d is not None:
            layer = d[0] if len(d) >= 1 else "?"
            print(f"ALIVE: handshake layer={layer} data={list(d)}")
            return 0
        print("dead-ish; trying EXIT_BOOT to recover to App...")
        link.transaction(const.FC_EXIT_BOOT, timeout_s=1.0)
        time.sleep(1.0)
        d = handshake(link)
        if d is not None:
            print(f"RECOVERED: handshake layer={d[0] if d else '?'} data={list(d)}")
            return 0
        print("still no response after EXIT_BOOT")
        return 1
    finally:
        link.close()

if __name__ == "__main__":
    sys.exit(main())
