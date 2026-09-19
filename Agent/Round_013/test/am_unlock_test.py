"""0x11 AM 标签解锁流程实测 (Round_013 新通道) — 纯计数窗, 全程不动电机。

2026-09-19 两裁决后的语义:
  1. 达标即结账 — 蜂鸣 1s + 绿三闪, rise/lower 恒 0 (无升起/保持/回降)。
  2. AM 解码器零控制 — 不探链/不切模式/不下发参数 (上电同样零下发),
     流程仅被动收 cmd17 计数; 解码器静默 (含挂起/自锁) 按窗满结账。
前置: 解码器处于自身默认/自主状态 (自然含检测消磁能力, 放标签即消)。
帧: 0x11 [cmd, amCnt, tmoL, tmoH, rsv0, rsv1]  (tmo: u16 秒)。
  受理 -> 计数窗 (每结算突发一计, 帧数不限 — 2026-09-19 判据修订;
  每成功一张推 0x0E + 绿闪; 识别蜂鸣 2026-09-19 裁决取消) -> 达标 (>=amCnt)
  即结账 -> 终帧 [0x11,0,endReason,rise(2,恒0),lower(2,恒0),
  elapsed(2 秒),amCnt,deactDone,deactFail(恒0),rsv0,rsv1]。
  amCnt=0 支路: 无计数门 0>=0 首查即达标, 不耗窗直接结账 ALL_OK。
  endReason: 1 ALL_OK / 3 TIMEOUT (窗满未达标) / 6 ABORTED。
零控制验证: 流程前后 0x22 GET_CONFIG 读 mode 应不变 (固件不再触碰)。
GET_PROGRESS (0x09): phase 5=计数窗/8=结束 (无 3/7/4 机械段),
  total/softCnt=amCnt, confirmed/softDone=已解锁数。
用法: python3 am_unlock_test.py <case> [flow 的 amCnt] [flow 的 tmo]
  case: param   — 入口校验 (tmo=0/amCnt=0且tmo=0/短帧 -> err=2;
                  amCnt=0+tmo>0 合法走 am0 支路)
        timeout — 无标签超时 (amCnt=1 tmo=8s -> endReason=3, 步数=0)
        cancel  — 窗内 CANCEL (amCnt=1 tmo=60s, 2s 后 0x04 -> endReason=6)
        flow    — 达标闭环 (默认 amCnt=3 tmo=120s, AM 标签已放消磁区,
                  解码器自行消磁 -> ALL_OK; argv2/argv3 改数/超时)
        am0     — amCnt=0 直接完成支路 (不耗窗即时 ALL_OK, 无需标签)
        mode    — 解码器模式读取 (信息性: 零控制后 mode 为解码器自主,
                  不再断言取值, 仅验证链路可读)
"""
import sys, time
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")
from zlr.hid_link import HidLink

FC_LOCKER, FC_AM = 0x23, 0x22
PH = {0: "无", 3: "升起", 4: "回降", 5: "计数窗", 7: "保持"}
ENDR = {1: "ALL_OK", 3: "TIMEOUT(窗满未达标)", 6: "ABORTED"}
MODE = {0: "检测消磁", 1: "仅检测", 2: "待机"}
pushes = []


def tx(link, fc, data, t=3.0, echo=None):
    """严格配对 + 顺带收集 0x23 过程推送帧 (0x0F 受理 / 0x0E 解锁)。"""
    for _ in range(16):
        f = link.recv_frame(0.05)
        if f and f["func"] == (FC_LOCKER ^ 0xFF) and len(f["data"]) >= 1 \
                and f["data"][0] in (0x0F, 0x0E):
            pushes.append(f["data"])
        if f is None:
            break
    link.send_frame(1, fc, bytes(data))
    deadline = time.time() + t
    while time.time() < deadline:
        f = link.recv_frame(max(0.05, min(0.5, deadline - time.time())))
        if f and f["func"] == (FC_LOCKER ^ 0xFF) and len(f["data"]) >= 1 \
                and f["data"][0] in (0x0F, 0x0E):
            pushes.append(f["data"])
            continue
        if f and f["func"] == (fc ^ 0xFF):
            if echo is None or (len(f["data"]) > 0 and f["data"][0] == echo):
                return f["data"]
    return None


def am_mode(link):
    """0x22 GET_CONFIG -> mode (index 14); None=链路/设备失败。"""
    d = tx(link, FC_AM, [0x01], t=5.0, echo=0x01)
    if d and len(d) >= 15 and d[1] == 0:
        return d[14]
    return None


def progress(link):
    d = tx(link, FC_LOCKER, [0x09], t=2.0, echo=0x09)
    if d and len(d) >= 12 and d[1] == 0:
        return {"phase": d[2], "holdMs": int.from_bytes(d[3:6], "little"),
                "total": d[6], "confirmed": d[7], "bm": d[8],
                "softCnt": d[9], "softDone": d[10], "lastCycle": d[11]}
    return None


