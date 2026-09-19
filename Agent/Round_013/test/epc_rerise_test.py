"""0x10 循环模式 "回退中放回即再升" 实测 (Round_013 裁决: retract-interrupt re-rise)。

场景 (用户操作): 标签在位下发 120s 窗 -> 升起中(心跳嗒嗒)移走标签 ->
电机停+回退 -> 回退途中把标签贴回 -> 应中停回退直接再升 (确认鸣 ->
心跳 -> 保持 -> 成功)。
判定: 相位轨迹出现 3(RISE)->4(LOWER)->2(VERIFY)->3(RISE) — 回退后
不经过 1(再入场闸) 直接再升; cycles>=2 (失败1次+再入场成功)。
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER = 0x23
EPC = bytes.fromhex("33553463a4000158eb7e7a50")
TMO, WIN, HOLD, TS = 100, 20000, 3000, 120
PH = {0: "NONE", 1: "WAIT_TAG(闸)", 2: "VERIFY", 3: "RISE", 4: "LOWER", 7: "HOLD"}
ENDR = {1: "ALL_OK", 2: "EPC_LOST", 3: "TIMEOUT", 4: "UHF_LOST", 6: "ABORTED"}


def main():
    lnk = HidLink()
    lnk.open()

    def io_diag():
        lnk.send_frame(1, 0x26, bytes([0]))
        dl = time.time() + 0.5
        while time.time() < dl:
            f = lnk.recv_frame(0.05)
            if f and f["func"] == 0xD9:
                return f["data"]
        return None

    # 等回零就绪 (复位后 POST 自动回零)
    t0 = time.time()
    while time.time() - t0 < 45:
        io = io_diag()
        if io and io[7] == 2:
            print(f"[{time.time()-t0:.1f}s] 回零就绪, 3s 后下发 — 请准备: 升起中移走标签, 回退一启动就贴回")
            break
        time.sleep(1)
    else:
        print("!! 回零超时")
        sys.exit(1)
    time.sleep(3)

    req = bytes([0x10, len(EPC), TMO & 0xFF, TMO >> 8, WIN & 0xFF, WIN >> 8,
                 HOLD & 0xFF, HOLD >> 8, TS, 0, 0, 0]) + EPC
    lnk.send_frame(1, FC_LOCKER, req)
    print(f"已下发 0x10 (120s 窗) — 升起心跳嗒嗒时移走标签, 电机回退启动即贴回")
    t0 = time.time()
    seq, last, terminal, pushes = [], None, None, []
    next_poll = 0.0
    while time.time() - t0 < TS + 8:
        now = time.time()
        f = lnk.recv_frame(0.05)
        if f and f["func"] == (FC_LOCKER ^ 0xFF):
            d = f["data"]
            if d[0] == 0x10:
                terminal = d
                break
            if d[0] == 0x09 and len(d) >= 12:
                if d[2] != last:
                    seq.append((now - t0, d[2]))
                    print(f"[{now-t0:6.1f}s] phase={PH.get(d[2], d[2])} cycles={d[9]} ok={d[10]} lastCycle={d[11]}")
                    last = d[2]
            if d[0] in (0x0F, 0x0B):
                pushes.append((now - t0, d.hex()))
        if now >= next_poll:
            next_poll = now + 0.3
            lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
    if terminal is None:
        print("!! 未收到终帧")
        sys.exit(1)
    err, endr = terminal[1], terminal[2]
    rise = terminal[4] | (terminal[5] << 8)
    lower = terminal[6] | (terminal[7] << 8)
    cyc, okc, failc = terminal[14], terminal[15], terminal[16]
    print(f"\n终帧: err={err} endReason={endr} ({ENDR.get(endr,'?')}) rise={rise} "
          f"lower={lower} 周期={cyc}/{okc}/{failc}")
    for t, p in pushes:
        print(f"  推帧[{t:.1f}s] {p}")
    ph_seq = [p for _, p in seq]
    # 判定: 出现 RISE->LOWER->VERIFY->RISE (回退中放回, 未过闸)
    rerise = any(ph_seq[i:i+4] == [3, 4, 2, 3] for i in range(len(ph_seq) - 3))
    print(f"\n相位序列: {'>'.join(PH.get(p, str(p)) for p in ph_seq)}")
    print(f"回退中放回再升 (3>4>2>3 免闸): {'PASS' if rerise else '未见'}; "
          f"周期自洽: {'PASS' if cyc == okc + failc else 'FAIL'}")
    lnk.close()


if __name__ == '__main__':
    main()
