"""USB HID transport wrapper (VID/PID 0x5377:0x5378, report ID 0x02)."""
import time

import hid

from . import const
from .frame import build_frame, parse_frame


class HidLink:
    def __init__(self, vid=const.VID, pid=const.PID):
        self.vid = vid
        self.pid = pid
        self.dev = None

    def open(self):
        self.dev = hid.Device(self.vid, self.pid)
        self.dev.nonblocking = 1
        return True

    def close(self):
        if self.dev is not None:
            try:
                self.dev.close()
            except Exception:
                pass
            self.dev = None

    def send_bytes(self, raw):
        """Send arbitrary protocol bytes (frame), prefixed with report id, padded to 64."""
        pkt = bytearray(const.PACKET)
        pkt[0] = const.REPORT_ID
        pkt[1:1 + len(raw)] = raw
        self.dev.write(bytes(pkt))

    def send_frame(self, dev_addr, func, data=b"", reserved=const.RESERVED):
        self.send_bytes(build_frame(dev_addr, func, data, reserved))

    def recv_frame(self, timeout_s=1.0):
        """Read reports, strip report id, return first valid parsed frame."""
        deadline = time.time() + timeout_s
        raw = b""
        while time.time() < deadline:
            r = self.dev.read(64, 50)  # ms
            if r:
                payload = bytes(r)
                body = payload[1:] if payload and payload[0] == const.REPORT_ID else payload
                raw = raw + body
                # try parse from start, consume one frame
                f, leftover = parse_frame(raw)
                if f is not None:
                    return f
                # if we have more than one frame worth, keep latest tail accumulating
                raw = leftover
            else:
                time.sleep(0.005)
        return None

    def transaction(self, req_func, data=b"", timeout_s=1.0, dev_addr=None,
                    reserved=const.RESERVED):
        """Build + send a request. Returns (resp_frame|None, timed_out:bool)."""
        if dev_addr is None:
            dev_addr = const.DEV_ADDR
        self.send_frame(dev_addr, req_func, data, reserved)
        resp = self.recv_frame(timeout_s)
        return resp, resp is None
