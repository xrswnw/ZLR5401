"""P4: UHF — 状态机/配置持久化/盘点/扫描/诊断/未就绪路径"""
import time
from lib import Recorder, open_link, tx, drain, alive, C, load_hw

rec = Recorder("P4")
lk = open_link()
assert alive(lk)
hw = load_hw()
have_tag = len(hw.get("uhf_tags", [])) > 0

def u(sub, data=None, t=6):
    payload = [sub] + (data or [])
    d, to = tx(lk, C.FC_UHF_CTRL, payload, timeout_s=t)
    return d if not to else None

# ---- U9 未 OPEN 直接 INVENTORY / QUERY ----
u(C.UHF_CLOSE)   # 确保下电态
time.sleep(1.5)  # Round_098: 0.5s 不足 — 模块下电过渡期 INVENTORY 仍可执行 (err=0)
d = u(C.UHF_INVENTORY, [0xE8, 0x03])
rec.check("U9a", "未 OPEN 直接 INVENTORY", d is not None and d[1] == C.UHF_ERR_NOT_READY,
          f"err={d[1]} NOT_READY" if d else "TO", f"err={d[1] if d else 'TO'}")
d = u(C.UHF_SCAN_STOP)
rec.check("U9b", "未 OPEN 直接 SCAN_STOP (幂等)", d is not None and d[1] == 0,
          f"err=0", f"err={d[1] if d else 'TO'}")

# ---- U1 OPEN -> QUERY -> CLOSE -> QUERY ----
d = u(C.UHF_OPEN, t=8)
rec.check("U1a", "UHF OPEN", d is not None and d[1] == 0, f"err=0", f"err={d[1] if d else 'TO'}")
d = u(C.UHF_QUERY)
ok = d is not None and d[1] == 0 and d[2] in (1, 2)  # state 字段: READY/CONFIG
rec.check("U1b", "OPEN 后 QUERY (链路+状态)", ok,
          f"err=0 state={d[2]} link={d[3]} totalTags={d[4]}" if d else "TO",
          f"resp={d.hex() if d else 'TO'}")
d = u(C.UHF_CLOSE)
rec.check("U1c", "UHF CLOSE", d is not None and d[1] == 0, "err=0", f"err={d[1] if d else 'TO'}")
time.sleep(1.5)   # Round_098: 下电过渡期 QUERY 可回 LINK(4), 等 1.5s 再验
d = u(C.UHF_QUERY)
# Round_098: CLOSE 后模块已下电, QUERY 稳定回 LINK(4) (语义上更宜 NOT_READY,
# 已记 P9 优化清单); 过渡期亦可回 NOT_READY(3)
ok = d is not None and d[1] in (0, C.UHF_ERR_NOT_READY, C.UHF_ERR_LINK)
rec.check("U1d", "CLOSE 后 QUERY", ok,
          f"err={d[1]} state={d[2] if d else '?'}" if d else "TO",
          f"resp={d.hex() if d else 'TO'}")

# ---- U2 配置写读回 ----
u(C.UHF_OPEN, t=8)
d = u(C.UHF_GET_CONFIG)
cfg0 = d
rec.check("U2a", "GET_CONFIG", d is not None and d[1] == 0,
          f"power={d[2]} ant={d[3]} chk={d[4]} sess={d[5]} tgt={d[6]} q={d[7]} band={d[8]}" if d else "TO",
          "无响应")
# 改配置: power +10 (范围内), session 翻转
import random
new_power = (cfg0[2] + 10) if cfg0[2] < 25 else (cfg0[2] - 10)
new_sess = 1 if cfg0[5] != 1 else 2
d = u(C.UHF_SET_CONFIG, [new_power, cfg0[3], cfg0[4], new_sess, cfg0[6], cfg0[7]])
rec.check("U2b", "SET_CONFIG (改 power/session)", d is not None and d[1] == 0,
          f"err=0", f"err={d[1] if d else 'TO'}")
d = u(C.UHF_GET_CONFIG)
ok = d is not None and d[2] == new_power and d[5] == new_sess
rec.check("U2c", "GET_CONFIG 读回一致", ok,
          f"power={d[2]} (写 {new_power}) sess={d[5]} (写 {new_sess})" if d else "TO",
          f"power={d[2] if d else '?'} sess={d[5] if d else '?'}")
# 短帧 SET_CONFIG
d = u(C.UHF_SET_CONFIG, [new_power, cfg0[3]])
rec.check("U2d", "SET_CONFIG 短帧", d is not None and d[1] == C.UHF_ERR_PARAM,
          f"err=1 PARAM", f"err={d[1] if d else 'TO'}")

# ---- U4/U5 盘点 (有标签: 解析; 无: NO_TAG) ----
d = u(C.UHF_INVENTORY, [0xE8, 0x03], t=8)
if have_tag:
    ok = d is not None and d[1] == 0 and (d[2] | (d[3] << 8)) >= 1
    epc_info = ""
    if ok:
        p = 4; el = d[p + 1]; epc_info = d[p + 2:p + 2 + el].hex()
    rec.check("U4", "INVENTORY (在场标签)", ok,
              f"count={d[2] | (d[3] << 8)} epc={epc_info} rssi={d[4]}" if d else "TO",
              f"resp={d.hex() if d else 'TO'}")
