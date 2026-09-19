#!/usr/bin/env python3
"""iap_load — 最小 IAP 加载器: 把 App bin 经 Boot IAP 通道推入设备并 EXEC 提交.

用途: JLink 直烧 (flash_app/flash_all) 不更新参数区 appCrc32, Boot 校验不过
会常驻 Boot 不跳 App. 走本脚本 IAP (START->DATA->VERIFY->EXEC), EXEC 两步提交
把新 CRC/大小/deviceStatus=RUN 落盘, 复位后 Boot 放行跳 App.

用法: python3 iap_load.py [bin路径]   (默认取 Build/Firmware 最新 App_*.bin;
      设备须已在 Boot 层 — 卡 Boot 或刚 flash_all 后都满足)
"""
import glob
import os
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink          # noqa: E402
from zlr.frame import build_frame         # noqa: E402
from zlr.crc32 import crc32               # noqa: E402

FC_HANDSHAKE, FC_UPG_START, FC_FW_DATA, FC_VERIFY, FC_EXEC = 0x01, 0x03, 0x04, 0x05, 0x06
CHUNK = 240
VERIFY_LEVEL_MID = 1


def u16(v):
    return [v & 0xFF, (v >> 8) & 0xFF]


def u32(v):
    return [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF]


class BootLink(HidLink):
    def send_bytes_big(self, raw):
        for off in range(0, len(raw), 63):
            pkt = bytearray(64)
            pkt[0] = 0x01
            c = raw[off:off + 63]
            pkt[1:1 + len(c)] = c
            self.dev.write(bytes(pkt))
            time.sleep(0.001)


def txf(lk, fc, data=b"", timeout_s=3.0):
    try:
        r, to = lk.transaction(fc, bytes(data), timeout_s=timeout_s)
        return (r["data"] if r else None), to
    except Exception:
        return None, True


def wait_layer(timeout_s=20):
    """重开设备直至握手 layer=0 (Boot)."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        lk = None
        try:
            lk = BootLink()
            lk.open()
            d, _ = txf(lk, FC_HANDSHAKE, timeout_s=1.5)
            if d is not None and len(d) > 19 and d[19] == 0:
                lk.recv_frame(0.3)
                return lk
        except Exception:
            pass
        if lk is not None:
            try:
                lk.close()
            except Exception:
                pass
        time.sleep(0.4)
    return None


def main():
    if len(sys.argv) > 1:
        fw_path = sys.argv[1]
    else:
        cands = sorted(glob.glob("/Users/swnw/Documents/Software/ZLR5401/Build/Firmware/App_*.bin"))
        if not cands:
            print("无 App bin, 先 build_app.sh")
            return 1
        fw_path = cands[-1]
    fw = open(fw_path, "rb").read()
    size, crc = len(fw), crc32(fw)
    print("固件: %s (%d B, crc=%08X)" % (os.path.basename(fw_path), size, crc))

    # App 层则先进 Boot (ENTER_BOOT 置升级标志+复位); Boot 层直接开始
    try:
        pre = BootLink()
        pre.open()
        d, _ = txf(pre, FC_HANDSHAKE, timeout_s=1.5)
        if d is not None and len(d) > 19 and d[19] == 1:
            print("App 层 -> ENTER_BOOT 进 Boot ...")
            txf(pre, 0x02, timeout_s=3)
        pre.close()
    except Exception:
        pass

    lk = wait_layer()
    if not lk:
        print("设备未在 Boot 层就绪")
        return 1
    print("已连 Boot 层, UPGRADE_START ...")

    d, _ = txf(lk, FC_UPG_START, bytes(u32(size) + u32(crc) + u32(0x00010003) + u32(0) + [VERIFY_LEVEL_MID]), 15)
    if not d or d[0] != 0:
        print("START 失败: %s" % (d.hex() if d else "TO"))
        return 1

    n = off = 0
    t0 = time.time()
    while off < size:
        lk.send_bytes_big(build_frame(1, FC_FW_DATA, bytes(u16(n) + u32(off)) + fw[off:off + CHUNK]))
        ack = None
        tw = time.time()
        while time.time() - tw < 3:
            f = lk.recv_frame(1.0)
            if f and f["func"] == (FC_FW_DATA ^ 0xFF) and len(f["data"]) >= 3:
                if (f["data"][0] | (f["data"][1] << 8)) == n:
                    ack = f["data"]
                    break
        if not ack or ack[2] != 0:
            print("DATA seq=%d 失败: %s" % (n, ack.hex() if ack else "TO"))
            return 1
        off += min(CHUNK, size - off)
        n += 1
        if n % 20 == 0:
            print("  %d/%d B (%.0fs)" % (off, size, time.time() - t0))
    print("DATA 完成: %d 帧" % n)

    d, _ = txf(lk, FC_VERIFY, timeout_s=6)
    print("VERIFY: %s" % (("OK" if d[0] == 0 else "err=%d" % d[0]) if d else "TO"))
    if not d or d[0] != 0:
        return 1
    d, _ = txf(lk, FC_EXEC, timeout_s=6)
    print("EXEC: %s (复位中)" % ("OK" if d and d[0] == 0 else (d.hex() if d else "TO")))
    lk.close()

    for _ in range(60):  # 等 App 启动重新枚举 (最长 ~6s)
        time.sleep(0.1)
        try:
            k = BootLink()
            k.open()
            dd, _ = txf(k, FC_HANDSHAKE, timeout_s=1.5)
            if dd and len(dd) > 19 and dd[19] == 1:
                print("App 层已就绪 (layer=1) — IAP 闭环完成")
                k.close()
                return 0
            k.close()
        except Exception:
            pass
    print("App 未在 6s 内就绪, 人工确认")
    return 1


if __name__ == "__main__":
    sys.exit(main())
