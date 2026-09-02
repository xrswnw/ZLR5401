"""P9b — Boot fault 风暴保护演练 (优化 #7).

原理: 风暴计数器在 RAM 保留区 0x20002A00 (magic) / 0x20002A04 (count),
仅 App 成功启动 (跳转前 BootStorm_Clear) 或 IAP 提交/EXIT_BOOT 才清零.
JLink 直接注入计数后复位, 验证:
  T1  count=3 -> Boot 常驻升级循环 (2.5s 窗口之外仍 Layer=0)
  T2  EXIT_BOOT 救援 -> App 正常启动 + 风暴清零
  T3  清零后普通复位 -> 正常 2.5s 窗口启动 (不再锁定)
  T4  count=2 (阈值下) -> 正常启动 (不锁定)
"""
import subprocess
import sys
import time

sys.path.insert(0, '.')
sys.path.insert(0, '../../Round_008/test')
from lib import Recorder, open_link, tx, reopen_until_alive, C

JLINK = "/Applications/SEGGER/JLink/JLinkExe"
MAGIC_ADDR, CNT_ADDR, MAGIC = "20002A00", "20002A04", "53544F52"

rec = Recorder("P9b-BootStorm")


def jlink_write_count(n):
    """注入风暴计数并复位 (RAM 跨复位保持)."""
    import tempfile, os
    cmd = f"""device GD32F303RCT6
si SWD
speed 4000
w4 {MAGIC_ADDR} {MAGIC}
w4 {CNT_ADDR} {n:08X}
r
g
exit
"""
    fd, path = tempfile.mkstemp(suffix=".jlink")
    with os.fdopen(fd, "w") as f:
        f.write(cmd)
    try:
        p = subprocess.run([JLINK, "-CommandFile", path],
                           capture_output=True, text=True, timeout=30)
        return p.returncode == 0 and "O.K." in p.stdout
    finally:
        os.unlink(path)


def handshake_layer(tmo=2):
    """单次握手, 返回 Layer 字节 (0=Boot / 1=App) 或 None."""
    try:
        lk = open_link()
        d, to = tx(lk, C.FC_HANDSHAKE, [0x01], timeout_s=tmo)
        lk.close()
        if not to and d is not None and len(d) > 19:
            return d[19]
    except Exception:
        pass
    return None


# ---- T1: count=3 -> 风暴锁定 ----
ok = jlink_write_count(3)
rec.check("T1a", "JLink 注入 count=3 + 复位", ok, "RAM 写入+复位", "JLink 失败")
time.sleep(5.0)   # 正常 2.5s 窗口早已过期
lay = handshake_layer()
rec.check("T1b", "T+5s (正常窗口外) 仍 Layer=0 — Boot 常驻升级循环",
          lay == 0, f"layer={lay}", f"layer={lay}")
time.sleep(3.0)
lay2 = handshake_layer()
rec.check("T1c", "T+8s 持续 Layer=0 (锁定非瞬态)", lay2 == 0, f"layer={lay2}", f"layer={lay2}")

# ---- T2: EXIT_BOOT 救援 ----
lk = open_link()
d, to = tx(lk, C.FC_EXIT_BOOT, [], timeout_s=3)
rec.check("T2a", "EXIT_BOOT 回 OK", d is not None and d[0] == 0,
          f"rsp={d.hex() if d else 'TO'}", f"to={to} d={d.hex() if d else None}")
lk.close()
lk, el = reopen_until_alive(20)
rec.check("T2b", "EXIT_BOOT 后 App 启动 (Layer=1)", lk is not None,
          f"{el:.1f}s" if lk else "TO", f"{el:.1f}s")
if lk:
    lk.close()

# ---- T3: 风暴已清零 — 普通复位走正常窗口 ----
lk = open_link()
d, to = tx(lk, C.FC_RESET, [], timeout_s=2)
lk.close()
lk, el = reopen_until_alive(20)
rec.check("T3", "清零后普通复位 -> 正常启动 (≤12s)", lk is not None and el <= 12.0,
          f"{el:.1f}s" if lk else "TO", f"{el:.1f}s")
if lk:
    lk.close()

# ---- T4: count=2 (阈值下) 不锁定 ----
ok = jlink_write_count(2)
rec.check("T4a", "JLink 注入 count=2 + 复位", ok, "RAM 写入+复位", "JLink 失败")
lk, el = reopen_until_alive(20)
rec.check("T4b", "count=2 < 阈值3 -> 正常启动 (≤12s, 不锁定)",
          lk is not None and el <= 12.0, f"{el:.1f}s" if lk else "TO", f"{el:.1f}s")
if lk:
    # App 启动后应清零计数 — 再复位一次确认无残留锁定
    d, to = tx(lk, C.FC_RESET, [], timeout_s=2)
    lk.close()
    lk, el = reopen_until_alive(20)
    rec.check("T4c", "App 启动清计数后再复位 -> 仍正常启动", lk is not None,
              f"{el:.1f}s" if lk else "TO", f"{el:.1f}s")
    if lk:
        lk.close()

n = rec.flush()
print("== P9b-BootStorm 完成 ==")
sys.exit(0 if n["FAIL"] == 0 else 1)