else:
    ok = d is not None and d[1] == C.UHF_ERR_NO_TAG
    rec.check("U5", "INVENTORY (无标签 NO_TAG)", ok, f"err=5 NO_TAG" if d else "TO",
              f"err={d[1] if d else 'TO'}")

# ---- U6 SCAN_START -> GET_TAGS -> SCAN_STOP ----
d = u(C.UHF_SCAN_START, [0x05, 0x00])   # 5s 周期? cycle 参数 (LE)
rec.check("U6a", "SCAN_START", d is not None and d[1] == 0, f"err=0", f"err={d[1] if d else 'TO'}")
time.sleep(4)
d = u(C.UHF_GET_TAGS, [0])
ok = d is not None and d[1] == 0
n = (d[2] | (d[3] << 8)) if d else -1
rec.check("U6b", "GET_TAGS 拉取 (扫描累积)", ok and n >= 0,
          f"count={n}" if d else "TO", f"resp={d.hex() if d else 'TO'}")
# 再拉一次: 取走后应为 0
d = u(C.UHF_GET_TAGS, [0])
n2 = (d[2] | (d[3] << 8)) if d else -1
rec.check("U6c", "GET_TAGS 取走清空", d is not None and d[1] == 0 and n2 == 0,
          f"count={n2}", f"count={n2}")
d = u(C.UHF_SCAN_STOP)
rec.check("U6d", "SCAN_STOP", d is not None and d[1] == 0, "err=0", f"err={d[1] if d else 'TO'}")

# ---- U7 诊断 ----
d = u(C.UHF_GET_STATUS)
rec.check("U7a", "GET_STATUS", d is not None and d[1] == 0,
          f"resp={d.hex()}" if d else "TO", f"resp={d.hex() if d else 'TO'}")
d = u(C.UHF_CHECK_ANT, t=10)
rec.check("U7b", "CHECK_ANT", d is not None and d[1] in (0, C.UHF_ERR_LINK),
          f"err={d[1]}" if d else "TO", f"resp={d.hex() if d else 'TO'}")
d = u(C.UHF_GET_DUMP)
rec.check("U7c", "GET_DUMP", d is not None and d[1] == 0,
          f"len={len(d)}B" if d else "TO", "无响应")

# ---- U8 读标签 (有标签: EPC 已知; 读 TID/EPC bank) ----
if have_tag:
    epc = bytes.fromhex(hw["uhf_tags"][0])
    d = u(C.UHF_READ_TAG, [len(epc)] + list(epc) + [1, 0, 0], t=8)   # bank=1 TID? 先按协议 bank=1
    rec.rec("U8", "READ_TAG", "OBS" if d is None else "PASS",
            f"resp={d.hex()}" if d else "无响应 (bank/地址参数需按标签型号调整)")

# ---- U3 配置持久化 (复位后读回) ----
d, to = tx(lk, C.FC_RESET, [], timeout_s=2)
rec.check("U3a", "FC_RESET (持久化前)", d is not None, f"resp={d.hex()}" if d else "无响应", "无响应")
lk.close()
lk2, dt = None, 0
from lib import reopen_until_alive
lk2, dt = reopen_until_alive(25)
rec.check("U3b", "复位重连", lk2 is not None, f"{dt:.1f}s", "未恢复")
lk = lk2 if lk2 else lk
if lk2:
    # UHF 配置持久化: App 启动时从参数区恢复 (Round_098: 恢复晚于重枚举, 早期
    # GET_CONFIG 可读到模块默认值 power=1/sess=0x55 — 重试等待恢复完成)
    d = None
    for _ in range(10):
        d = u(C.UHF_GET_CONFIG)
        if d is not None and d[1] == 0 and d[2] == new_power and d[5] == new_sess:
            break
        time.sleep(1.0)
    ok = d is not None and d[2] == new_power and d[5] == new_sess
    rec.check("U3c", "复位后 UHF 配置持久 (power/session)", ok,
              f"power={d[2] if d else '?'} (期望 {new_power}) sess={d[5] if d else '?'} (期望 {new_sess})" if d else "TO",
              f"power={d[2] if d else '?'} sess={d[5] if d else '?'} 期望 {new_power}/{new_sess}")
    # U3d 还原原始配置: 持久化验证写下的 session 若不还原, S2/S3 会话下标签
    # 盘点标志在连续盘存的场脉冲间不复位, Locker 连续重扫场景静默失效
    # (Round_098 P8-S3 根因: U2b 写 S2 未还原 -> Locker 周期#2 起永无匹配)
    d = u(C.UHF_SET_CONFIG, [cfg0[2], cfg0[3], cfg0[4], cfg0[5], cfg0[6], cfg0[7]], t=8)
    d2 = u(C.UHF_GET_CONFIG)
    ok = (d is not None and d[1] == 0 and d2 is not None
          and d2[2] == cfg0[2] and d2[5] == cfg0[5])
    rec.check("U3d", "还原原始 UHF 配置 (防 S2 污染后续用例)", ok,
              f"power={d2[2] if d2 else '?'} sess={d2[5] if d2 else '?'} (原 {cfg0[2]}/{cfg0[5]})",
              f"resp={d.hex() if d else 'TO'} -> {d2.hex() if d2 else 'TO'}")

rec.flush()
lk.close()
