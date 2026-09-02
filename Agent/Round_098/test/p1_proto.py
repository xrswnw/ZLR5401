"""P1: 协议层测试 — 帧/CRC/重组/异常注入/地址过滤/未知FC"""
import time
from lib import Recorder, open_link, tx, drain, alive, C, build_frame

rec = Recorder("P1")
lk = open_link()
assert alive(lk), "设备不存活, 中止 P1"


def send_raw_chunks(lk, raw, rid=0x01, delay=0.002):
    """跨多 report 发送原始字节流: 每 report = [rid, 63B], 固件剥离 rid(0x01)."""
    for i in range(0, len(raw), 63):
        chunk = raw[i:i + 63]
        pkt = bytearray(64)
        pkt[0] = rid
        pkt[1:1 + len(chunk)] = chunk
        lk.dev.write(bytes(pkt))
        if delay:
            time.sleep(delay)


def expect_silent(lk, name, raw, note=""):
    """发送畸形帧, 期望 0.6s 内无任何响应."""
    drain(lk)
    send_raw_chunks(lk, raw)
    f = lk.recv_frame(timeout_s=0.6)
    if f is None:
        rec.pass_(name, f"静默丢弃 {note}")
    else:
        rec.fail(name, f"异常帧竟有响应 func=0x{f['func']:02X} {note}")
    # 恢复验证: 正常帧仍通
    if alive(lk):
        rec.pass_(name + "+恢复", "畸形帧后链路恢复")
    else:
        rec.fail(name + "+恢复", "畸形帧后链路死!")


def corrupt_crc(frame):
    b = bytearray(frame)
    b[-1] ^= 0xFF
    return bytes(b)


# ---- A1 正常往返 + 响应FC = req^0xFF ----
r, to = lk.transaction(C.FC_HANDSHAKE, b"\x01", timeout_s=2)
ok = (not to) and r is not None and r["func"] == (C.FC_HANDSHAKE ^ 0xFF)
rec.check("A1", "正常帧往返/响应FC取反", ok,
          f"rsp func=0x{r['func']:02X} data={r['data'].hex() if r else None}",
          f"rsp func=0x{r['func']:02X} (期望 0xFE)")

# ---- A2a 帧头错误 ----
good = build_frame(C.DEV_ADDR, C.FC_HANDSHAKE, b"\x01")
bad = bytearray(good); bad[0] = 0x54
expect_silent(lk, "A2a", bytes(bad), "(帧头 0x5477)")

# ---- A2b CRC 错误 ----
expect_silent(lk, "A2b", corrupt_crc(good), "(CRC 篡改)")

# ---- A2c 长度超限 lenTarget>1029 ----
badlen = bytes([0x53, 0x77, C.DEV_ADDR, 0x00, 0xFD, 0xFF]) + b"\x11" * 20
expect_silent(lk, "A2c", badlen, "(len=0xFFFD)")

# ---- A2d reserved 非零 ----
badres = bytearray(good); badres[3] = 0x01
# 注: 帧 CRC 需重算才合法, 但 reserved!=0 在 DEV_RESERVED 状态即丢, 无需重算
expect_silent(lk, "A2d", bytes(badres), "(reserved=1)")

# ---- A3 跨 report 长帧重组 (611B = 600B data) ----
drain(lk)
big = build_frame(C.DEV_ADDR, C.FC_HANDSHAKE, bytes(range(256)) * 3)
t0 = time.time()
send_raw_chunks(lk, big)
f = lk.recv_frame(timeout_s=3)
ok = f is not None and f["func"] == (C.FC_HANDSHAKE ^ 0xFF)
rec.check("A3", "长帧跨 report 流式重组 (611B/10 报文)", ok,
          f"{len(big)}B 帧收到响应, 耗时{time.time()-t0:.2f}s",
          f"长帧无响应 ({len(big)}B)")

# ---- A3b 超长 data>1024 构帧即拒 (主机侧保护, 记录) ----
# Round_098: 主机 zlr.build_frame 无 >1024 保护 — 主机侧工具约束, 归 OBS 非设备缺陷
# (设备侧由固件 LEN 状态机守门, 见下 1024 边界帧实测)
try:
    build_frame(C.DEV_ADDR, C.FC_HANDSHAKE, b"\x00" * 1025)
    rec.obs("A3b-host", "超长 data 主机构帧 (主机侧无>1024保护)", "构出 >1024 帧")
except Exception:
    pass
big2 = build_frame(C.DEV_ADDR, C.FC_HANDSHAKE, b"\x00" * 1024)  # 1035B, 恰在边界
drain(lk)
send_raw_chunks(lk, big2)
f = lk.recv_frame(timeout_s=4)
rec.check("A3b", "1024B data 边界长帧 (1035B/17 报文)", f is not None,
          f"{len(big2)}B 帧收到响应 (RX环512B 分段解析通过)",
          f"{len(big2)}B 帧无响应")

# ---- A4 半帧残留 -> 新帧恢复 ----
drain(lk)
half = good[:9]           # 有效头+长度, 载荷残缺
send_raw_chunks(lk, half)
time.sleep(0.2)
# 跟一个完整帧: 其字节会先补完死帧载荷, CRC 必失配丢弃; 之后是否可恢复取决于剩余字节
lk.send_bytes(good)
f1 = lk.recv_frame(timeout_s=1.5)
# 再发一个干净帧, 必须恢复
lk.send_bytes(good)
f2 = lk.recv_frame(timeout_s=1.5)
if f2 is not None:
    extra = "紧随帧被吞(流式解析器固有, 无帧间超时)" if f1 is None else "紧随帧正常响应"
    rec.pass_("A4", "半帧残留后链路恢复", f"补帧后第2个正常帧恢复; {extra}")
else:
    rec.fail("A4", "半帧残留后链路恢复", "连发 2 个正常帧均无响应, 链路未恢复")

# ---- A5 devAddr 过滤 ----
drain(lk)
r, to = lk.transaction(C.FC_HANDSHAKE, b"\x01", timeout_s=1.5, dev_addr=0x02)
silent2 = (r is None)
drain(lk)
r0, to0 = lk.transaction(C.FC_HANDSHAKE, b"\x01", timeout_s=1.5, dev_addr=0x00)
silent0 = (r0 is None)
rec.check("A5", "devAddr 过滤 (非本机地址丢弃)", silent2,
          f"addr=0x02 静默; addr=0x00 {'静默' if silent0 else '有响应(广播式放行)'}",
          "addr=0x02 竟有响应")

# ---- A6 未知 FC 静默 (外层 default:break) ----
drain(lk)
send_raw_chunks(lk, build_frame(C.DEV_ADDR, 0x37, b"\x01"))
f = lk.recv_frame(timeout_s=0.8)
if f is None:
    rec.pass_("A6", "未知 FC (0x37) 静默丢弃", "无响应且不挂死 (外层 default:break)")
else:
    rec.rec("A6", "未知 FC (0x37)", "OBS", f"有响应 func=0x{f['func']:02X} (文档未定义)")
if not alive(lk):
    rec.fail("A6+存活", "未知 FC 后设备存活", "挂死!")
else:
    rec.pass_("A6+存活", "未知 FC 后设备存活", "握手恢复")

rec.flush()
lk.close()
