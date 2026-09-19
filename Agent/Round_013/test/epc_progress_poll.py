"""0x10 EPC 解锁流程进行中的 GET_PROGRESS 轮询 (Round_013 循环模式验证)。

用法: python3 epc_progress_poll.py [轮询秒数, 缺省 30]
循环模式 (0x10 rsv0>0) 运行时另一终端执行:
  softCnt=周期数 softDone=成功周期数 lastCycle=0无/1成功/2移除/3更换
  phase: 1=等放标 2=校对 3=升起 7=顶部保持 4=回降
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER = 0x23
PH = {0: "无", 1: "等放标", 2: "校对", 3: "升起", 4: "回降", 5: "软标", 7: "顶部保持"}
LASTCYC = {0: "无/进行中", 1: "成功", 2: "移除", 3: "更换"}

lnk = HidLink(); lnk.open()
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
t0 = time.time()
last = None
while time.time() - t0 < dur:
    lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
    f = lnk.recv_frame(1.0)
    if f and f["func"] == (FC_LOCKER ^ 0xFF) and len(f["data"]) >= 12 and f["data"][0] == 0x09:
        d = f["data"]
        cur = (d[2], int.from_bytes(d[3:6], "little"), d[6], d[7], d[8], d[9], d[10], d[11])
        if cur != last:
            print(f"[{time.time()-t0:6.1f}s] phase={d[2]}({PH.get(d[2],'?')}) holdMs={cur[1]} "
                  f"total={d[6]} confirmed={d[7]} bm={d[8]:02X} cycles={d[9]} ok={d[10]} "
                  f"lastCycle={d[11]}({LASTCYC.get(d[11],'?')})")
            last = cur
    time.sleep(0.2)
lnk.close()
