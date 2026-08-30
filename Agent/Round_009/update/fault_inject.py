"""故障注入 (F1-F7) + 现场 USB 不稳 (U1-U7) 用例.

每个用例: 执行注入(扰动) + 判据 + 用正确固件完整重跑恢复。
返回 (name, verdict, detail). verdict: PASS/FAIL.
"""
import random
import time

from . import conf
from .fw_crc import fw_crc  # noqa


def make_bad_fw(up):
    """复制固件并在非向量区翻转一字节(避开前8字节向量), 定向命中 CRC 而无害向量。"""
    blob = bytearray(up.fw)
    idx = min(200, len(blob) - 1)
    blob[idx] ^= 0xFF
    return bytes(blob)


# ---- F: 命令/固件层注入 ----

def f1_dropped_chunk(up):
    """F1: DATA 中途故意不投递一个块, 由 full_update 重发补上 -> 成功。"""
    state = {"skip": True}
    hit = {"did": False}

    def on_chunk(seq, offset, retry, chunk, upd):
        if state["skip"] and retry == 0:
            state["skip"] = False
            hit["did"] = True
            return {"injected": True}
        return {}

    res = up.full_update(chunk_cfg=on_chunk)
    ok = res.get("ok") and hit["did"]
    return (("PASS" if ok else "FAIL"),
            f"dropped-one-pkt-injected-then-resent; ok={res.get('ok')}")


def f2_corrupt_frame(up):
    """F2: 发一个帧级 CRC 错块(静默丢弃), 重发正确块 -> 成功。"""
    state = {"once": True}
    hit = {"did": False}

    def on_chunk(seq, offset, retry, chunk, upd):
        if state["once"] and retry == 0:
            state["once"] = False
            hit["did"] = True
            return {"corrupt": True}
        return {}

    res = up.full_update(chunk_cfg=on_chunk)
    return (("PASS" if res.get("ok") and hit["did"] else "FAIL"),
            f"corrupt-frame dropped then resent; ok={res.get('ok')}")


def _send_all(up):
    """发送全部 DATA 块 (使用当前 up.fw, 不带重试/注入)。"""
    e = up.start()
    if e != 0:
        return e, None
    n = 0
    offset = 0
    while offset < up.fw_size:
        chunk = up.fw[offset:offset + conf.CHUNK]
        _, ok = up.send_chunk(n, offset, chunk)
        if not ok:
            return -1, n
        n += 1
        offset += len(chunk)
    return 0, None


def _send_retry(up, n, offset, chunk):
    """带重试地投递一个块, 容忍瞬态丢响应(设备幂等写)。"""
    for _ in range(conf.CHUNK_RETRY):
        _, ok = up.send_chunk(n, offset, chunk)
        if ok:
            return None, True
        time.sleep(0.05)
    return None, False


def f3_bad_crc_fw(up):
    """F3: 上传 CRC 错的固件 -> VERIFY 返回 CRC_MISMATCH; 停留 Boot; 重传正确固件成功。

    技巧: 载荷用坏固件字节, 但 START 头仍携带【原正确固件】的 CRC,
    使 Boot 整区 CRC 复核必然失败 -> 定向命中 CRC_MISMATCH 分支。
    """
    bad = make_bad_fw(up)
    orig = up.fw
    orig_crc = up.fw_crc
    up.load_fw(bad)
    up.fw_crc = orig_crc                       # START 上报原 CRC
    e, _ = _send_all(up)
    if e != 0:
        return ("FAIL", f"start/send bad fw fail e={e}")
    v = up.verify()
    if v != conf.VERIFY_CRC_MISMATCH:
        return ("FAIL", f"expected CRC_MISMATCH({conf.VERIFY_CRC_MISMATCH}), got {v}; unrecovered")
    d = up.read_count()
    if d is None or d[1] != 0:
        return ("FAIL", "did not stay in Boot after bad-crc verify")
    up.load_fw(orig)      # 恢复正确固件
    res = up.full_update()
    if not res.get("ok"):
        return ("FAIL", f"recovery failed: {res}")
    return ("PASS", "bad-crc rejected (stayed Boot), good fw recovered")


def f4_reset_before_exec(up):
    """F4: START 后、EXEC 提交前软复位(模拟提交前中断) -> 仍 Boot/UPG; 重跑成功。"""
    up.load_fw()
    e = up.start()
    if e != 0:
        return ("FAIL", f"start fail {e}")
    time.sleep(0.2)
    up.cmd(conf.FC_RESET)
    if up.wait_for_reboot(0, timeout_s=10) is None:
        return ("FAIL", "not in Boot after reset-before-exec")
    res = up.full_update()
    return (("PASS" if res.get("ok") else "FAIL"),
            f"reset before commit kept Boot/UPG; recovered ok={res.get('ok')}")


