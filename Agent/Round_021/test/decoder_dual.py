"""P1: Round_021 解码器双模 — 类型切换语义 + AM 零回归 + RF 命令面.

前置: 真机 USB HID 在线。AM 消磁解码器接 RS232 (现网形态);
RF 样机到位后接同一 RS232 口再跑 RF 段 (脚本按 SET_DECODER 的
link 位自动判定 RF 在场, 缺席段 SKIP)。

覆盖:
  D1-D5   AM 模式基线 + 零回归快检 (新码位不破坏旧命令面)
  D6-D8   切换语义: 非法值/幂等/切换往返 + 持久化 (软复位后保持)
  D9      RF 模式命令面 (探链/透传/AM 专属码位回 PARAM)
  D10     busy 门禁 (0x11 流程中 GET_DECODER 放行 / SET_DECODER BUSY)
"""
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_098/test")

from lib import open_link, tx, txm, reopen_until_alive, load_hw  # noqa: E402
from zlr import const as C                                        # noqa: E402

# ---- Round_021 新常量 (App_CustomProtocol.h) ----
AM_GET_DECODER = 0x0A
AM_SET_DECODER = 0x0B
AM_GET_CONFIG = 0x01
AM_SET_CONFIG = 0x02
AM_GET_PARAM = 0x03
AM_SET_PARAM = 0x04
AM_QUERY = 0x05
AM_GET_STATUS = 0x06
AM_SET_MODE = 0x07
AM_GET_WAVE = 0x08
AM_GET_WAVE_PAGE = 0x09
AM_ERR_OK, AM_ERR_PARAM, AM_ERR_BUSY, AM_ERR_LINK = 0, 1, 2, 4
DEC_AM, DEC_RF = 0, 1
LOCKER_SUB_CANCEL = 0x04
LOCKER_SUB_UNLOCK_AM = 0x11      # 0x11 [cmd, amCnt, tmoL, tmoH, rsv0, rsv1]

RESULT_JSONL = os.path.join(_HERE, "results.jsonl")


class Rec:
    def __init__(self):
        self.n = {"PASS": 0, "FAIL": 0, "SKIP": 0, "OBS": 0}

    def _w(self, cid, name, v, msg=""):
        with open(RESULT_JSONL, "a", encoding="utf-8") as f:
            f.write(json.dumps({"phase": "P1", "id": cid, "name": name,
                                "verdict": v, "msg": str(msg)},
                               ensure_ascii=False) + "\n")
        tag = {"PASS": "✅", "FAIL": "❌", "SKIP": "⏭", "OBS": "👁"}.get(v, "?")
        print(f"  [{tag} {v}] {cid} {name}: {msg}", flush=True)
        self.n[v] = self.n.get(v, 0) + 1

    def check(self, cid, name, cond, okmsg="", failmsg=""):
        self._w(cid, name, "PASS" if cond else "FAIL",
                okmsg if cond else failmsg)
        return cond

    def skip(self, cid, name, msg=""):
        self._w(cid, name, "SKIP", msg)

    def obs(self, cid, name, msg=""):
        self._w(cid, name, "OBS", msg)


rec = Rec()
lk = open_link()
hw = load_hw()
am_present = hw.get("am_link", False)


def am(sub, data=None, t=8):
    d, to = txm(lk, C.FC_AM_CTRL, [sub] + (data or []), timeout_s=t)
    return d


def cur_decoder():
    d = am(AM_GET_DECODER)
    if d is not None and len(d) >= 3 and d[1] == AM_ERR_OK:
        return d[2], d
    return None, d


# ---- D1 基线: 上电类型 (现网 AM 形态期望 0; 若上次跑完停在 RF 则也接受,
#      由 D8 复位持久化用例闭环) ----
t0, d = cur_decoder()
rec.check("D1", "GET_DECODER 基线", t0 is not None,
          f"type={t0} resp={d.hex() if d else 'TO'}",
          f"resp={d.hex() if d else 'TO'}")

# ---- D2 AM 零回归快检 (仅 AM 在场时; 当前应为 AM 模式) ----
if t0 == DEC_AM and am_present:
    d = am(AM_QUERY, t=8)
    rec.check("D2a", "AM 模式 QUERY (0x63 探链)", d is not None and d[1] == AM_ERR_OK,
              f"err=0 link={d[2] if d else '?'}", f"resp={d.hex() if d else 'TO'}")
    d = am(AM_GET_CONFIG)
    ok = d is not None and d[1] == AM_ERR_OK and len(d) >= 16
    rec.check("D2b", "AM 模式 GET_CONFIG (16B 布局不变)", ok,
              f"resp_len={len(d) if d else 0}", f"resp={d.hex() if d else 'TO'}")
    d = am(AM_GET_STATUS)
    ok = d is not None and len(d) >= 17
    rec.check("D2c", "AM 模式 GET_STATUS (17B 布局不变)", ok,
              f"evt={int.from_bytes(d[3:7],'little') if ok else '?'}",
              f"resp={d.hex() if d else 'TO'}")
    d = am(AM_GET_PARAM, [0x11], t=8)   # amCmd=0x11 接收阀值 (AM 命令字)
    rec.check("D2d", "AM 模式 GET_PARAM(0x11)", d is not None and d[1] == AM_ERR_OK,
              f"val={int.from_bytes(d[3:5],'big') if d and len(d)>=5 else '?'}",
              f"resp={d.hex() if d else 'TO'}")
