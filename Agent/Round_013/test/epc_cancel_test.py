"""0x10 EPC 解锁 CANCEL 打断实测 (Round_013): 电机升起+标签在场时打断 -> 全部复位。

场景 A: 升起段 (phase=3) 打断 — 电机运行中被 CANCEL 停机 -> 免疫回降。
场景 B: 保持段 (phase=7) 打断 — 顶部保持监守中被 CANCEL -> 免疫回降。
每场景验证: CANCEL 立即回 OK; 终帧 err=0 endReason=6 (ABORTED), 回降步数
完整 (~4300); 打断后 GET_PROGRESS phase=0; IO_DIAG keyUp=0 keyDown=1
locker=IDLE homing=就绪。末尾重跑一轮完整流程 PASS 证明彻底复位。
前置: 标签在位 IR 遮挡, 设备回零就绪。
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER, FC_IO_DIAG = 0x23, 0x26
RESP = 0xFF
EPC = "33553463a4000158eb7e7a50"
TMO, WIN, HOLD = 100, 20000, 30000   # holdTop 30s: 保持段打断留足窗口

PH = {0: "NONE", 1: "WAIT_TAG", 2: "VERIFY", 3: "RISE", 4: "LOWER", 7: "HOLD"}
ENDR = {1: "ALL_OK", 2: "EPC_LOST", 3: "TIMEOUT", 4: "UHF_LOST", 6: "ABORTED"}


def req_flow():
    epc = bytes.fromhex(EPC)
    return bytes([0x10, len(epc), TMO & 0xFF, TMO >> 8, WIN & 0xFF, WIN >> 8,
                  HOLD & 0xFF, HOLD >> 8, 0, 0, 0, 0]) + epc


def scenario(lnk, name, target_ph):
    """下发 0x10 -> 0.2s 轮询 GET_PROGRESS -> 命中目标段发 CANCEL -> 收终帧."""
    print(f"\n=== 场景 {name}: phase={target_ph} ({PH[target_ph]}) 时 CANCEL ===")
    lnk.send_frame(1, FC_LOCKER, req_flow())
    t0 = time.time()
    cancel_t = term_t = None
    ph_seq, last_ph, terminal, cancel_ok = [], -1, None, None
    next_poll = 0.0
    while time.time() - t0 < 60:
        now = time.time()
        f = lnk.recv_frame(0.05)
        if f and f["func"] == (FC_LOCKER ^ RESP):
            d = f["data"]
            if d[0] == 0x10:
                terminal, term_t = d, now
                break
            if d[0] == 0x09 and len(d) >= 12:
                last_ph = d[2]
                if not ph_seq or ph_seq[-1] != last_ph:
                    ph_seq.append(last_ph)
            if d[0] == 0x04:
                cancel_ok = d[1]
        if now >= next_poll:
            next_poll = now + 0.2
            lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
            if cancel_t is None and last_ph == target_ph:
                lnk.send_frame(1, FC_LOCKER, bytes([0x04]))
                cancel_t = now
                print(f"  [{now - t0:5.2f}s] phase={PH[target_ph]} 确认 -> 已发 CANCEL")
    if terminal is None:
        print("  !! 无终帧")
        return None
    err, endr = terminal[1], terminal[2]
    rise = terminal[4] | (terminal[5] << 8)
    lower = terminal[6] | (terminal[7] << 8)
    el = terminal[8] | (terminal[9] << 8)
    cyc, okc = terminal[14], terminal[15]
    print(f"  CANCEL->终帧 {term_t - cancel_t:.2f}s (CANCEL回OK={cancel_ok == 0}) "
          f"phase轨迹: {'>'.join(PH.get(p, str(p)) for p in ph_seq)}")
    print(f"  终帧: err={err} endReason={endr} ({ENDR.get(endr, '?')}) rise={rise} "
          f"lower={lower} elapsed={el}s cycles={cyc}/{okc}")
    if target_ph == 3:   # 升起段打断: 从部分高度回底, 回降步数≈升起步数
        return err == 0 and endr == 6 and lower > 0 and abs(lower - rise) < 300
    return err == 0 and endr == 6 and 4000 < lower < 5000


def post_check(lnk, tag):
    """打断后复位快照: GET_PROGRESS phase + IO_DIAG 行程/电机/锁状态."""
    time.sleep(0.3)
    lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
    ph = None
    dl = time.time() + 1.5
    while time.time() < dl:
        f = lnk.recv_frame(0.1)
        if f and f["func"] == (FC_LOCKER ^ RESP) and f["data"][0] == 0x09:
            ph = f["data"][2]
            break
    lnk.send_frame(1, FC_IO_DIAG, bytes([0]))
    io = None
    dl = time.time() + 1.5
    while time.time() < dl:
        f = lnk.recv_frame(0.1)
        if f and f["func"] == (FC_IO_DIAG ^ RESP):
            io = f["data"]
            break
    ok = ph == 0 and io and io[2] == 1 and io[3] == 0 and io[7] == 2 and io[8] == 0
    print(f"  [{tag}] 复位: phase={ph} ({PH.get(ph, '?')}) IR={io[1]} keyUp={io[2]} "
          f"keyDown={io[3]} homing={io[7]} locker={io[8]} -> "
          f"{'复位 OK' if ok else '!! 复位异常'}")
    return ok


def main():
    lnk = HidLink()
    lnk.open()
    time.sleep(0.5)
    r_a = scenario(lnk, "A 升起段打断", 3)
    o_a = post_check(lnk, "A后")
    r_b = scenario(lnk, "B 保持段打断", 7)
    o_b = post_check(lnk, "B后")
    lnk.close()
    print(f"\n场景A(升起打断): {'PASS' if r_a else 'FAIL'}  复位: {'OK' if o_a else 'FAIL'}")
    print(f"场景B(保持打断): {'PASS' if r_b else 'FAIL'}  复位: {'OK' if o_b else 'FAIL'}")
    sys.exit(0 if (r_a and o_a and r_b and o_b) else 1)


if __name__ == '__main__':
    main()
