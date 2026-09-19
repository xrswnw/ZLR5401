#!/usr/bin/env python3
"""am_flow_monitor — 0x11 AM 解锁流程 + 解码器实时监控 (区分标签问题/设备问题)

用法: python3 am_flow_monitor.py [amCnt=5] [tmo=180]
依据: 流程态内 AM 通道仅放行 GET_STATUS (0x22/0x06) — 只读本地计数器,
  不触总线不冲 cmd17; 配 GET_PROGRESS (0x23/0x09) 拉相位/计数。
读数:
  evt (cmd17 帧累计) = 消磁攻击量 — 放一张活标应立刻见帧
  deactCnt (结算成功累计) = 突发静默 3s 结算, 每突发 +1
  failCnt 恒 0 (判据证伪后无来源)
判定表:
  放标签后零帧              -> 标签未被检测 (已消死/非 AM 标/位置不对) — 标签问题
  帧持续增加但 deactCnt 不动 -> 该张消磁失败重试 (连发永不结算) — 标签问题
  deactCnt +1 但无 0x0E/计数不动 -> 设备问题
  正常: 突发几帧 -> 3s 静默 -> deactCnt+1 -> 0x0E 推送 -> 计数+1
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER, FC_AM = 0x23, 0x22
PH = {0: "无", 3: "升起", 4: "回降", 5: "计数窗", 7: "保持"}
ENDR = {1: "ALL_OK", 3: "TIMEOUT(窗满未达标)", 6: "ABORTED"}
MODE = {0: "检测消磁", 1: "仅检测", 2: "待机"}


def am_mode(lnk):
    """探链 + GET_CONFIG -> mode (data[14])。"""
    lnk.send_frame(1, FC_AM, bytes([0x05]))
    dl = time.time() + 2.0
    while time.time() < dl:
        f = lnk.recv_frame(0.1)
        if f and f["func"] == (FC_AM ^ 0xFF) and f["data"] and f["data"][0] == 0x05:
            break
    lnk.send_frame(1, FC_AM, bytes([0x01]))
    dl = time.time() + 2.0
    while time.time() < dl:
        f = lnk.recv_frame(0.1)
        if f and f["func"] == (FC_AM ^ 0xFF) and f["data"] \
                and f["data"][0] == 0x01 and len(f["data"]) >= 15 \
                and f["data"][1] == 0:
            return f["data"][14]
    return None


def main():
    am_cnt = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    tmo = int(sys.argv[2]) if len(sys.argv) > 2 else 180
    lnk = HidLink(); lnk.open()

    m0 = am_mode(lnk)
    print(f"流程前 AM mode={m0}({MODE.get(m0, '?')})")
    print(f"下发 0x11 amCnt={am_cnt} tmo={tmo}s — 依次放标签, 监控帧/结算/推送...")

    lnk.send_frame(1, FC_LOCKER,
                   bytes([0x11, am_cnt, tmo & 0xFF, (tmo >> 8) & 0xFF, 0xA5, 0x5A]))
    t0 = time.time()
    st = {"evt": None, "deact": None, "fail": None, "phase": None, "conf": None}
    stats = {"push0e": 0}
    term = [None]

    def pump(want_fc, want_sub, wait):
        """收帧至匹配响应/超时; 顺带记推送, 截获 0x11 终帧。返回 (resp, stop)。"""
        dl = time.time() + wait
        while time.time() < dl:
            f = lnk.recv_frame(0.05)
            if not f or not f["data"]:
                continue
            d, fn = f["data"], f["func"]
            at = time.time() - t0
            if fn == (FC_LOCKER ^ 0xFF) and d[0] in (0x0F, 0x0E):
                if d[0] == 0x0F:
                    print(f"  [{at:6.1f}s] 0x0F 受理: phase={d[2]} "
                          f"winMs={int.from_bytes(d[3:6], 'little')}")
                else:
                    stats["push0e"] += 1
                    print(f"  [{at:6.1f}s] 0x0E 推送: done={d[1]}/{d[2]}")
            elif fn == (FC_LOCKER ^ 0xFF) and d[0] == 0x11:
                term[0] = d
                return None, True
            elif fn == (want_fc ^ 0xFF) and d[0] == want_sub:
                return d, False
        return None, False

    while term[0] is None and time.time() - t0 < tmo + 40:
        # 解码器监控 (流程态内唯一放行的 AM 命令)
        lnk.send_frame(1, FC_AM, bytes([0x06]))
        d, stop = pump(FC_AM, 0x06, 0.4)
        if stop:
            break
        if d and len(d) >= 19:
            at = time.time() - t0
            evt = int.from_bytes(d[2:6], "little")
            le_ms = int.from_bytes(d[6:10], "little")
            dc = int.from_bytes(d[11:15], "little")
            fc_ = int.from_bytes(d[15:19], "little")
            if st["evt"] is None:
                st.update(evt=evt, evt0=evt, deact=dc, deact0=dc, fail=fc_)
                print(f"  [{at:6.1f}s] 基线: evt={evt} deactCnt={dc} failCnt={fc_}")
            else:
                if evt != st["evt"]:
                    print(f"  [{at:6.1f}s] cmd17 +{evt - st['evt']} 帧 "
                          f"(累计 {evt}, 帧时刻=上电后 {le_ms}ms)")
                    st["evt"] = evt
                if dc != st["deact"]:
                    print(f"  [{at:6.1f}s] ★ 结算 +{dc - st['deact']} -> deactCnt={dc} "
                          f"(突发静默3s=一次成功)")
                    st["deact"] = dc
                if fc_ != st["fail"]:
                    print(f"  [{at:6.1f}s] !! failCnt +{fc_ - st['fail']} (新判据下应恒 0)")
                    st["fail"] = fc_
        # 流程进度
        lnk.send_frame(1, FC_LOCKER, bytes([0x09]))
        d, stop = pump(FC_LOCKER, 0x09, 0.4)
        if stop:
            break
        if d and len(d) >= 12 and d[1] == 0:
            at = time.time() - t0
            ph, conf = d[2], d[7]
            if ph != st["phase"]:
                print(f"  [{at:6.1f}s] phase {st['phase']}->{ph}({PH.get(ph, '?')}) "
                      f"holdMs={int.from_bytes(d[3:6], 'little')}")
                st["phase"] = ph
            if conf != st["conf"]:
                print(f"  [{at:6.1f}s] 计数 {st['conf']}->{conf}")
                st["conf"] = conf

    d = term[0]
    if d is None:
        print(f"FAIL: {tmo + 40:.0f}s 内未见终帧 (流程疑似滞留)")
        lnk.close()
        return 1
    if d[1] != 0 or len(d) < 14:
        print(f"终帧 err={d[1]} data={d.hex()}")
        lnk.close()
        return 1
    end = d[2]
    rise = int.from_bytes(d[3:5], "little")
    lower = int.from_bytes(d[5:7], "little")
    el = int.from_bytes(d[7:9], "little")
    print(f"终帧: endReason={end}({ENDR.get(end, '?')}) rise={rise} lower={lower} "
          f"elapsed={el}s amCnt={d[9]} deactDone={d[10]} deactFail={d[11]} "
          f"rsv={d[12]:02X},{d[13]:02X}")
    d_evt = (st["evt"] - st["evt0"]) if st["evt"] is not None else None
    d_deact = (st["deact"] - st["deact0"]) if st["deact"] is not None else None
    m1 = am_mode(lnk)
    print(f"流程后 AM mode={m1}({MODE.get(m1, '?')})  (零控制: 期望与流程前一致)")
    print(f"汇总: cmd17 帧 +{d_evt} | 结算成功 +{d_deact} | 0x0E 推送 {stats['push0e']} | "
          f"终帧 deactDone={d[10]} (四者应一致=结算数; 帧数≥结算数×1)")
    lnk.close()
    return 0 if (end == 1 and d[10] == am_cnt and m1 == 1) else 1


if __name__ == "__main__":
    sys.exit(main())
