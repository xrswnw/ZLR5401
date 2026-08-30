#!/usr/bin/env python3
"""读电机 QUERY 状态(只读, 不驱动). """
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zlr import const
from zlr.hid_link import HidLink

link = HidLink()
try:
    link.open()
except Exception:
    print("NOUSB"); sys.exit(2)
d, to = link.transaction(const.FC_MOTOR_CTRL, bytes([0x05]), timeout_s=2.0)
if to or not d:
    print("NORESP"); sys.exit(1)
x = d['data']
print(f"len={len(x)} cmd={x[0]} err={x[1]} state={x[2]} switchErr={x[8]:#x}")
link.close()