elif t0 != DEC_AM:
    rec.obs("D2", "AM 回归快检", "当前非 AM 模式, 由 D6 切回后 D10 复检")
else:
    rec.skip("D2", "AM 回归快检", "AM 硬件缺席 (hw am_link=0)")

# ---- D3 非法类型值 ----
for bad in (2, 0xFF):
    d = am(AM_SET_DECODER, [bad], t=4)
    rec.check(f"D3-{bad:#04x}", f"SET_DECODER 非法值 {bad:#04x} -> PARAM",
              d is not None and d[1] == AM_ERR_PARAM,
              f"err=1", f"resp={d.hex() if d else 'TO'}")

# ---- D4 幂等: 同类型直回 (无总线重建抖动, err=0) ----
if t0 is not None:
    d = am(AM_SET_DECODER, [t0], t=8)
    rec.check("D4", "SET_DECODER 幂等 (同类型 OK)", d is not None and d[1] == AM_ERR_OK,
              f"err=0 type={d[2] if d else '?'}",
              f"resp={d.hex() if d else 'TO'}")

# ---- D5 AM 专属码位在 AM 模式正常 (对照 D9 RF 模式回 PARAM) ----
if t0 == DEC_AM and am_present:
    d = am(AM_GET_WAVE_PAGE, [0], t=8)
    rec.check("D5", "AM 模式 GET_WAVE_PAGE 正常", d is not None and len(d) >= 5,
              f"resp_len={len(d) if d else 0}", f"resp={d.hex() if d else 'TO'}")

# ---- D6 切换 AM -> RF (重建后端 + 联动探链) ----
d = am(AM_SET_DECODER, [DEC_RF], t=8)
sw_ok = d is not None and d[1] == AM_ERR_OK and d[2] == DEC_RF
rf_link = d[3] if d is not None and len(d) >= 4 else None
rec.check("D6a", "SET_DECODER 0->1 (回 type+link)", sw_ok,
          f"type=1 link={rf_link} (0=RF 在场, 非0=缺席如实上报)",
          f"resp={d.hex() if d else 'TO'}")
t1, d = cur_decoder()
rec.check("D6b", "切换后 GET_DECODER=1", t1 == DEC_RF,
          f"type={t1}", f"resp={d.hex() if d else 'TO'}")

# ---- D7/D9 RF 模式命令面 ----
if t1 == DEC_RF:
    d = am(AM_QUERY, t=6)
    rec.check("D9a", "RF 模式 QUERY (0x01 探链)",
              d is not None and d[1] == AM_ERR_OK,
              f"link={d[2] if d else '?'} (与 D6a link 一致性供对读)",
              f"resp={d.hex() if d else 'TO'}")
    # AM 专属码位 -> PARAM (RF 模式码位显式拒绝, 不自适应)
    for sub, nm in ((AM_GET_CONFIG, "GET_CONFIG"), (AM_SET_CONFIG, "SET_CONFIG"),
                    (AM_GET_WAVE, "GET_WAVE"), (AM_GET_WAVE_PAGE, "GET_WAVE_PAGE")):
        d = am(sub, [0] if sub in (AM_SET_CONFIG, AM_GET_WAVE_PAGE) else None, t=4)
        rec.check(f"D9b-{nm}", f"RF 模式 {nm} -> PARAM",
                  d is not None and d[1] == AM_ERR_PARAM,
                  f"err=1", f"resp={d.hex() if d else 'TO'}")
    if rf_link == 0:
        # RF 在场: 透传读写 + 逻辑模式
        d = am(AM_GET_PARAM, [0x06], t=6)   # RF 命令字 0x06 = 状态 (0x01 查询抽取)
        rec.check("D9c", "RF GET_PARAM(0x06 状态)", d is not None and d[1] == AM_ERR_OK,
                  f"val={d[4] if d and len(d) >= 5 else '?'} (0检测解码/1只检测/2休眠)",
                  f"resp={d.hex() if d else 'TO'}")
        d = am(AM_SET_MODE, [1], t=6)        # RF 0x06 写只检测 (回显确认)
        rec.check("D9d", "RF SET_MODE(1 只检测)", d is not None and d[1] == AM_ERR_OK,
                  f"err=0", f"resp={d.hex() if d else 'TO'}")
        d = am(AM_SET_PARAM, [0x1A, 0x00], t=6)   # 0x1A 测试报警 (无标签可触发 0x3A)
        rec.check("D9e", "RF SET_PARAM(0x1A 测试报警)", d is not None and d[1] == AM_ERR_OK,
                  f"err=0 (随后 GET_STATUS evt 增量为观测项)",
                  f"resp={d.hex() if d else 'TO'}")
        d = am(AM_GET_STATUS, t=6)
        rec.obs("D9f", "RF GET_STATUS (0x3A 计数观测)",
                f"evt={int.from_bytes(d[3:7],'little') if d and len(d)>=7 else '?'} "
                f"deactCnt={int.from_bytes(d[11:15],'little') if d and len(d)>=15 else '?'}")
    else:
        rec.skip("D9c-e", "RF 透传读写", "RF 设备缺席 (D6a link!=0), 样机到位后复跑")
        d = am(AM_GET_PARAM, [0x06], t=6)
        rec.check("D9c-absent", "RF 缺席 GET_PARAM(0x06) -> LINK",
                  d is not None and d[1] == AM_ERR_LINK,
                  f"err=4 (0x01 查询无回帧)", f"resp={d.hex() if d else 'TO'}")

    # RF 模式下发 AM 命令字 (0x50 RF 表外): 现行口径 err 落空 OK+值0
    # (与 AM 模式非法命令字同口径, Round_098 p5_am AM4a 固化)
    d = am(AM_GET_PARAM, [0x50], t=6)
    rec.obs("D9g", "RF 模式 GET_PARAM(AM 0x50 表外)",
            f"resp={d.hex() if d else 'TO'} (口径: err 落空 OK val=0, 双表文档约束)")

