"""P2: 基础功能 — 握手/信息/RGB/自检/复位"""
import time
from lib import (Recorder, open_link, tx, drain, alive, reopen_until_alive, C,
                 FC_SELFTEST_CTRL, SELFTEST_SUB_QUERY, SELFTEST_SUB_RERUN,
                 SELFTEST_SUB_CLEAR, bits_str, parse_selftest)

rec = Recorder("P2")
lk = open_link()
assert alive(lk), "设备不存活"

# ---- B1 握手 x100 连发 ----
ok_cnt, to_cnt, t0 = 0, 0, time.time()
lat = []
for _ in range(100):
    d, to = tx(lk, C.FC_HANDSHAKE, [0x01], timeout_s=2)
    if to:
        to_cnt += 1
    elif d[0] == 0x00:
        ok_cnt += 1
        lat.append(time.time())
dt = time.time() - t0
rec.check("B1", "握手 x100 连发", ok_cnt == 100,
          f"{ok_cnt}/100 OK, 总耗时{dt:.1f}s (均 {dt/100*1000:.0f}ms)",
          f"OK={ok_cnt} 超时={to_cnt}")

# ---- B2 设备信息 ----
d, to = tx(lk, C.FC_DEVICE_INFO, [], timeout_s=2)
if d and not to:
    s = bytes(x for x in d if 32 <= x < 127).decode("ascii", "replace")
    hw_name = d[2:2 + 8].split(b"\x00")[0].decode("ascii", "replace")
    fw_name = d[13:13 + 10].split(b"\x00")[0].decode("ascii", "replace")
    rec.check("B2", "设备信息字段", True,
              f"raw={d.hex()} 硬件={hw_name} 固件={fw_name}")
    if "C8T6" in hw_name:
        rec.obs("B2-obs", "DEVICE_INFO 硬件名与实际 MCU (GD32F303RCT6) 不一致",
                f"硬件名={hw_name} (疑似早期板名遗留, 建议更正)")
else:
    rec.fail("B2", "设备信息字段", "无响应")

# ---- B3 RGB 控制 ----
results = []
for mask in range(1, 8):
    d, to = tx(lk, C.FC_RGB_CTRL, [0x01, mask, 0x00], timeout_s=2)
    results.append(d and not to and d[1] == 0 and d[2] == mask)
rec.check("B3a", "RGB SET mask=1..7 全组合 (空闲)", all(results),
          "全部 OK+回显" if all(results) else f"{sum(results)}/7",
          f"仅 {sum(results)}/7 通过")
d, to = tx(lk, C.FC_RGB_CTRL, [0x01, 0x00, 0x00], timeout_s=2)
rec.check("B3b", "RGB mask=0 撤销", d and not to and d[1] == 0,
          "OK", f"resp={d.hex() if d else 'TO'}")
d, to = tx(lk, C.FC_RGB_CTRL, [0x01, 0x08, 0x00], timeout_s=2)  # 非法 bit3
rec.check("B3c", "RGB 非法 mask=0x08 (仅 G/R/B 位有效)", d and not to and d[1] in (0, 1),
          f"err={d[1] if d else 'TO'} (回显 mask={d[2] if d else '?'})",
          f"resp={d.hex() if d else 'TO'}")
d, to = tx(lk, C.FC_RGB_CTRL, [0x01, 0x01], timeout_s=2)  # 短帧
rec.check("B3d", "RGB 短帧 (缺 reserved)", d and not to and d[1] == 1,
          f"err=1 PARAM", f"resp={d.hex() if d else 'TO'}")

# B3e: Locker CONFIGURED (未 START) 时 RGB 应 BUSY
tx(lk, C.FC_LOCKER_CTRL, [C.LOCKER_CONFIGURE, 0, 0, 0, 1])   # 纯软标任务, 进入 CONFIGURED
time.sleep(0.2)
d, to = tx(lk, C.FC_RGB_CTRL, [0x01, 0x01, 0x00], timeout_s=2)
rec.check("B3e", "RGB SET 于 Locker CONFIGURED 态", d and not to and d[1] == 2,
          "err=2 BUSY (配置未激活也拒绝, 保守正确)" if d and d[1] == 2 else f"err={d[1] if d else 'TO'}",
          f"err={d[1] if d else 'TO'} (期望 2)")
