"""One-off: AM link test over USB HID (FC_AM_CTRL)."""
import sys
import time

sys.path.insert(0, ".")
from zlr.const import VID, PID, DEV_ADDR, FC_AM_CTRL
from zlr.hid_link import HidLink

AM_QUERY = 0x05
AM_GET_CONFIG = 0x01
AM_GET_STATUS = 0x06


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


def main():
    link = HidLink()
    link.open()
    print(f"opened HID {VID:04X}:{PID:04X}")

    for name, sub in [("QUERY", AM_QUERY), ("GET_STATUS", AM_GET_STATUS), ("GET_CONFIG", AM_GET_CONFIG)]:
        link.send_frame(DEV_ADDR, FC_AM_CTRL, bytes([sub]))
        f = link.recv_frame(timeout_s=3.0)
        if f is None:
            print(f"{name}: no response (timeout)")
        else:
            data = f.get("data") or f.get("payload") or b""
            print(f"{name}: fc={f.get('func'):02X} err={data[1] if len(data) > 1 else '?'} raw=[{hexs(data)}]")

    link.close()


if __name__ == "__main__":
    main()
