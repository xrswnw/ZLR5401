"""Boot 升级客户端 (zlr_updater).

基于 Round_008 的 HidLink, 实现 Boot 侧完整升级协议:
    ENTER_BOOT -> START(擦) -> DATA(分块) -> VERIFY -> EXEC(两步提交+复位)
并提供:
    - 进/出 boot 的重枚举 + 握手层判定
    - 升级前/后 upgradeCount 读取 (+1 为真实提交证据)
    - 可注入的块级/命令级扰动 (丢帧/坏CRC/延时/重复/乱序) 供故障注入用例
"""
import sys
import os
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Round_008", "test"))

from zlr.hid_link import HidLink  # noqa
from . import conf  # noqa
from .fw_crc import fw_crc  # noqa


class Updater:
    def __init__(self, fw_bin=conf.FW_BIN, verbose=True):
        self.fw_bin = fw_bin
        self.link = HidLink()
        self.fw = None        # bytes
        self.fw_size = 0
        self.fw_crc = 0
        self.verbose = verbose
        self._log("updater ready fw=%s" % fw_bin)

    def _log(self, m):
        if self.verbose:
            print("[update] " + m, flush=True)

    # ---------- 固件装载 ----------
    def load_fw(self, blob=None):
        if blob is None:
            with open(self.fw_bin, "rb") as fh:
                blob = fh.read()
        self.fw = bytes(blob)
        self.fw_size = len(self.fw)
        self.fw_crc = fw_crc(self.fw)
        # 静态断言 MSP / Reset 向量 (MID 校验会检查)
        msp = int.from_bytes(self.fw[0:4], "little")
        rv = int.from_bytes(self.fw[4:8], "little")
        assert (msp & 0x2FFC0000) == 0x20000000, "MSP vector invalid in fw"
        assert conf.APP_FLASH_ORIGIN and rv >= 0x08006000, "Reset vector out of APP"
        self._log(f"fw size={self.fw_size} crc=0x{self.fw_crc:08X} "
                  f"msp=0x{msp:08X} reset=0x{rv:08X}")
        return self.fw

    # ---------- 打开/重枚举 ----------
    def open_best_effort(self):
        try:
            self.link.open()
            return True
        except Exception:
            return False

    def reopen(self, max_tries=None):
        """关断并重枚举 USB (App/Boot 同 VID:PID)。返回是否成功打开。"""
        max_tries = max_tries or conf.REOPEN_MAX
        self.link.close()
        for _ in range(max_tries):
            if self.open_best_effort():
                return True
            time.sleep(conf.REOPEN_SLEEP)
        return False

    def wait_layer(self, want_layer, max_tries=None, bump=conf.BUMP_WAIT):
        """等待握手返回预期层 (0=Boot,1=App)。最多 max_tries 次握手。"""
        if bump:
            time.sleep(bump)
        max_tries = max_tries or conf.REOPEN_MAX
        for _ in range(max_tries):
            try:
                resp, to = self.link.transaction(conf.FC_HANDSHAKE, timeout_s=conf.REQ_TIMEOUT)
                if not to and conf.handshake_layer(resp["data"]) == want_layer:
                    return resp["data"]
            except Exception:
                pass
            time.sleep(conf.REOPEN_SLEEP)
        return None

    def wait_for_reboot(self, want_layer, timeout_s=15.0, bump=conf.BUMP_WAIT):
        """设备软复位/重枚举后, 一直等待其 reach 期望层。

        close+reopen+handshake 循环, 覆盖设备在复位瞬间从 USB 上短暂消失的窗口。
        """
        if bump:
            time.sleep(bump)
        deadline = time.time() + timeout_s
        opened = False
        while time.time() < deadline:
            if not opened:
                self.link.close()
                opened = self.open_best_effort()
                if not opened:
                    time.sleep(conf.REOPEN_SLEEP)
                    continue
            try:
                resp, to = self.link.transaction(
                    conf.FC_HANDSHAKE, timeout_s=conf.REQ_TIMEOUT)
                if not to and conf.handshake_layer(resp["data"]) == want_layer:
                    return resp["data"]
            except Exception:
                opened = False  # 句柄失效, 重新打开
            time.sleep(conf.REOPEN_SLEEP)
        return None

    # ---------- 握手 + 读取 upgradeCount ----------
    def read_count(self):
        resp, to = self._safe_transaction(conf.FC_HANDSHAKE)
        if to or not resp:
            return None
        return conf.handshake_upgrade_count(resp["data"]), conf.handshake_layer(resp["data"])

    def _safe_transaction(self, fc, data=b"", timeout_s=conf.REQ_TIMEOUT):
        """发送并等待响应; 句柄失效(设备重枚举/复位)时自动重开重试。"""
        for _ in range(4):
            try:
                return self.link.transaction(fc, data, timeout_s=timeout_s)
            except Exception:
                pass  # 句柄失效, 重开
            if not self.reopen():
                time.sleep(0.4)
                continue
            time.sleep(0.3)
        return None, True

    # ---------- 升级命令 ----------
    def cmd(self, fc, data=b"", timeout_s=conf.REQ_TIMEOUT):
        """发送命令请求并等待响应, 返回 (rsp_dict|None, timed_out)."""
        return self._safe_transaction(fc, data, timeout_s)

    def enter_boot(self):
        """从 App 进 Boot (须复位换固件)。写 UPG, 设备软复位到 Boot。"""
        resp, to = self.cmd(conf.FC_ENTER_BOOT)
        if to or not resp or resp["data"][0] != 0:
            raise RuntimeError(f"ENTER_BOOT fail rsp={resp and resp['data']}")
        d = self.wait_for_reboot(0)
        if d is None:
            raise RuntimeError("did not reach Boot(layer=0) after enter_boot")
        return d

    def start(self, verify_level=conf.VERIFY_LEVEL_MID):
        """FC_UPGRADE_START: 16B 头 + 校验级别。擦除 App 区。"""
        hdr = (
            (self.fw_size & 0xFF, (self.fw_size >> 8) & 0xFF,
             (self.fw_size >> 16) & 0xFF, (self.fw_size >> 24) & 0xFF)
            + ((self.fw_crc & 0xFF), (self.fw_crc >> 8) & 0xFF,
               (self.fw_crc >> 16) & 0xFF, (self.fw_crc >> 24) & 0xFF)
            + (0, 0, 0, 0)                       # FwVer (0)
            + (0, 0, 0, 0)                       # BindVerify (0)
        )
        payload = bytes(hdr) + bytes([verify_level])
        resp, to = self.cmd(conf.FC_UPGRADE_START, payload)
        if to or not resp or resp["data"][0] != 0:
            return resp["data"][0] if (resp and resp["data"]) else -1
        return 0

    def send_chunk(self, seq, addr_offset, chunk_bytes, corrupt_frame=False,
                   delay_after=0.0, skip_response=False, injected=False):
        """发一个 DATA 块。返回 (rsp, ok_bool)，ok 表示【本次尝试】成功写入。

        seq: 16bit; addr_offset: 16bit 相对 APP 基址; chunk<=52。
        corrupt_frame: 翻转帧级 CRC，设备静默丢弃 -> 本次失败(ok=False)，由调用方重发。
        skip_response: 已发出正确帧但模拟"丢响应/超时"，返回失败供重发(设备幂等写)。
        injected: 纯"本块本次不投递"(模拟丢包)，ok=False，重发投递。
        """
        data = bytes([seq & 0xFF, (seq >> 8) & 0xFF,
                      addr_offset & 0xFF, (addr_offset >> 8) & 0xFF]) + chunk_bytes
        if injected:
            return None, False          # 本次不投递，视为失败，重发
        if corrupt_frame:
            self._send_corrupt(conf.FC_FW_DATA, data)
            return None, False          # 设备丢弃，本次失败，重发
        resp, to = self.cmd(conf.FC_FW_DATA, data)
        if skip_response:
            return None, False          # 已写但假装响应超时，重发覆盖(幂等)
        if to or not resp:
            return None, False
        # rsp: [seqL,seqH,err]
        err = resp["data"][2] if len(resp["data"]) > 2 else -1
        seq_echo = (resp["data"][0] | (resp["data"][1] << 8)) if resp["data"] else -1
        ok = (err == 0 and seq_echo == (seq & 0xFFFF))
        if delay_after:
            time.sleep(delay_after)
        return resp, ok

    def _send_corrupt(self, fc, data):
        """发送帧级 CRC 错误的帧, 设备应静默丢弃。构建工具与 HidLink 一致但改 CRC。"""
        from zlr.frame import build_frame
        good = build_frame(conf.DEV_ADDR, fc, data)
        bad = bytearray(good)
        bad[-1] ^= 0xFF
        self.link.send_bytes(bytes(bad))
        # 等待片刻确保设备收到并丢弃
        time.sleep(0.15)
        return None, True

    def verify(self):
        resp, to = self.cmd(conf.FC_UPGRADE_VERIFY)
        if to or not resp:
            return -1
        return resp["data"][0]

    def exec(self):
        resp, to = self.cmd(conf.FC_UPGRADE_EXEC)
        if to or not resp:
            return None
        return resp["data"]

    # ---------- 完整一轮升级 ----------
    def full_update(self, blob=None, verify_level=conf.VERIFY_LEVEL_MID,
                    chunk_cfg=None, before_count=None):
        """完整一轮: 假定当前已处于 Boot。返回 dict(结果)。"""
        self.load_fw(blob)
        before = before_count if before_count is not None else self.read_count()
        e = self.start(verify_level)
        if e != 0:
            return {"ok": False, "stage": "start", "err": e}
        n = 0
        offset = 0
        while offset < self.fw_size:
            chunk = self.fw[offset:offset + conf.CHUNK]
            ok = False
            for retry in range(conf.CHUNK_RETRY):
                stall = {}
                if chunk_cfg:
                    stall = chunk_cfg(seq=n, offset=offset, retry=retry,
                                      chunk=chunk, upd=self)
                resp, ok = self.send_chunk(
                    n, offset, chunk,
                    corrupt_frame=stall.get("corrupt", False),
                    delay_after=stall.get("delay", 0.0),
                    skip_response=stall.get("skip_resp", False),
                    injected=stall.get("injected", False))
                if ok:
                    break
                if stall.get("no_retry", False):
                    break
                time.sleep(0.05)
            if not ok:
                return {"ok": False, "stage": "data", "seq": n, "offset": offset,
                        "retry_total": retry + 1}
            n += 1
            offset += len(chunk)
        v = self.verify()
        if v != 0:
            return {"ok": False, "stage": "verify", "err": v}
        self.exec()
        # EXEC 会复位; 等待回到 App
        d = self.wait_for_reboot(1)
        if d is None:
            return {"ok": False, "stage": "app_layer", "layer": "?"}
        after = self.read_count()
        if after is None:
            return {"ok": False, "stage": "count_after"}
        after_cnt = after[0]
        if before is None or after_cnt != before[0] + 1:
            return {"ok": False, "stage": "count_mismatch",
                    "before": before[0] if before else None, "after": after_cnt}
        return {"ok": True, "before": before[0] if before else None, "after": after_cnt}