tx(lk, C.FC_LOCKER_CTRL, [C.LOCKER_CANCEL], timeout_s=6)
time.sleep(0.3)

# ---- B4 自检 ----
d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_QUERY], timeout_s=3)
st = parse_selftest(d if not to else None)
rec.check("B4a", "SELFTEST QUERY 基线", st is not None and st["err"] == 0,
          f"errBits=0x{st['errBits']:04X}({bits_str(st['errBits'])})" if st else "无响应",
          f"resp={d.hex() if d else 'TO'}")
t0 = time.time()
d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_RERUN], timeout_s=8)
dt = time.time() - t0
ok = (not to) and d is not None and d[1] == 0
rec.check("B4b", "SELFTEST RERUN (空闲, 阻塞探测)", ok,
          f"err=0 errBits=0x{d[2] | (d[3] << 8):04X} 探测耗时{dt:.1f}s" if ok
          else f"resp={d.hex() if d else 'TO'}",
          f"resp={d.hex() if d else 'TO'}")
# RERUN 于 Locker 忙: BUSY
tx(lk, C.FC_LOCKER_CTRL, [C.LOCKER_CONFIGURE, 0, 0, 0, 1])
time.sleep(0.2)
d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_RERUN], timeout_s=4)
rec.check("B4c", "SELFTEST RERUN 于 CONFIGURED 态", d and not to and d[1] == 2,
          f"err=2 BUSY" if d and d[1] == 2 else f"err={d[1] if d else 'TO'}",
          f"resp={d.hex() if d else 'TO'}")
tx(lk, C.FC_LOCKER_CTRL, [C.LOCKER_CANCEL], timeout_s=6)
time.sleep(0.3)
# ---- B4d (BUG#1 已修复: 文档布局 [cmd,maskL,maskH] 应可用) ----
d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_CLEAR, 0x3F, 0x00], timeout_s=3)
ok = (not to) and d is not None and len(d) >= 4 and d[1] == 0
rec.check("B4d", "SELFTEST CLEAR 文档布局 [cmd,maskL,maskH]", ok,
          f"清后 errBits=0x{d[2] | (d[3] << 8):04X}" if ok else f"resp={d.hex() if d else 'TO'}",
          f"resp={d.hex() if d else 'TO'} (BUG#1 修复失效)")
# 未知子命令
d, to = tx(lk, FC_SELFTEST_CTRL, [0x77], timeout_s=2)
rec.check("B4e", "SELFTEST 未知子命令", d and not to and d[1] == 1,
          f"err=1 PARAM" if d and d[1] == 1 else f"err={d[1] if d else 'TO'}",
          f"resp={d.hex() if d else 'TO'}")

# ---- B5 软件复位 (破坏性, 独立收尾) ----
lk.close()
lk = open_link()
d, to = tx(lk, C.FC_RESET, [], timeout_s=2)
rec.check("B5a", "FC_RESET 回帧", d is not None and not to,
          f"resp={d.hex()}" if d else "无响应 (复位太快吞帧?)",
          "无响应")
lk.close()
lk, dt = reopen_until_alive(25)
rec.check("B5b", "复位后重枚举+存活 (25s 内, 含回零阻塞)", lk is not None,
          f"重连耗时 {dt:.1f}s",
          "25s 未恢复")
if lk is not None:
    d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_QUERY], timeout_s=3)
    st = parse_selftest(d if not to else None)
    rec.check("B5c", "复位后自检基线 (回零重建)", st is not None and st["errBits"] == 0,
              f"errBits=0x{st['errBits']:04X}({bits_str(st['errBits'])}) switchErr={st['switchErr']}" if st else "无响应",
              f"st={st}")
    # Round_098: 复位后 App 启动即回零 (电机 SEEK), 需等回零完成才 IDLE
    d, to = None, True
    for _ in range(20):   # 最长 ~20s 等回零
        d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_QUERY], timeout_s=2)
        if d and not to and d[2] == 0:
            break
        time.sleep(1.0)
    rec.check("B5d", "复位后电机 IDLE (回零完成)", d and not to and d[2] == 0,
              f"state={d[2] if d else 'TO'}",
              f"state={d[2] if d else 'TO'} (期望 0)")

rec.flush()
lk.close()