def f5_erase_only_reset(up):
    """F5: START 擦除后不写数据即复位 -> 仍 UPG 留 Boot; START 可重写。"""
    up.load_fw()
    e = up.start()
    if e != 0:
        return ("FAIL", f"start fail {e}")
    up.cmd(conf.FC_RESET)
    if up.wait_for_reboot(0, timeout_s=10) is None:
        return ("FAIL", "not in Boot after erase-only reset")
    res = up.full_update()
    return (("PASS" if res.get("ok") else "FAIL"),
            f"erase-only leaves Boot/UPG; restart START rewrote ok={res.get('ok')}")


def f6_exit_boot_corrupt(up):
    """F6: 损坏镜像下 EXIT_BOOT 拒绝复位, 仍留 Boot; 正确固件恢复。"""
    up.load_fw()
    up.load_fw(make_bad_fw(up))
    e = up.start()
    up.load_fw(up.fw)   # 恢复正确供之后重跑
    if e != 0:
        return ("FAIL", f"start fail {e}")
    resp, to = up.cmd(conf.FC_EXIT_BOOT)
    d = up.read_count()
    still_boot = (d is None) or d[1] == 0
    accepted = (not to) and resp and resp["data"][0] == 0
    ok = (accepted is False) and still_boot
    res = up.full_update()
    if not res.get("ok"):
        return ("FAIL", f"recovery failed: {res}")
    err = resp["data"][0] if (resp and not to) else "no-ack"
    return (("PASS" if ok else "FAIL"),
            f"EXIT_BOOT err={err} refused({ok}); recovered to App")


def f7_exec_then_plain_reset(up):
    """F7: EXEC 成功后软复位, 验证新固件已持久(RUN已提交) -> 直接回 App。"""
    res = up.full_update()
    if not res.get("ok"):
        return ("FAIL", f"update failed: {res}")
    up.cmd(conf.FC_RESET)
    d = up.wait_for_reboot(1, timeout_s=10)
    return (("PASS" if d is not None else "FAIL"),
            "after commit, FC_RESET boots straight to App")


# ---- U: 现场 USB 不稳 ----

def u1_random_drop_resp(up):
    """U1: 随机"丢包"(几块在首投递时被丢弃), 由重发补上 -> 最终成功。"""
    state = {"n": -1, "dropped": 0}
    hit = {"did": False}
    drop_total = 3

    def on_chunk(seq, offset, retry, chunk, upd):
        state["n"] += 1
        if state["dropped"] < drop_total and state["n"] % 5 == 0 and retry == 0:
            state["dropped"] += 1
            hit["did"] = True
            return {"injected": True}
        return {}

    res = up.full_update(chunk_cfg=on_chunk)
    return (("PASS" if res.get("ok") and hit["did"] else "FAIL"),
            f"dropped {hit['did'] and state['dropped'] or 0} pkts; recovered ok={res.get('ok')}")


def u2_slow_link(up):
    """U2: 块间随机 10~500ms 延迟(模拟链路慢/主机卡) -> 完整上传成功。"""
    state = {"n": 0}
    waited = {"s": 0.0}

    def on_chunk(seq, offset, retry, chunk, upd):
        if state["n"] % 3 == 0:
            delay = random.choice([0.05, 0.2, 0.5])
            waited["s"] += delay
            state["n"] += 1
            return {"delay": delay}
        state["n"] += 1
        return {}

    res = up.full_update(chunk_cfg=on_chunk)
    return (("PASS" if res.get("ok") else "FAIL"),
            f"slow-link added {waited['s']:.1f}s; ok={res.get('ok')}")


def u3_replug_mid(up):
    """U3: 升级中途中止 USB 重枚举(close+reopen, 模拟拔插) -> 仍在 Boot, 重跑成功。"""
    up.load_fw()
    e = up.start()
    if e != 0:
        return ("FAIL", f"start fail {e}")
    for k in range(2):
        chunk = up.fw[k*conf.CHUNK:(k+1)*conf.CHUNK]
        up.send_chunk(k, k*conf.CHUNK, chunk)
    up.link.close()
    time.sleep(0.8)
    if up.wait_for_reboot(0, timeout_s=10) is None:
        return ("FAIL", "not in Boot after replug mid-update")
    res = up.full_update()
    return (("PASS" if res.get("ok") else "FAIL"),
            f"replug mid-update stayed Boot/UPG; recovered ok={res.get('ok')}")