def unlock(link, am_cnt, tmo_sec, t, rsv0=0xA5, rsv1=0x5A):
    """下发 0x11 并等终帧; 期间顺带记录推送 (tx 内收集)。"""
    data = [0x11, am_cnt, tmo_sec & 0xFF, (tmo_sec >> 8) & 0xFF, rsv0, rsv1]
    return tx(link, FC_LOCKER, data, t=t, echo=0x11)


def show_terminal(d, rsv0=0xA5, rsv1=0x5A):
    if d is None:
        print("FAIL: 终帧未收到"); return 1
    err = d[1]
    print(f"终帧 err={err}", end="")
    if err == 0 and len(d) >= 14:
        end = d[2]
        rise = int.from_bytes(d[3:5], "little")
        lower = int.from_bytes(d[5:7], "little")
        el = int.from_bytes(d[7:9], "little")
        print(f" endReason={end}({ENDR.get(end, '?')}) rise={rise} lower={lower} "
              f"elapsed={el}s amCnt={d[9]} deactDone={d[10]} deactFail={d[11]} "
              f"rsv回显={d[12]:02X},{d[13]:02X} (期望 {rsv0:02X},{rsv1:02X})")
        ok = (d[12] == rsv0 and d[13] == rsv1)
        print("PASS" if ok else "FAIL: rsv 回显不符")
        return 0 if ok else 1
    print(f" data={[hex(x) for x in d]}")
    return 1


def case_param(link):
    """入口校验: tmo=0 / amCnt=0且tmo=0 / 短帧 一律 err=2 PARAM。
    (amCnt=0 + tmo>0 已合法 — 直接升起支路, 见 am0 用例。)"""
    rc = 0
    for name, data in (("tmo=0", [0x11, 1, 0, 0, 0, 0]),
                       ("amCnt=0且tmo=0", [0x11, 0, 0, 0, 0, 0]),
                       ("短帧", [0x11, 1, 8])):
        d = tx(link, FC_LOCKER, data, t=3.0, echo=0x11)
        got = d[1] if d else None
        ok = (d is not None and d[1] == 2)
        print(f"{name}: err={got} {'PASS' if ok else 'FAIL (期望 2)'}")
        rc |= 0 if ok else 1
    return rc


def case_timeout(link):
    """无标签 8s 超时: endReason=3, 磁块未动 (rise=lower=0), deactDone=0。"""
    print("下发 0x11 amCnt=1 tmo=8s (不放标签) ...")
    d = unlock(link, 1, 8, t=25.0)
    if d and d[1] == 0 and len(d) >= 14:
        ok = (d[2] == 3 and int.from_bytes(d[3:5], "little") == 0
              and int.from_bytes(d[5:7], "little") == 0 and d[10] == 0)
        show_terminal(d)
        print("PASS" if ok else "FAIL: 期望 endReason=3 步数0 deactDone=0")
        return 0 if ok else 1
    return show_terminal(d)


def case_cancel(link):
    """窗内 CANCEL: 立即 ABORTED (磁块未动)。
    顺序式: 收到 0x0F 受理推送即原路发 CANCEL, 同一接收循环收终帧
    (不做并发 — 两线程抢 HID 链路会互相吃帧)。"""
    print("下发 0x11 amCnt=1 tmo=60s, 受理后立即 CANCEL ...")
    link.send_frame(1, FC_LOCKER, bytes([0x11, 1, 60, 0, 0xA5, 0x5A]))
    deadline = time.time() + 30.0
    cancelled = False
    d = None
    while time.time() < deadline:
        f = link.recv_frame(0.2)
        if not f or f["func"] != (FC_LOCKER ^ 0xFF) or len(f["data"]) < 1:
            continue
        sub = f["data"][0]
        if sub == 0x0F and not cancelled:
            print(f"  推送 0x0F 受理: phase={f['data'][2]} "
                  f"winMs={int.from_bytes(f['data'][3:6], 'little')}")
            link.send_frame(1, FC_LOCKER, bytes([0x04]))
            cancelled = True
        elif sub == 0x04:
            print(f"  CANCEL 响应 err={f['data'][1] if len(f['data']) > 1 else '?'}")
        elif sub == 0x11:
            d = f["data"]
            break
    if d and d[1] == 0 and len(d) >= 14:
        ok = (d[2] == 6 and int.from_bytes(d[3:5], "little") == 0
              and int.from_bytes(d[5:7], "little") == 0)
        show_terminal(d)
        print("PASS" if ok else "FAIL: 期望 endReason=6 rise=0 lower=0")
        return 0 if ok else 1
    return show_terminal(d)


