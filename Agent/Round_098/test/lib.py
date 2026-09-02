"""Round_098 测试执行器公共库.

复用 Round_008/test/zlr 框架 (HidLink/frame/const), 扩展缺失常量,
提供统一用例记录器 (PASS/FAIL/SKIP/OBS -> JSONL).
"""
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "/Users/swnw/Documents/Software/ZLR5401/Agent/Round_008/test")

from zlr.hid_link import HidLink          # noqa: E402
from zlr import const as C                # noqa: E402
from zlr.frame import build_frame, parse_frame  # noqa: E402

# ---- 框架缺失的常量 (新功能, 见 App_CustomProtocol.h / App_LockerOneShot.h) ----
FC_SELFTEST_CTRL = 0x0F
SELFTEST_SUB_QUERY = 0x01
SELFTEST_SUB_RERUN = 0x02
SELFTEST_SUB_CLEAR = 0x03

# AM 子命令
AM_GET_CONFIG = 0x01
AM_SET_CONFIG = 0x02
AM_GET_PARAM = 0x03
AM_SET_PARAM = 0x04
AM_QUERY = 0x05
AM_GET_STATUS = 0x06
AM_SET_MODE = 0x07
AM_GET_WAVE = 0x08
AM_GET_WAVE_PAGE = 0x09
AM_ERR_OK = 0
AM_ERR_PARAM = 1
AM_ERR_BUSY = 2
AM_ERR_LINK = 4
AM_ERR_TIMEOUT = 6

# Locker OneShot
LOCKER_ONE_SHOT = 0x08
LOCKER_GET_PROGRESS = 0x09
ONE_ERR_OK = 0
ONE_ERR_BUSY = 1
ONE_ERR_PARAM = 2
ONE_ERR_UHF_OPEN = 3
ONE_ERR_UHF_LINK = 4
ONE_ERR_NO_TAG = 5
ONE_ERR_MISMATCH = 6
ONE_ERR_HOMING = 7
ONE_ERR_MOTOR_FAULT = 8
ONE_ERR_MOTOR_TIMEOUT = 9
ONE_ERR_AM_LINK = 10
ONE_ERR_NO_IR = 11
ONE_END_NONE = 0
ONE_END_TAG_REMOVED = 1
ONE_END_HOLD_TIMEOUT = 2
ONE_END_TAG_CHANGED = 3
ONE_END_UHF_LOST = 4
ONE_END_DEMAG_DONE = 5
ONE_END_ABORTED = 6
ONE_END_STABLE_OK = 7
ONE_PH_NONE = 0
ONE_PH_IR_WAIT = 7
ONE_RETREAT_NONE = 2

RESULT_JSONL = os.path.join(_HERE, "results.jsonl")
HW_JSON = os.path.join(_HERE, "hw_presence.json")


class Recorder:
    def __init__(self, phase):
        self.phase = phase
        self.rows = []

    def rec(self, cid, name, verdict, msg=""):
        row = {"phase": self.phase, "id": cid, "name": name,
               "verdict": verdict, "msg": str(msg)}
        self.rows.append(row)
        with open(RESULT_JSONL, "a", encoding="utf-8") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
        tag = {"PASS": "✅", "FAIL": "❌", "SKIP": "⏭", "OBS": "👁"}.get(verdict, "?")
        print(f"  [{tag} {verdict}] {cid} {name}: {msg}", flush=True)
        return verdict

    def pass_(self, cid, name, msg=""):
        return self.rec(cid, name, "PASS", msg)

    def fail(self, cid, name, msg=""):
        return self.rec(cid, name, "FAIL", msg)

    def skip(self, cid, name, msg=""):
        return self.rec(cid, name, "SKIP", msg)

    def obs(self, cid, name, msg=""):
        return self.rec(cid, name, "OBS", msg)

    def check(self, cid, name, cond, okmsg="", failmsg=""):
        return (self.pass_ if cond else self.fail)(
            cid, name, okmsg if cond else failmsg)

    def flush(self):
        with open(RESULT_JSONL, "a", encoding="utf-8") as f:
            for r in self.rows:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        n = {"PASS": 0, "FAIL": 0, "SKIP": 0, "OBS": 0}
        for r in self.rows:
            n[r["verdict"]] += 1
        print(f"== {self.phase} 汇总: PASS={n['PASS']} FAIL={n['FAIL']} "
              f"SKIP={n['SKIP']} OBS={n['OBS']}", flush=True)
        return n


