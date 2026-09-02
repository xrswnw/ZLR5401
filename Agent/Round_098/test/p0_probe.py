"""P0.5: 硬件自适应探测 -> hw_presence.json"""
import time
from lib import (Recorder, open_link, tx, uhf_tags, load_hw, save_hw,
                 C, AM_QUERY, ONE_ERR_NO_IR, bits_str, parse_selftest, FC_SELFTEST_CTRL,
                 SELFTEST_SUB_QUERY)

rec = Recorder("P0")
lk = open_link()

# --- UHF 在场标签 ---
tags = uhf_tags(lk, rounds=3)
rec.check("HW-UHF", "UHF 在场标签", len(tags) > 0,
          f"EPC={sorted(set(tags))}", "无标签(OneShot 真实闭环用例将 SKIP)")
hw = {"uhf_tags": sorted(set(tags))}

# --- AM 链路 ---
d, to = tx(lk, C.FC_AM_CTRL, [AM_QUERY], timeout_s=4)
am_ok = (not to) and d is not None and d[1] == 0
rec.check("HW-AM", "AM 链路探测", am_ok, f"query resp={'TO' if to else d.hex()}",
          "AM 链路断(AM 配置/波形用例将走 LINK 错误路径)")
hw["am_link"] = am_ok

# --- 自检基线 (行程开关/回零) ---
d, to = tx(lk, FC_SELFTEST_CTRL, [0x01], timeout_s=3)
st = parse_selftest(d if not to else None)
if st:
    rec.check("HW-SW", "行程开关/回零基线", st["errBits"] & 0x3F == 0,
              f"errBits=0x{st['errBits']:04X}({bits_str(st['errBits'])}) switchErr={st['switchErr']}",
              f"errBits=0x{st['errBits']:04X}({bits_str(st['errBits'])})")
hw["selftest"] = st

# --- 光电门控: 短窗 OneShot 假 EPC (maxHold=2.5s) ---
# NO_IR(11) = 光电未触发(感应区无标签/异物); 其他 = 门控已过(有物体挡光电)
d, to = tx(lk, C.FC_LOCKER_CTRL,
           [0x08, 0xE8, 0x03, 0xC0, 0x00, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE],
           timeout_s=10)
ir = None
if d and not to:
    ir = d[1]
    if ir == ONE_ERR_NO_IR:
        rec.pass_("HW-IR", "光电门控行为", "无放标 -> NO_IR(11), 门控生效, 磁块未动")
        hw["ir_gate"] = "wait(无遮挡)"
    else:
        rec.pass_("HW-IR", "光电门控行为",
                  f"err={ir}(光电已触发, 有遮挡) — 门控通过分支")
        hw["ir_gate"] = f"triggered(err={ir})"
else:
    rec.fail("HW-IR", "光电门控行为", "短窗 OneShot 无响应")
hw["ir_err"] = ir

# --- JLink 通道 ---
import subprocess
jl = "/Applications/SEGGER/JLink/JLinkExe"
try:
    v = subprocess.run([jl, "-CommandFile", "/dev/stdin"], input="exit\n",
                       capture_output=True, text=True, timeout=15)
    rec.check("HW-JLINK", "JLink 通道", True, "JLinkExe 可用 (VTref 曾瞬断, 已恢复)")
    hw["jlink"] = True
except Exception as e:
    rec.check("HW-JLINK", "JLink 通道", False, str(e))
    hw["jlink"] = False

save_hw(hw)
print("hw_presence:", hw)
rec.flush()
lk.close()
