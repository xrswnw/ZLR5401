"""P7 — IAP 升级全链 + 校验注入 (CRC/SIZE/VECTOR/BIND 只 VERIFY 拒 EXEC)."""
import sys, os, time
sys.path.insert(0, '.')
sys.path.insert(0, '../../Round_008/test')
sys.path.insert(0, '../../Round_009/update')
from zlr import const as C
from zlr.hid_link import HidLink
from zlr.frame import build_frame
from zlr.crc32 import crc32
from lib import Recorder, tx, txm, reopen_until_alive

FW_PATH = os.path.abspath(os.path.join(
    os.path.dirname(__file__), '..', '..', '..', 'Build', 'app.bin'))

FC_HANDSHAKE, FC_ENTER_BOOT, FC_UPG_START, FC_FW_DATA, FC_VERIFY, FC_EXEC = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06)
FC_DEVICE_INFO, FC_RESET, FC_EXIT_BOOT = 0x07, 0x08, 0x09
UPG_NO_SPACE = 1
DATA_ADDR_ERR = 3
VERIFY_CRC_MISMATCH, VERIFY_SIZE_MISMATCH, VERIFY_VECTOR_INVALID, VERIFY_BIND_FAIL = 1, 2, 3, 4
# VerifyLevel 语义以 Boot_Protocol.html 为准: 0=FULL / 1=MID(默认,含向量) / 2=BASIC
# (Round_098 BUG#7: 原先误用 2=MID/3=FULL (与 Round_009 conf.py 同源错位),
#  I6C 零映像实际按 BASIC 跳过向量校验被 VERIFY 放行, I6C-x EXEC 直接提交了
#  垃圾固件并复位 — "I6C 挂死"实为脚本句柄失效 + (旧固件) BUG#5 死模式叠加)
VERIFY_LEVEL_FULL, VERIFY_LEVEL_MID, VERIFY_LEVEL_BASIC = 0, 1, 2

CHUNK = 240          # FW_DATA payload: 帧data = 6+240 = 246B -> 5 HID reports


def u32(v):
    return [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF]


def u16(v):
    return [v & 0xFF, (v >> 8) & 0xFF]


class BootLink(HidLink):
    """多 report 大帧发送: 每片 [rid=0x01, 63B]."""

    def send_bytes_big(self, raw):
        # 每片 1ms 节拍: 设备 OUT 环形缓冲 512B 仅容 ~8 report,
        # 背靠背连灌会撑满 -> 端点 NAK -> macOS IOHIDDeviceSetReport
        # 无限阻塞 (Round_098 I6A 偶发挂死根因). 节拍留出主循环消费窗口.
        for off in range(0, len(raw), 63):
            pkt = bytearray(64)
            pkt[0] = 0x01
            chunk = raw[off:off + 63]
            pkt[1:1 + len(chunk)] = chunk
            self.dev.write(bytes(pkt))
            time.sleep(0.001)


def txf(lk, fc, data=b"", timeout_s=3.0):
    """Boot 事务: 返回 (resp_data|None, timed_out)."""
    try:
        r, to = lk.transaction(fc, bytes(data), timeout_s=timeout_s, dev_addr=1)
        return (r["data"] if r else None), to
    except Exception:
        return None, True