# ---- D10 busy 门禁: 0x11 流程中 GET_DECODER 放行 / SET_DECODER BUSY ----
# 0x11 受理即 SET_MODE(检测解码): AM 模式跑此段需 AM 在场;
# tmo=10s 长窗内打两针, 随后 CANCEL 复原 (出口自动切回只检测)。
mode_for_busy = t1 if t1 is not None else DEC_AM
if (mode_for_busy == DEC_AM and am_present) or (mode_for_busy == DEC_RF and rf_link == 0):
    d, to = txm(lk, C.FC_LOCKER_CTRL, [LOCKER_SUB_UNLOCK_AM, 1, 10, 0, 0, 0], timeout_s=3)
    started = d is not None and d[1] == 0
    if started:
        time.sleep(0.5)   # 受理帧/0x0E 推送窗口
        t2, dg = cur_decoder()
        rec.check("D10a", "0x11 流程中 GET_DECODER 放行", t2 is not None,
                  f"type={t2}", f"resp={dg.hex() if dg else 'TO'}")
        d = am(AM_SET_DECODER, [DEC_AM if t2 == DEC_RF else DEC_RF], t=4)
        rec.check("D10b", "0x11 流程中 SET_DECODER -> BUSY",
                  d is not None and d[1] == AM_ERR_BUSY,
                  f"err=2", f"resp={d.hex() if d else 'TO'}")
        dc, _ = txm(lk, C.FC_LOCKER_CTRL, [LOCKER_SUB_CANCEL], timeout_s=6)
        rec.check("D10c", "CANCEL 流程", dc is not None and dc[1] == 0,
                  f"err=0 (ABORTED 终帧随后异步到达)", f"resp={dc.hex() if dc else 'TO'}")
        time.sleep(1.0)
    else:
        rec.skip("D10", "busy 门禁", f"0x11 受理失败: {d.hex() if d else 'TO'}")
else:
    rec.skip("D10", "busy 门禁", "对应解码器硬件缺席")

# ---- D11 切回 AM + 复检 ----
d = am(AM_SET_DECODER, [DEC_AM], t=8)
rec.check("D11a", "SET_DECODER 1->0 切回 AM",
          d is not None and d[1] == AM_ERR_OK and d[2] == DEC_AM,
          f"type=0 link={d[3] if d is not None and len(d) >= 4 else '?'}",
          f"resp={d.hex() if d else 'TO'}")
if am_present:
    d = am(AM_QUERY, t=8)
    rec.check("D11b", "切回后 AM QUERY 复检", d is not None and d[1] == AM_ERR_OK,
              f"err=0 (回归红线: AM 链路无损)", f"resp={d.hex() if d else 'TO'}")
    d = am(AM_GET_CONFIG)
    rec.check("D11c", "切回后 GET_CONFIG 复检", d is not None and d[1] == AM_ERR_OK,
              f"err=0", f"resp={d.hex() if d else 'TO'}")

# ---- D8 持久化: 软复位后类型保持 (终态=AM, 与现网一致) ----
d, to = tx(lk, C.FC_RESET, [], timeout_s=3)
time.sleep(1.0)
lk = reopen_until_alive(timeout_s=20)
t3, d = cur_decoder()
rec.check("D8", "软复位后 GET_DECODER 持久化 (期望 0=AM 终态)",
          t3 == DEC_AM, f"type={t3}", f"resp={d.hex() if d else 'TO'}")

print(f"\n== P1 汇总: PASS={rec.n['PASS']} FAIL={rec.n['FAIL']} "
      f"SKIP={rec.n['SKIP']} OBS={rec.n['OBS']}", flush=True)
sys.exit(0 if rec.n["FAIL"] == 0 else 1)
