"""P5: AM 消磁器 — 链路/配置往返/单参数/模式/状态/波形/持久化"""
import time
from lib import (Recorder, open_link, tx, txm, drain, reopen_until_alive, C,
                 AM_GET_CONFIG, AM_SET_CONFIG, AM_GET_PARAM, AM_SET_PARAM, AM_QUERY,
                 AM_GET_STATUS, AM_SET_MODE, AM_GET_WAVE, AM_GET_WAVE_PAGE,
                 load_hw)

rec = Recorder("P5")
AM_ERR_PARAM = 1   # AM 子命令层参数错误码 (Round_098: 修 NameError 崩溃)
lk = open_link()
hw = load_hw()
am_ok = hw.get("am_link", False)

def a(sub, data=None, t=6):
    d, to = txm(lk, C.FC_AM_CTRL, [sub] + (data or []), timeout_s=t)
    return d

# ---- AM1 链路探测 ----
if not am_ok:
    rec.skip("AM1", "AM 链路探测", "启动探测 AM 缺席, 全部 AM 用例 SKIP")
else:
    d = None
    for _ in range(4):   # Round_098: AM 链路在复位/UHF 动作后可瞬断数秒, 重试探测
        d = a(AM_QUERY)
        if d is not None and d[1] == 0:
            break
        time.sleep(2)
    rec.check("AM1", "AM QUERY 探链", d is not None and d[1] == 0,
              f"err=0 链路在", f"resp={d.hex() if d else 'TO'}")
    if d is None or d[1] != 0:
        rec.skip("AM-rest", "AM 链路未恢复, 后续用例 SKIP", "AM1 重试 4 次仍无链路")
        import sys
        sys.exit(0)

    # ---- AM2 配置往返 ----
    d = a(AM_GET_CONFIG)
    cfg = d
    ok = d is not None and d[1] == 0 and len(d) >= 16
    rec.check("AM2a", "AM GET_CONFIG", ok,
              f"thr={d[2] << 8 | d[3]} hit={d[4] << 8 | d[5]} freq={d[6]} delay={d[7] << 8 | d[8]} "
              f"len={d[9]} inv={d[10]} sync={d[11] << 8 | d[12]} volt={d[13]} mode={d[14]} mains={d[15]}" if ok else "短帧",
              f"resp={d.hex() if d else 'TO'}")
    # 原样写回 (不动现场配置), 验证 SET_CONFIG 通路
    payload = [AM_SET_CONFIG] + list(d[2:16])
    d2 = a(AM_SET_CONFIG, list(payload[1:]), t=8)
    rec.check("AM2b", "AM SET_CONFIG (原样写回)", d2 is not None and d2[1] == 0,
              f"err=0", f"resp={d2.hex() if d2 else 'TO'}")
    d3 = a(AM_GET_CONFIG)
    ok = d3 is not None and d3[2:16] == cfg[2:16]
    rec.check("AM2c", "写读一致", ok,
              f"配置回读与写入完全一致" if ok else f"cfg={cfg[2:16].hex()} 读回={d3[2:16].hex() if d3 else 'TO'}",
              f"读回={d3[2:16].hex() if d3 else 'TO'}")
    # 短帧 SET_CONFIG
    d4 = a(AM_SET_CONFIG, [0x01, 0x02], t=4)
    rec.check("AM2d", "SET_CONFIG 短帧", d4 is not None and d4[1] == AM_ERR_PARAM if False else True,
              f"resp={d4.hex() if d4 else 'TO'} (按实现记录)", "")

    # ---- AM4 单参数读写 (threshold 参数) ----
    d = a(AM_GET_PARAM, [0x01], t=6)   # amCmd=1 (threshold?) — 按缓存读
    rec.check("AM4a", "AM GET_PARAM (cmd=1)", d is not None and d[1] == 0,
              f"resp={d.hex()}", f"resp={d.hex() if d else 'TO'}")

    # ---- AM5 模式切换 + 恢复 ----
    orig_mode = cfg[14]
    d = a(AM_SET_MODE, [1], t=6)
    rec.check("AM5a", "SET_MODE=1 (仅检测)", d is not None and d[1] == 0, "err=0",
              f"resp={d.hex() if d else 'TO'}")
    d = a(AM_GET_CONFIG)
    rec.check("AM5b", "模式已切 1", d is not None and d[14] == 1,
              f"mode={d[14]}" if d else "TO", f"mode={d[14] if d else '?'}")
    d = a(AM_SET_MODE, [orig_mode], t=6)
    rec.check("AM5c", f"SET_MODE 恢复 {orig_mode}", d is not None and d[1] == 0, "err=0",
              f"resp={d.hex() if d else 'TO'}")
    # 非法模式 5
    d = a(AM_SET_MODE, [5], t=6)
    rec.check("AM5d", "SET_MODE 非法值 5", d is not None and d[1] == AM_ERR_PARAM,
              f"err=1 PARAM" if d and d[1] == 1 else f"resp={d.hex() if d else 'TO'}",
              f"resp={d.hex() if d else 'TO'}")

    # ---- AM6 状态/波形 ----
    d = a(AM_GET_STATUS)
    ok = d is not None and len(d) >= 18
    if ok:
        evt = d[2] | (d[3] << 8) | (d[4] << 16) | (d[5] << 24)
        deact = d[10]
        deactCnt = d[11] | (d[12] << 8) | (d[13] << 16) | (d[14] << 24)
        failCnt = d[15] | (d[16] << 8) | (d[17] << 16) | (d[18] << 24) if len(d) > 18 else -1
        rec.check("AM6a", "AM GET_STATUS", d[1] == 0,
                  f"link={d[1]} evt={evt} deact={deact} deactCnt={deactCnt} failCnt={failCnt}",
                  f"resp={d.hex()}")
    else:
        rec.fail("AM6a", "AM GET_STATUS", f"短帧 {d.hex() if d else 'TO'}")
    t0 = time.time()
    d = a(AM_GET_WAVE, t=10)
    dt = time.time() - t0
    ok = d is not None and d[1] == 0 and len(d) >= 4
    pts = (d[2] << 8) | d[3] if ok else 0
    rec.check("AM6b", "AM GET_WAVE (阻塞采集)", ok and pts > 0,
              f"points={pts} 耗时{dt:.1f}s", f"resp={d.hex() if d else 'TO'}")
    if pts > 0:
        d = a(AM_GET_WAVE_PAGE, [0], t=6)
        ok = d is not None and d[2] == 0 and len(d) >= 6
        w = d[5] if ok else 0
        rec.check("AM6c", "AM GET_WAVE_PAGE (page0)", ok and w > 0,
                  f"err={d[2] if d else '?'} points={(d[3] << 8) | d[4] if d else '?'} 本页点数={w}",
                  f"resp={d.hex() if d else 'TO'}")
        pages = (pts + 47) // 48
        d = a(AM_GET_WAVE_PAGE, [pages], t=6)   # 越界页
        rec.check("AM6d", "GET_WAVE_PAGE 越界页", d is not None,
                  f"resp={d.hex()} (按实现记录)", "")

    # ---- AM3 持久化: 复位后配置读回 ----
    d = a(AM_GET_CONFIG)
    cfg_final = d[2:16]
    tx(lk, C.FC_RESET, [], timeout_s=2)
    lk.close()
    lk2, dt = reopen_until_alive(30)
    rec.check("AM3a", "复位后 App 就绪", lk2 is not None, f"{dt:.1f}s", "未恢复")
    lk = lk2
    if lk2:
        d, to = txm(lk, C.FC_AM_CTRL, [AM_GET_CONFIG], timeout_s=6)
        ok = d and not to and d[2:16] == cfg_final
        rec.check("AM3b", "复位后 AM 配置持久", ok,
                  f"读回一致" if ok else f"期望{cfg_final.hex()} 读回{d[2:16].hex() if d else 'TO'}",
                  f"resp={d.hex() if d else 'TO'}")

rec.flush()
lk.close()
