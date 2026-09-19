"""0x10 EPC 解锁全流程闭环实测 (0x21 单标签轮询 + 循环周期模型) — Round_013。

前置: 设备已回零 (复位后 POST 自动回零), 标签在位 (IR 应触发)。
帧: 0x10 [epcLen, tmo(2), win(2), holdTop(2), rsv0=超时秒, rsv1..3, epc]。
  rsv0=0 单轮模式: IR 门控 -> 0x21 计数确认 -> 升起(监守) -> 保持 ->
        回降 -> 终帧 [.., cycles, okCycles, failCycles]。
  rsv0>0 循环模式: 受理起 rsv0 秒内周期往复 (回退后须标签移走+红外
        回落才开下一轮); 周期失败不推帧, 状态经 GET_PROGRESS 拉取
        (softCnt=周期数 softDone=成功数 lastCycle=0无/1成功/2移除/3更换)。
过程推帧 (0x0F 受理 / 0x0B 确认) 被严格配对过滤时顺带记录。
用法: python3 uhf_epc_flow.py [timeoutSec]   (缺省 0=单轮)
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_MOTOR, FC_LOCKER = 0x20, 0x23
MOTOR_QUERY = 0x05
HOMING = {0: "未启动", 1: "回零中", 2: "已就绪", 3: "失败"}
ENDR = {1: "ALL_OK", 2: "EPC_LOST", 3: "窗满 (单轮W/循环超时)", 4: "UHF_LOST", 6: "ABORTED"}
LASTCYC = {0: "无/进行中", 1: "成功", 2: "移除", 3: "更换"}
EPC = "33553463a4000158eb7e7a50"
TMO, WIN, HOLD = 100, 20000, 3000

pushes = []


def tx(link, fc, data, t=3.0, echo=None):
    """严格配对 + 顺带记录 0x23 推送帧 (0x0F/0x0B)。"""
    for _ in range(8):
        try:
            f = link.recv_frame(0.05)
            if f and f["func"] == (FC_LOCKER ^ 0xFF) and len(f["data"]) >= 1 \
                    and f["data"][0] in (0x0F, 0x0B):
                pushes.append(f["data"])
            if f is None:
                break
        except Exception:
            break
    link.send_frame(1, fc, bytes(data))
    deadline = time.time() + t
    while time.time() < deadline:
        f = link.recv_frame(max(0.05, min(0.5, deadline - time.time())))
        if f and f["func"] == (fc ^ 0xFF):
            if echo is None or (len(f["data"]) > 0 and f["data"][0] == echo):
                return f["data"]
            if f["func"] == (FC_LOCKER ^ 0xFF):
                pushes.append(f["data"])
    return None


def main():
    timeout_sec = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    lnk = HidLink()
    lnk.open()

    # 等回零就绪 (复位后 POST 自动回零, 最多 ~40s); 回零由 0x10 前置检查兜底
    time.sleep(8)

    epc = bytes.fromhex(EPC)
    req = bytes([0x10, len(epc), TMO & 0xFF, TMO >> 8, WIN & 0xFF, WIN >> 8,
                 HOLD & 0xFF, HOLD >> 8, timeout_sec, 0, 0, 0]) + epc
    mode = "循环" if timeout_sec else "单轮"
    print(f"下发 0x10 EPC 解锁 ({mode}): epc={EPC} tmo={TMO} win={WIN} "
          f"holdTop={HOLD} timeoutSec={timeout_sec}")
    t0 = time.time()
    d = tx(lnk, FC_LOCKER, req, t=90.0, echo=0x10)
    wall = time.time() - t0
    if d is None:
        print("!! 无终帧响应")
        sys.exit(1)

    err = d[1]
    print(f"\n终帧: err={err} wall={wall:.1f}s")
    if err == 0:
        endr, bm = d[2], d[3]
        rise = d[4] | (d[5] << 8)
        lower = d[6] | (d[7] << 8)
        el = d[8] | (d[9] << 8)
        rsv = d[10:14].hex()
        cyc, okc, failc = d[14], d[15], d[16]
        print(f"  endReason={endr} ({ENDR.get(endr, '?')})  bitmap={bm:02X}")
        print(f"  rise={rise} lower={lower} elapsed={el}s rsv={rsv}")
        print(f"  周期: cycles={cyc} ok={okc} fail={failc}")
        if timeout_sec == 0:
            verdict = (endr == 1 and bm == 1 and cyc == 1 and okc == 1 and failc == 0
                       and 4000 < rise < 5000 and 4000 < lower < 5000)
        else:
            # 循环模式: 窗满结束, 至少 1 个成功周期, 周期计数自洽
            verdict = (endr == 1 and cyc >= 1 and okc >= 1
                       and cyc == okc + failc and 4000 < rise < 5000)
        print(f"\n判定: {'PASS — 全流程闭环 OK' if verdict else 'CHECK — 见上字段'}")
    else:
        print(f"  诊断: {d.hex()}")
    print(f"过程推帧 {len(pushes)} 条:")
    for p in pushes:
        print(f"  0x{p[0]:02X}: {p.hex()}")
    lnk.close()


if __name__ == '__main__':
    main()