def reopen_layer(lk_want, layer, timeout_s=20):
    """重开设备直至握手 layer 匹配 (0=Boot, 1=App). 返回 lk|None."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        lk = None
        try:
            lk = BootLink()
            lk.open()
            d, _ = txf(lk, FC_HANDSHAKE, b"", timeout_s=1.5)
            if d is not None and len(d) > 19 and d[19] == layer:
                try:
                    lk.recv_frame(0.4)   # 排空启动期陈旧回显
                except Exception:
                    pass
                return lk
        except Exception:
            pass
        if lk is not None:
            try:
                lk.close()
            except Exception:
                pass
        time.sleep(0.4)
    return None


def upload(lk, rec, fw, size, crc, level=VERIFY_LEVEL_MID, bind=0, ver=0x00010003,
           tag=""):
    """一轮完整上传 (START+全部 FW_DATA). 返回 True/False."""
    # 排空陈旧回显, 防上一轮残留 ack 干扰本轮 seq 匹配
    try:
        lk.recv_frame(0.1)
    except Exception:
        pass
    d, _ = txf(lk, FC_UPG_START, bytes(u32(size) + u32(crc) + u32(ver)
                                       + u32(bind) + [level]), timeout_s=15)
    if d is None or d[0] != 0:
        rec.fail(f"{tag}-start", "UPGRADE_START", f"resp={d.hex() if d else 'TO'}")
        return False
    n = 0
    off = 0
    while off < size:
        payload = bytes(fw[off:off + CHUNK])
        if not payload:
            break   # 源数据耗尽 (SIZE 注入: 声称多于实发, 留给 VERIFY 判 SIZE_MISMATCH)
        frame = build_frame(1, FC_FW_DATA, bytes(u16(n) + u32(off)) + payload)
        lk.send_bytes_big(frame)
        # 收 ack: [seqL,seqH,result]
        ack = None
        t0 = time.time()
        while time.time() - t0 < 3:
            f = lk.recv_frame(1.0)
            if f and f["func"] == (FC_FW_DATA ^ 0xFF) and len(f["data"]) >= 3:
                aseq = f["data"][0] | (f["data"][1] << 8)
                if aseq == n:
                    ack = f["data"]
                    break
                # 陈旧 ack 丢弃继续收
        if ack is None or ack[2] != 0:
            rec.fail(f"{tag}-data{n}", f"FW_DATA seq={n} off={off}",
                     f"ack={ack.hex() if ack else 'TO'}")
            return False
        off += len(payload)
        n += 1
    return True


def run():
    fw = open(FW_PATH, 'rb').read()
    S, CRC = len(fw), crc32(fw)
    rec = Recorder("P7-IAP")
    print(f"固件: {FW_PATH} size={S} crc=0x{CRC:08X}")

    lk = BootLink(); lk.open()
    d, _ = txf(lk, FC_HANDSHAKE)
    layer0 = d[19] if d is not None and len(d) > 19 else None
    if layer0 == 1:
        rec.check("I1", "App 在线 (layer=1)", True, "layer=1", "")
        d, _ = txf(lk, FC_ENTER_BOOT)
        rec.check("I2a", "ENTER_BOOT -> OK", d is not None and len(d) >= 1 and d[0] == 0,
                  f"r={d.hex() if d else 'TO'}", "")
    else:
        rec.obs("I1", "起始已在 Boot (上次流程遗留 UPG)", f"layer={layer0}")
        rec.obs("I2a", "ENTER_BOOT 跳过 (已在 Boot)", "")
    if lk:
        lk.close()
    lk = reopen_layer(None, 0, 20)
    rec.check("I2b", "复位后 Boot 驻留 (layer=0)", lk is not None, "layer=0", "未入 Boot")
    cnt0 = None
    if lk:
        d, _ = txf(lk, FC_HANDSHAKE)
        cnt0 = int.from_bytes(d[20:24], 'little') if d and len(d) >= 24 else None
        rec.obs("I2c", f"Boot 握手 upgradeCount={cnt0}", "")

    # ---- Boot 专属命令面 ----
    d, _ = txf(lk, FC_DEVICE_INFO)
    rec.check("I3a", "Boot DEVICE_INFO 响应", d is not None and len(d) >= 3,
              f"len={len(d) if d else 0}", "")
    d, to = txf(lk, 0x0A, [0x05], timeout_s=1.5)      # App 专属 FC, Boot 静默
    rec.check("I3b", "Boot 对 App FC(0x0A) 静默", d is None, "无响应", f"resp={d}")

    # ---- EXIT_BOOT 回 App (App 区当前有效) ----
    d, _ = txf(lk, FC_EXIT_BOOT)
    rec.check("I4a", "EXIT_BOOT -> OK (App 完好)", d is not None and d[0] == 0,
              f"r={d.hex() if d else 'TO'}", "")
    if lk:
        lk.close()
    lk2 = reopen_layer(None, 1, 25)
    rec.check("I4b", "退出后回 App (layer=1)", lk2 is not None, "App 在线", "")
    if lk2:
        lk2.close()

    # ---- Happy path: 完整升级同一镜像 ----
    lk3 = lk4 = None
    lk = BootLink(); lk.open()
    d, _ = txf(lk, FC_ENTER_BOOT)
    if lk:
        lk.close()
    lk = reopen_layer(None, 0, 20)
    rec.check("I5a", "再入 Boot", lk is not None, "layer=0", "")

    # 参数错误: FW_DATA 未 START 先发
    d, _ = txf(lk, FC_FW_DATA, bytes(u16(0) + u32(0)) + b'ABCD', timeout_s=2)
    rec.check("I5b", "未 START 先 FW_DATA -> DATA_ADDR_ERR(3)",
              d is not None and len(d) >= 3 and d[2] == DATA_ADDR_ERR,
              f"r={d.hex() if d else 'TO'}", "")
    # 参数错误: FwSize=0
    d, _ = txf(lk, FC_UPG_START, bytes(u32(0) + u32(CRC) + u32(1) + u32(0) + [2]),
               timeout_s=4)
    rec.check("I5c", "FwSize=0 -> 拒绝", d is not None and d[0] != 0,
              f"r={d.hex() if d else 'TO'}", "")
    # 参数错误: FwSize 超 232KB
    d, _ = txf(lk, FC_UPG_START, bytes(u32(300 * 1024) + u32(CRC) + u32(1) + u32(0) + [2]),
               timeout_s=4)
    rec.check("I5d", "FwSize=300KB -> UPG_NO_SPACE(1)", d is not None and d[0] == UPG_NO_SPACE,
              f"r={d.hex() if d else 'TO'}", "")

    t0 = time.time()
    okup = upload(lk, rec, fw, S, CRC, tag="I5")
    rec.check("I5e", f"上传 {S}B / {CHUNK}B 分片 / u32 偏移 (BUG#4 修复验证)", okup,
              f"耗时{time.time()-t0:.1f}s", "上传中断")
    if okup:
        d, _ = txf(lk, FC_VERIFY, b"", timeout_s=5)
        rec.check("I5f", "VERIFY -> OK (CRC+SIZE+VECTOR)",
                  d is not None and d[0] == 0 and d[1] == 0,
                  f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_EXEC, b"", timeout_s=5)
        rec.check("I5g", "EXEC -> OK + 复位", d is not None and d[0] == 0,
                  f"r={d.hex() if d else 'TO'}", "")
        lk.close()
        lk3 = reopen_layer(None, 1, 25)
        rec.check("I5h", "升级后 App 启动 (layer=1)", lk3 is not None, "App 在线", "")
        if lk3:
            d = None
            for _ in range(6):   # 陈旧握手回显风暴中重试匹配 MOTOR_QUERY
                d, _ = txf(lk3, 0x0A, [0x05], timeout_s=1.5)
                if d is not None and len(d) >= 5 and d[0] == 5:
                    break
            rec.check("I5i", "新 App 业务可用 (MOTOR_QUERY)",
                      d is not None and len(d) >= 5 and d[0] == 5,
                      f"state={d[4] if d and len(d)>4 else '?'}",
                      f"echo-storm d={d.hex() if d else 'TO'}")
            d = None
            cnt1 = None
            for _ in range(6):   # 陈旧 Boot 握手回显 (同 FC) 干扰: 按 layer=1 过滤重试
                d, _ = txf(lk3, FC_HANDSHAKE, timeout_s=1.5)
                if d is not None and len(d) >= 24 and d[19] == 1:
                    cnt1 = int.from_bytes(d[20:24], 'little')
                    break
            rec.check("I5j", "upgradeCount +1",
                      cnt0 is not None and cnt1 == cnt0 + 1,
                      f"{cnt0}->{cnt1}", "")
            lk3.close()

    # ---- 注入轮: 全程只 VERIFY 拒 EXEC ----
    for _lk in (lk, lk3, lk4):
        try:
            if _lk is not None:
                _lk.close()
        except Exception:
            pass
    lk = BootLink(); lk.open()
    txf(lk, FC_ENTER_BOOT)
    lk.close()
    lk = reopen_layer(None, 0, 20)
    rec.check("I6a", "注入轮进 Boot", lk is not None, "layer=0", "")

    # A. SIZE: FwSize 多 100B
    if upload(lk, rec, fw, S + 100, CRC, tag="I6A"):
        d, _ = txf(lk, FC_VERIFY, b"", timeout_s=5)
        rec.check("I6A", "SIZE 注入: VERIFY_SIZE_MISMATCH(2)",
                  d is not None and d[0] == VERIFY_SIZE_MISMATCH, f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_EXEC, b"", timeout_s=3)
        rec.check("I6A-x", "EXEC 被拒 (err=1)", d is not None and d[0] != 0,
                  f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_HANDSHAKE)
        rec.check("I6A-s", "拒后仍留 Boot (未跳)", d is not None and d[19] == 0, "layer=0", "")

    # B. CRC: 错误 CRC
    if upload(lk, rec, fw, S, CRC ^ 0xA5A5A5A5, tag="I6B"):
        d, _ = txf(lk, FC_VERIFY, b"", timeout_s=5)
        rec.check("I6B", "CRC 注入: VERIFY_CRC_MISMATCH(1)",
                  d is not None and d[0] == VERIFY_CRC_MISMATCH, f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_EXEC, b"", timeout_s=3)
        rec.check("I6B-x", "EXEC 被拒", d is not None and d[0] != 0,
                  f"r={d.hex() if d else 'TO'}", "")

    # C. VECTOR: 全零镜像 (CRC 自洽, MSP=0 非法) — App 区被覆写为垃圾, 绝不 EXEC
    garbage = b'\x00' * S
    if upload(lk, rec, garbage, S, crc32(garbage), tag="I6C"):
        d, _ = txf(lk, FC_VERIFY, b"", timeout_s=5)
        rec.check("I6C", "VECTOR 注入: VERIFY_VECTOR_INVALID(3)",
                  d is not None and d[0] == VERIFY_VECTOR_INVALID, f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_EXEC, b"", timeout_s=3)
        rec.check("I6C-x", "EXEC 被拒 (App 区已垃圾, 红线: 不跳)",
                  d is not None and d[0] != 0, f"r={d.hex() if d else 'TO'}", "")
        # EXIT_BOOT 也应拒绝 (App 无效)
        d, _ = txf(lk, FC_EXIT_BOOT, timeout_s=3)
        rec.check("I6C-e", "EXIT_BOOT 拒绝 (App 区无效, 不复位)",
                  d is not None and d[0] != 0, f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_HANDSHAKE)
        rec.check("I6C-s", "设备仍安全留 Boot", d is not None and d[19] == 0, "layer=0", "")

    # D. BIND: FULL 级 + 错误绑定值
    if upload(lk, rec, fw, S, CRC, level=VERIFY_LEVEL_FULL, bind=0xDEADBEEF, tag="I6D"):
        d, _ = txf(lk, FC_VERIFY, b"", timeout_s=5)
        rec.check("I6D", "BIND 注入: VERIFY_BIND_FAIL(4)",
                  d is not None and d[0] == VERIFY_BIND_FAIL, f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_EXEC, b"", timeout_s=3)
        rec.check("I6D-x", "EXEC 被拒", d is not None and d[0] != 0,
                  f"r={d.hex() if d else 'TO'}", "")

    # E. 恢复: 正常参数重传 + EXEC -> App 复活
    okE = upload(lk, rec, fw, S, CRC, tag="I6E")
    if okE:
        d, _ = txf(lk, FC_VERIFY, b"", timeout_s=5)
        rec.check("I6E-v", "恢复轮 VERIFY OK", d is not None and d[0] == 0,
                  f"r={d.hex() if d else 'TO'}", "")
        d, _ = txf(lk, FC_EXEC, b"", timeout_s=5)
        rec.check("I6E-x", "恢复轮 EXEC -> OK", d is not None and d[0] == 0,
                  f"r={d.hex() if d else 'TO'}", "")
        lk.close()
        lk4 = reopen_layer(None, 1, 25)
        rec.check("I6E-f", "恢复后 App 启动 (IAP 自愈, 无需 JLink)", lk4 is not None,
                  "App 在线", "")
        if lk4:
            lk4.close()

    rec.flush()
    return rec


if __name__ == "__main__":
    run()
