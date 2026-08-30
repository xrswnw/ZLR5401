#!/usr/bin/env python3
"""直接 MOVE 电机微步, 验证驱动通路 (不经回零). 读 MOVE 后步骤. """
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zlr import const
from zlr.hid_link import HidLink

link = HidLink()
try:
    link.open()
except Exception:
    print("NOUSB"); sys.exit(2)

def query():
    d, to = link.transaction(const.FC_MOTOR_CTRL, bytes([0x05]), timeout_s=1.5)
    if to or not d:
        return None
    x = d['data']
    return (x[2], x[8], x[6] | (x[7] << 8))

q0 = query()
print("pre  :", q0)

# MOVE 300 微步 dir=0 (CW). 设备回零应已就绪 (switchErr=0)
r, to = link.transaction(const.FC_MOTOR_CTRL,
                         bytes([0x01, 0, 300 & 0xFF, (300 >> 8) & 0xFF, 0]), timeout_s=1.5)
print("MOVE :", "noresp" if to else f"err={r['data'][1]}")

time.sleep(1.0)
q1 = query()
print("post :", q1)
if q0 and q1 and q1[2] > q0[2]:
    print("RESULT: 电机转动 (stepsDone 增加)")
else:
    print("RESULT: 电机未转或 stepsDone 未变")
link.close()