def open_link():
    lk = HidLink()
    lk.open()
    return lk


def tx(lk, fc, data, timeout_s=2.0):
    """返回 (resp_data:bytes|None, timed_out:bool). hidapi 在设备重枚举窗口会抛
    HIDException (error waiting for more data) — 捕获按超时处理."""
    try:
        r, to = lk.transaction(fc, bytes(data), timeout_s=timeout_s)
        return (r["data"] if r else None), to
    except Exception:
        return None, True


def reopen_until_alive(timeout_s=20):
    """设备复位/重枚举后重开句柄直至握手成功. 返回 (lk|None, 耗时s)."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        lk = None
        try:
            lk = HidLink()
            lk.open()
            d, to = tx(lk, C.FC_HANDSHAKE, [0x01], timeout_s=2)
            # Layer 字节 (data[19]) =1 才是 App; =0 是 Boot 驻留窗口 (复位后
            # ~1s 内 Boot 应答握手但不答 App 业务命令), 必须等 App 真正起来
            if (not to) and d is not None and len(d) > 19 and d[19] == 1:
                drain(lk, 0.5)   # 排空启动期堆积的陈旧回显风暴 (逐帧应答设计)
                return lk, time.time() - t0
        except Exception:
            pass
        if lk is not None:
            try:
                lk.close()
            except Exception:
                pass
        time.sleep(0.5)
    return None, time.time() - t0


def txm(lk, fc, data, timeout_s=3.0, retries=3):
    """回显匹配事务: 丢弃错位响应 (启动期陈旧回显风暴), 直至 data[0]==期望 sub.
    返回 (resp_data|None, timed_out)."""
    want_sub = data[0] if data else None
    for _ in range(retries):
        d, to = tx(lk, fc, data, timeout_s=timeout_s)
        if d is None:
            continue
        if want_sub is None or (len(d) > 0 and d[0] == want_sub):
            return d, False
    return None, True


def drain(lk, dur_s=0.3):
    """清空接收缓冲里未消费的帧."""
    t0 = time.time()
    while time.time() - t0 < dur_s:
        f = lk.recv_frame(timeout_s=0.05)
        if f is None:
            break


def alive(lk):
    d, to = tx(lk, C.FC_HANDSHAKE, [0x01], timeout_s=2)
    return (not to) and d is not None and d[0] == 0x00


def uhf_tags(lk, rounds=2):
    """同步盘点, 返回在场 EPC 列表 (hex)."""
    tags = []
    tx(lk, C.FC_UHF_CTRL, [C.UHF_OPEN], timeout_s=4)
    for _ in range(rounds):
        d, to = tx(lk, C.FC_UHF_CTRL, [C.UHF_INVENTORY, 0xE8, 0x03], timeout_s=8)
        if d and not to and d[1] == C.UHF_ERR_OK:
            cnt = d[2] | (d[3] << 8)
            p = 4
            for _ in range(min(cnt, 16)):
                el = d[p + 1]
                tags.append(d[p + 2:p + 2 + el].hex())
                p += 2 + el
    return tags


def load_hw():
    if os.path.exists(HW_JSON):
        with open(HW_JSON, encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_hw(hw):
    with open(HW_JSON, "w", encoding="utf-8") as f:
        json.dump(hw, f, ensure_ascii=False, indent=1)


def locker_idle(lk):
    """确保 Locker 回 IDLE (取消幂等)."""
    d, to = tx(lk, C.FC_LOCKER_CTRL, [C.LOCKER_CANCEL], timeout_s=6)
    return d


def parse_selftest(d):
    """SELFTEST_SUB_QUERY 响应 -> dict."""
    if not d or len(d) < 10:
        return None
    return {"err": d[1], "errBits": d[2] | (d[3] << 8),
            "motorCommOk": d[4], "drvFault": d[5], "uhfLink": d[6],
            "amLink": d[7], "paramCrc": d[8], "switchErr": d[9]}


SELF_BIT_NAMES = ["MOTOR_SPI", "MOTOR_FAULT", "UHF_COMM", "AM_COMM",
                  "PARAM_CRC", "TRAVEL_SW"]


def bits_str(bits):
    return ",".join(n for i, n in enumerate(SELF_BIT_NAMES) if bits >> i & 1) or "0"