def case_flow(link, am_cnt=3, tmo_sec=120):
    """达标闭环: AM 标签已放消磁区, 解码器按自身状态自行消磁 (零控制)。
    期望: am_cnt 个 0x0E 推帧 + 终帧 endReason=1, deactDone>=am_cnt,
    rise=lower=0 (不动电机), 流程前后 mode 不变 (零控制验证)。"""
    m0 = am_mode(link)
    print(f"流程前 AM mode={m0}({MODE.get(m0, '?')})  (零控制: 仅记录, 不断言)")
    print(f"下发 0x11 amCnt={am_cnt} tmo={tmo_sec}s — 标签已在消磁区, 等消磁事件 ...")
    d = unlock(link, am_cnt, tmo_sec, t=tmo_sec + 30.0)
    rc = show_terminal(d)
    if d and d[1] == 0 and len(d) >= 14:
        ok = (d[2] == 1 and d[10] >= am_cnt
              and int.from_bytes(d[3:5], "little") == 0
              and int.from_bytes(d[5:7], "little") == 0)
        print("PASS" if ok else f"FAIL: 期望 endReason=1 deactDone>={am_cnt} 且 rise=lower=0")
        rc = 0 if ok else 1
    for p in pushes:
        if p[0] == 0x0F:
            print(f"  推送 0x0F 受理: phase={p[2]} winMs={int.from_bytes(p[3:6], 'little')}")
        elif p[0] == 0x0E:
            print(f"  推送 0x0E 解锁: done={p[1]} amCnt={p[2]}")
    m1 = am_mode(link)
    print(f"流程后 AM mode={m1}({MODE.get(m1, '?')})  期望与流程前一致 (零控制)")
    if m0 is not None and m1 is not None and m0 != m1:
        print(f"FAIL: mode 流程内被改变 {m0}->{m1} (零控制被破坏)"); rc |= 1
    return rc


def case_am0(link):
    """amCnt=0 直接完成支路: 0>=0 首查即达标, 不耗窗即时结账 ALL_OK。
    期望: endReason=1, rise=lower=0, elapsed<2s (即时), deactDone=0
    (无标签), 流程前后 mode 不变。无需 AM 标签。"""
    m0 = am_mode(link)
    print(f"流程前 AM mode={m0}({MODE.get(m0, '?')})  (零控制: 仅记录, 不断言)")
    print("下发 0x11 amCnt=0 tmo=5s (直接完成支路, 即时结账) ...")
    d = unlock(link, 0, 5, t=15.0)
    rc = show_terminal(d)
    if d and d[1] == 0 and len(d) >= 14:
        el = int.from_bytes(d[7:9], "little")
        ok = (d[2] == 1 and int.from_bytes(d[3:5], "little") == 0
              and int.from_bytes(d[5:7], "little") == 0 and el <= 2
              and d[10] == 0)
        print("PASS" if ok else "FAIL: 期望 endReason=1 rise=lower=0 elapsed<=2s deactDone=0")
        rc = 0 if ok else 1
    m1 = am_mode(link)
    print(f"流程后 AM mode={m1}({MODE.get(m1, '?')})  期望与流程前一致 (零控制)")
    if m0 is not None and m1 is not None and m0 != m1:
        print(f"FAIL: mode 流程内被改变 {m0}->{m1} (零控制被破坏)"); rc |= 1
    return rc


def case_mode(link):
    """解码器模式读取 (信息性): 2026-09-19 零控制裁决后 mode 为解码器
    自主状态 (上电零下发/流程不切), 固件不再归一切断 — 不断言取值,
    仅验证链路可读 (None=链路失败)。静置消磁 (mode 0) 属解码器本职,
    挂起/自锁问题另行观察, 不由 mode 断言。"""
    m = am_mode(link)
    if m is None:
        print("AM mode 读取失败 (链路/设备)  FAIL")
        return 1
    print(f"AM mode={m}({MODE.get(m, '?')})  信息性读取 PASS (零控制: 解码器自主)")
    return 0


CASES = {"param": case_param, "timeout": case_timeout, "cancel": case_cancel,
         "flow": case_flow, "am0": case_am0, "mode": case_mode}

if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "param"
    if name not in CASES:
        print(f"未知 case: {name}  可选: {', '.join(CASES)}"); sys.exit(2)
    lnk = HidLink(); lnk.open()
    if name in ("timeout", "cancel", "flow", "am0"):
        time.sleep(2)   # HID 枚举稳定等待 (0x11 不动电机, 无回零前置)
    if name == "flow":
        am_cnt = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        tmo_sec = int(sys.argv[3]) if len(sys.argv) > 3 else 120
        rc = case_flow(lnk, am_cnt, tmo_sec)
    else:
        rc = CASES[name](lnk)
    lnk.close()
    sys.exit(rc)