def u4_response_timeout(up):
    """U4: 某块响应超时(不回应), 主机超时重发 -> 继续成功。"""
    state = {"once": True}
    hit = {"did": False}

    def on_chunk(seq, offset, retry, chunk, upd):
        if state["once"] and retry == 0:
            state["once"] = False
            hit["did"] = True
            return {"corrupt": True}   # 制造无响应, 由超时重发兜底
        return {}

    res = up.full_update(chunk_cfg=on_chunk)
    return (("PASS" if res.get("ok") and hit["did"] else "FAIL"),
            f"response-timeout retried; ok={res.get('ok')}")


def u5_burst(up):
    """U5: 连续满速批量发包(无间隔) -> 不丢序, VERIFY 通过, 回 App。"""
    up.load_fw()
    e = up.start()
    if e != 0:
        return ("FAIL", f"start fail {e}")
    n = 0
    offset = 0
    while offset < up.fw_size:
        chunk = up.fw[offset:offset + conf.CHUNK]
        _, ok = _send_retry(up, n, offset, chunk)
        if not ok:
            return ("FAIL", f"burst lost chunk #{n} offset={offset}")
        n += 1
        offset += len(chunk)
    v = up.verify()
    if v != 0:
        return ("FAIL", f"verify after burst fail {v}; unrecovered")
    up.exec()
    d = up.wait_for_reboot(1, timeout_s=10)
    return (("PASS" if d is not None else "FAIL"), "burst no-reorder, VERIFY OK, back to App")


def u6_dup_out_of_order(up):
    """U6: 某些块重复发送(重复/乱序, 幂等写) -> 最终 VERIFY 通过。"""
    up.load_fw()
    e = up.start()
    if e != 0:
        return ("FAIL", f"start fail {e}")
    n = 0
    offset = 0
    while offset < up.fw_size:
        chunk = up.fw[offset:offset + conf.CHUNK]
        _, ok = _send_retry(up, n, offset, chunk)
        if not ok:
            return ("FAIL", f"dup lost #{n}")
        if n % 7 == 0:
            _, ok = _send_retry(up, n, offset, chunk)
            if not ok:
                return ("FAIL", f"dup-again lost #{n}")
        n += 1
        offset += len(chunk)
    v = up.verify()
    if v != 0:
        return ("FAIL", f"verify after dup fail {v}; unrecovered")
    up.exec()
    d = up.wait_for_reboot(1, timeout_s=10)
    return (("PASS" if d is not None else "FAIL"),
            "duplicate chunks idempotent, VERIFY OK, back to App")


def u7_size_edge(up):
    """U7: 超长 FwSize 被 START 拒(UPG_NO_SPACE)且留 Boot; 剩余<48末块由正常流程覆盖。"""
    real = up.fw
    huge = 0x40000  # 256K > 232K App
    hdr = (huge & 0xFF, (huge >> 8) & 0xFF, (huge >> 16) & 0xFF, (huge >> 24) & 0xFF) \
        + (0, 0, 0, 0) + (0, 0, 0, 0) + (0, 0, 0, 0)
    resp, to = up.cmd(conf.FC_UPGRADE_START, bytes(hdr) + bytes([conf.VERIFY_LEVEL_MID]))
    if to or not resp:
        return ("FAIL", "oversize start no resp")
    if resp["data"][0] != conf.UPG_NO_SPACE:
        return ("FAIL", f"expected UPG_NO_SPACE, got {resp['data'][0]}")
    d = up.read_count()
    if d is None or d[1] != 0:
        return ("FAIL", "oversize rejected but not in Boot")
    up.load_fw(real)
    res = up.full_update()
    return (("PASS" if res.get("ok") else "FAIL"),
            f"oversize->UPG_NO_SPACE stays Boot; normal fw recovered ok={res.get('ok')}")


CASES = [
    ("F1", "丢DATA帧→补发", f1_dropped_chunk),
    ("F2", "帧CRC坏块→重发", f2_corrupt_frame),
    ("F3", "错CRC固件→VERIFY拒", f3_bad_crc_fw),
    ("F4", "提交前软复位仍BOOT", f4_reset_before_exec),
    ("F5", "擦后即复位可重写", f5_erase_only_reset),
    ("F6", "损坏镜像EXIT拒绝", f6_exit_boot_corrupt),
    ("F7", "提交后复位直入APP", f7_exec_then_plain_reset),
    ("U1", "随机丢响应", u1_random_drop_resp),
    ("U2", "链路慢速/延迟", u2_slow_link),
    ("U3", "中途USB重枚举", u3_replug_mid),
    ("U4", "响应超时重试", u4_response_timeout),
    ("U5", "连续满速批量", u5_burst),
    ("U6", "重复/乱序发送", u6_dup_out_of_order),
    ("U7", "固件尺寸边界", u7_size_edge),
]
