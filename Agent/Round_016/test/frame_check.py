#!/usr/bin/env python3
"""Round_016 帧级离线验证 (无需真机):
  1. 新 0x10 请求帧构造 (tmo + epcCnt + (len,EPC)×n 变长) + CRC 往返
  2. 0x11 帧构造; F5 (v3): 0x09 九字节位图应答解码 + 速查值
  3. 边界断言: epcCnt 上限 10 / 重复 EPC / tmo=0 / 装配长度
用法: python3 frame_check.py
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "Round_008", "test"))
from zlr.frame import build_frame, parse_frame
from zlr.crc32 import crc32

FC_LOCKER = 0x23
SUB_UNLOCK_EPC = 0x10
SUB_UNLOCK_AM = 0x11
SUB_GET_PROGRESS = 0x09
SUB_CANCEL = 0x04

fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print(f"  FAIL: {msg}")

def locker(sub, data):
    return build_frame(0x01, FC_LOCKER, bytes([sub]) + bytes(data))

def epc10(tmo_sec, epcs, epc_cnt=None):
    """0x10 请求: [0x10, tmoL, tmoH, epcCnt, (len, EPC...)×n]"""
    body = bytes([SUB_UNLOCK_EPC, tmo_sec & 0xFF, tmo_sec >> 8,
                  len(epcs) if epc_cnt is None else epc_cnt])
    for e in epcs:
        body += bytes([len(e)]) + e
    return locker_raw(body)

def locker_raw(body):
    return build_frame(0x01, FC_LOCKER, body)

A = bytes.fromhex("E2000000000000000000A001")
B = bytes.fromhex("E2000000000000000000B002")
C = bytes([0x11] * 11)
D = bytes.fromhex("E2000000000000000000D004")

print("== F1 0x10 新帧构造与解析 ==")
f = epc10(60, [A, B, C])
p, rest = parse_frame(f)
check(p is not None and rest == b"", "帧可解析且无残余")
check(p["func"] == FC_LOCKER and p["data"][0] == SUB_UNLOCK_EPC, "FC/子命令")
d = p["data"]
check(d[1] | (d[2] << 8) == 60, "tmo=60s")
check(d[3] == 3, "epcCnt=3")
pos = 4
for i, e in enumerate([A, B, C]):
    ln = d[pos]; pos += 1
    check(ln == len(e) and d[pos:pos+ln] == e, f"第{i+1}张 EPC 变长装配")
    pos += ln
check(pos == len(d), "无多余字节")
check(crc32(f[:-4]) == int.from_bytes(f[-4:], "little"), "CRC-32/MPEG-2 往返")

print("== F2 边界长度 ==")
check(len(epc10(1, [bytes(12)] * 10)) < 64 + 64 + 64, "10 张 12B EPC 三包内")
check(all(len(e) <= 12 for e in [A, B, C, D]), "EPC 均 ≤12B")

print("== F3 0x09 / 0x11 / CANCEL 帧构造 ==")
for sub, body in [(SUB_GET_PROGRESS, []),
                  (SUB_UNLOCK_AM, [3, 120 & 0xFF, 120 >> 8, 0, 0]),
                  (SUB_CANCEL, [])]:
    f = locker(sub, body)
    p, _ = parse_frame(f)
    check(p and p["data"][0] == sub, f"子命令 {sub:#04x} 装配")

print("== F4 响应 FC 派生 ==")
resp_fc = FC_LOCKER ^ 0xFF
check(resp_fc == 0xDC, "响应 FC = 0x23^0xFF")


print("== F5 v3 0x09 九字节 statusBits 应答 ==")
# 模拟应答体: [cmd,err,status,tag,elapsed,riseCnt,alarmCnt,lastCycle,rsv]
def resp09(status, tag=0, elapsed=0, rise=0, alarm=0, cyc=0):
    return bytes([0x09, 0x00, status, tag, elapsed, rise, alarm, cyc, 0x00])

f = locker(SUB_GET_PROGRESS, [])
# 例值: 在顶保持 0x15 / 介入快降 0x79 / 全失快降 0x59 / 超窗回降 0x1D / 空闲 0x00
idle  = resp09(0x00)
up    = resp09(0x15, 2, 12, 1, 0, 1)
alarm = resp09(0x79, 2, 14, 1, 1, 1)
lost  = resp09(0x59, 0, 12, 1, 1, 2)
norm  = resp09(0x1D, 2, 30, 2, 1, 1)
check(len(idle) == 9 and len(up) == 9, "0x09 应答 9 字节")
def bits(b, mask): return (b[2] & mask) == mask
check(bits(alarm, 0x20) and bits(alarm, 0x40) and (alarm[2] >> 3) & 3 == 3, "0x79 = FGN+ALARM+下行")
check(not bits(lost, 0x20) and bits(lost, 0x40) and (lost[2] >> 3) & 3 == 3, "0x59 = 无外来+报警+下行 (全失)")
check(not bits(norm, 0x40) and (norm[2] >> 3) & 3 == 3, "0x1D = 下行无报警 (超窗正常回降)")
check(idle[2] == 0, "空闲 status=0x00")
check(up[7] == 1 and up[5] == 1, "lastCycle/riseCnt 槽位")
# 0x0F 受理帧 status 字节: 0x10=0x01, 0x11=0x05
check(bytes([0x0F, 0x00, 0x01, 0x10, 0x27, 0x00])[2] == 0x01, "0x10 受理 status=0x01")
check(bytes([0x0F, 0x00, 0x05, 0x60, 0xEA, 0x00])[2] == 0x05, "0x11 受理 status=0x05")

print(f"\nframe_check: {'ALL OK' if fails == 0 else f'{fails} FAILURES'}")
sys.exit(1 if fails else 0)
