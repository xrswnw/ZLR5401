#!/usr/bin/env python3
"""ONE_SHOT (0x08) 流程中交互闭环测试 — 泵内嵌 Proto_Poll / GET_PROGRESS / CANCEL 打断.

场景 (用户指定): 下发消磁标签数=3 (实际无消磁事件) 或 demagCnt=0 + 长保持窗,
流程阻塞等待期间: 拉取 0x09 进度 (phase/holdMs/tagPresent/demagDone),
验证 0x08 重入 / UHF / MOTOR / AM 控制类回 BUSY, 再 CANCEL 打断 ->
安全回降后 0x08 回帧 err=OK endReason=6 (ABORTED), 事后状态恢复.

用法: python3 abort.py
"""
import sys
import time

sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink           # noqa: E402
from zlr import const                     # noqa: E402

FC = const.FC_LOCKER_CTRL
SUB = 0x08
PROG = 0x09
PH_NAME = {0: "无流程", 1: "前置检查", 2: "UHF就绪", 3: "盘点", 4: "升起", 5: "保持期", 6: "回降"}
ERR_NAME = {0: "OK", 1: "BUSY", 2: "PARAM", 3: "UHF_OPEN", 4: "UHF_LINK",
            5: "NO_TAG", 6: "MISMATCH", 7: "HOMING", 8: "MOTOR_FAULT",
            9: "MOTOR_TIMEOUT", 10: "AM_LINK"}
END_NAME = {1: "TAG_REMOVED", 2: "HOLD_TIMEOUT", 3: "TAG_CHANGED", 4: "UHF_LOST",
            5: "DEMAG_DONE", 6: "ABORTED"}

results = []


def record(name, ok, msg):
    results.append((name, ok, msg))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")


def tx(link, fc, data, t=2.0):
    resp, to = link.transaction(fc, bytes(data), timeout_s=t)
    return resp, to


def discover_epc(link, tries=3):
    """单轮 1s 盘点偶发漏读在场标签 (模块 0x22 时序), 连试多轮取首轮命中。"""
    for _ in range(tries):
        link.transaction(const.FC_UHF_CTRL, bytes([const.UHF_OPEN]), timeout_s=4)
        ri, _ = tx(link, const.FC_UHF_CTRL, [const.UHF_INVENTORY, 0xE8, 0x03], t=6)
        if ri and ri["data"][1] == 0:
            cnt = ri["data"][2] | (ri["data"][3] << 8)
            if cnt and len(ri["data"]) >= 6:
                elen = ri["data"][5]
                return bytes(ri["data"][6:6 + elen])
    return None


def poll_progress(link, t=2.0):
    """拉一次 GET_PROGRESS: 回 [0x09, err, phase, holdL, holdH, stepsL, stepsH,
    tagPresent, demagDone, epcLen, epc..]; err!=0 时仅 [0x09, err]."""
    resp, to = tx(link, FC, [PROG], t)
    if to or not resp:
        return None
    d = resp["data"]
    if d[0] != PROG or d[1] != 0:
        return {"err": d[1] if len(d) > 1 else -1}
    return {
        "phase": d[2],
        "holdMs": d[3] | (d[4] << 8),
        "steps": d[5] | (d[6] << 8),
        "tagPresent": d[7],
        "demagDone": d[8],
        "epcLen": d[9],
        "epc": bytes(d[10:10 + d[9]]).hex() if len(d) > 10 and d[9] else "",
    }


def wait_phase(link, want, tmo_s=25.0):
    """轮询进度直到 phase==want, 打印沿途 phase 变化. 返回 (进入时刻快照|None)."""
    t0 = time.time()
    last = None
    while time.time() - t0 < tmo_s:
        p = poll_progress(link)
        if p is None:
            print("    (进度无响应, 重试)")
            continue
        if p.get("err") is not None:
            print(f"    (进度回 err={p['err']})")
            time.sleep(0.3)
            continue
        if p["phase"] != last:
            print(f"    phase {last}->{p['phase']}({PH_NAME.get(p['phase'])}) "
                  f"steps={p['steps']} @{time.time()-t0:.1f}s")
            last = p["phase"]
        if p["phase"] == want:
            return p
        time.sleep(0.3)
    return None


