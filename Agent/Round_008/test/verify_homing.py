#!/usr/bin/env python3
"""上电行程自检/回零 + 开关错误位验证.
设备为担保完好设备: 上电应回零到 KEY_DOWN 成功 (switchErr=0, 可 MOVE/TEST).
检查 QUERY 新增的第9字结 switchErr 是否上报. 只读查询, 不驱动电机.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zlr import const
from zlr.hid_link import HidLink

MOTOR_QUERY = 0x05

def main():
    link = HidLink()
    link.open()
    try:
        resp, to = link.transaction(const.FC_MOTOR_CTRL, bytes([MOTOR_QUERY]), timeout_s=2.0)
        if to or resp is None:
            print("FAIL: QUERY 无响应")
            return 1
        d = resp["data"]
        print(f"QUERY len={len(d)}")
        print(f"  cmd={d[0]} err={d[1]} state={d[2]} fault={d[3]:#x} "
              f"diag1={d[4]} diag2={d[5]}")
        if len(d) >= 9:
            sw = d[8]
            print(f"  stepsDone={d[6]|(d[7]<<8)} switchErr={sw:#x} "
                  f"(bit0=上错 bit1=下错)")
            print("  上行程错=" + ("YES" if sw & 0x01 else "no") +
                  " 下行程错=" + ("YES" if sw & 0x02 else "no"))
            if sw == 0:
                print("PASS: switchErr=0 -> 回零成功, 开关正常")
            else:
                print("FAIL/WARN: switchErr 置位 -> 上电自检未成功")
        else:
            print(f"旧固件? QUERY 只有 {len(d)} 字节, 无 switchErr 字段")
            return 1
        return 0
    finally:
        link.close()

if __name__ == "__main__":
    sys.exit(main())
