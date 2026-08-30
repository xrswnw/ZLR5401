import sys, os, time
sys.path.insert(0, os.path.abspath("."))
from zlr import const
from zlr.hid_link import HidLink
link = HidLink()
try:
    link.open()
except Exception:
    print("NOUSB"); sys.exit(2)
link.transaction(const.FC_MOTOR_CTRL, bytes([0x06]), timeout_s=1.5)  # CLEAR
r, to = link.transaction(const.FC_MOTOR_CTRL, bytes([0x01, 1, 0xD0, 0x07, 0]), timeout_s=1.5)  # down 2000
time.sleep(2.5)
d, to = link.transaction(const.FC_MOTOR_CTRL, bytes([0x05]), timeout_s=1.5)
if not to:
    x = d['data']
    print(f"moved-down: state={x[2]} fault=0x{x[3]:02X} stepsDone={x[6]|(x[7]<<8)}")
link.close()