def main():
    link = HidLink()
    link.open()
    print("== 0x08 流程中交互闭环测试 (进度拉取 + BUSY 互斥 + CANCEL 打断) ==")

    tx(link, FC, [const.LOCKER_CANCEL])   # 干净起点
    epc = discover_epc(link)
    print(f"感应区 EPC: {epc.hex() if epc else '无 (需真实标签在场)'}")
    if not epc:
        record("前置", False, "无标签, 无法进入保持期")
        return summary()

    # ---- A1: demagCnt=3, AM 消磁器当前接在 Mac FTDI (设备侧 RS485 空置) ----
    # 预期: 前置 AM 探链失败 -> 快速回 err=10 AM_LINK
    print("\n-- A1 demagCnt=3 (AM 消磁器不在设备侧, 预期 AM_LINK 快速回) --")
    t0 = time.time()
    resp, to = tx(link, FC, [SUB, 0xE8, 0x03, 0x10, 0x00, len(epc)] + list(epc) + [3], t=30)
    dt = time.time() - t0
    if to:
        # AM 链路意外可达: 流程会真的等待 3 个消磁事件 (不会发生) 直到保持窗 16s 超时
        record("A1 demagCnt=3 无消磁事件", False, "无响应(超时) — AM 意外可达?")
    else:
        d = resp["data"]
        record("A1 demagCnt=3 无消磁事件", d[1] == 10,
              f"err={d[1]}({ERR_NAME.get(d[1])}) 耗时{dt:.2f}s "
              f"{'预期快速 AM_LINK' if d[1]==10 else 'AM 意外可达, 消磁等待已实际执行'}")

    # ---- B1: demagCnt=0 + 60s 保持窗, 流程中拉进度 + 互斥 + CANCEL 打断 ----
    print("\n-- B1 demagCnt=0 hold=60s: 流程中拉进度 + BUSY 互斥 + CANCEL --")
    req = bytes([SUB, 0xE8, 0x03, 0x60, 0xEA]) + bytes([len(epc)]) + epc
    link.send_frame(const.DEV_ADDR, FC, req)      # 发出后不等待 — 0x08 阻塞执行中

    # 升起段沿途应能看到 phase 4 -> 5
    ph = wait_phase(link, 5, tmo_s=25.0)
    if ph is None:
        record("B1 进入保持期", False, "25s 内未观察到 phase=5")
    else:
        record("B1 进入保持期", True,
               f"phase=5 holdMs={ph['holdMs']} tagPresent={ph['tagPresent']} "
               f"demagDone={ph['demagDone']} EPC回显={ph['epc']}")
        # 保持期字段: tagPresent=1 / demagDone=0 / holdMs 递增
        time.sleep(1.0)
        p2 = poll_progress(link)
        record("B1 保持期进度", p2 is not None and p2["phase"] == 5
               and p2["tagPresent"] == 1 and p2["demagDone"] == 0
               and p2["holdMs"] > ph["holdMs"],
               f"holdMs {ph['holdMs']}->{p2['holdMs'] if p2 else '?'} "
               f"tagPresent={p2['tagPresent'] if p2 else '?'}")

    # ---- B2: 流程中其他命令互斥 (0x08 重入 / UHF / MOTOR / AM 控制类 -> BUSY) ----
    print("\n-- B2 流程中命令互斥 --")
    r, _ = tx(link, FC, [SUB, 0xE8, 0x03, 0x10, 0x00, len(epc)] + list(epc) + [0], t=3)
    record("B2a 0x08 重入", r is not None and r["data"][1] == 1,
           f"err={r['data'][1] if r else '无响应'}(期望1 BUSY)")
    r, _ = tx(link, const.FC_UHF_CTRL, [const.UHF_CLOSE], t=3)
    record("B2b UHF 控制类", r is not None and r["data"][1] == 2,
           f"err={r['data'][1] if r else '无响应'}(期望2 UHF_ERR_BUSY)")
    r, _ = tx(link, const.FC_MOTOR_CTRL, [const.MOTOR_MOVE, 0, 10, 0, 0], t=3)
    record("B2c MOTOR 控制类", r is not None and r["data"][1] == 3,
           f"err={r['data'][1] if r else '无响应'}(期望3 MOTOR_ERR_BUSY)")
    r, _ = tx(link, const.FC_AM_CTRL, [5], t=3)   # AM_SUB_QUERY
    record("B2d AM 控制类", r is not None and r["data"][1] == 2,
           f"err={r['data'][1] if r else '无响应'}(期望2 AM_ERR_BUSY)")

    # ---- B3: CANCEL 打断 ----
    print("\n-- B3 CANCEL 打断 --")
    ph1 = poll_progress(link)
    t0 = time.time()
    r, to = tx(link, FC, [const.LOCKER_CANCEL], t=3)
    dt = time.time() - t0
    record("B3a CANCEL 立即回 OK", r is not None and r["data"][0] == 4 and r["data"][1] == 0,
           f"回帧=[{r['data'][0]},{r['data'][1]}] 耗时{dt*1000:.0f}ms")

    # 0x08 挂起回帧: 安全回降后 err=OK endReason=6 ABORTED
    fin = link.recv_frame(timeout_s=15.0)
    if fin is None or fin["func"] != (FC ^ 0xFF) or fin["data"][0] != SUB:
        record("B3b 0x08 回帧", False, f"未收到 0x08 回帧: {fin}")
    else:
        d = fin["data"]
        if d[1] == 0:
            endr = d[2]
            elen = d[3]
            rise = d[4 + elen] | (d[5 + elen] << 8)
            lower = d[6 + elen] | (d[7 + elen] << 8)
            demag = d[8 + elen] if len(d) > 8 + elen else 0
            record("B3b 0x08 回帧 endReason=6 ABORTED", endr == 6,
                   f"err=OK end={END_NAME.get(endr)} rise={rise} lower={lower} demag={demag}")
        else:
            extra = " ".join(f"{b:02X}" for b in d[2:])
            record("B3b 0x08 回帧 endReason=6 ABORTED", False,
                   f"err={d[1]}({ERR_NAME.get(d[1])}) diag=[{extra}]")

    # ---- B4: 事后恢复 ----
    print("\n-- B4 事后恢复 --")
    time.sleep(0.3)
    r, _ = tx(link, FC, [const.LOCKER_QUERY], t=3)
    st = r["data"][2] if r and len(r["data"]) > 2 else -1
    record("B4a Locker 恢复 IDLE", st == 0, f"lockerState={st}(期望0)")
    r, _ = tx(link, const.FC_MOTOR_CTRL, [const.MOTOR_QUERY], t=3)
    mst = r["data"][2] if r and len(r["data"]) > 2 else -1
    record("B4b 电机恢复 IDLE", mst == 0, f"stepperState={mst}(期望0)")
    p = poll_progress(link)
    record("B4c 进度恢复 phase=0", p is not None and p.get("phase") == 0,
           f"phase={p.get('phase') if p else '无响应'}(期望0)")

    return summary()


def summary():
    print("\n== 结果汇总 ==")
    npass = sum(1 for _, ok, _ in results if ok)
    for name, ok, msg in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}: {msg}")
    print(f"== {npass}/{len(results)} PASS ==")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
