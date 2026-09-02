"""P3: 电机控制 — 参数域/运动/健康统计/行程测试"""
import time
from lib import Recorder, open_link, tx, drain, alive, C, bits_str, \
    FC_SELFTEST_CTRL, SELFTEST_SUB_QUERY, parse_selftest

rec = Recorder("P3")
lk = open_link()
assert alive(lk)

def mquery():
    d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_QUERY], timeout_s=2)
    return d if not to else None

def wait_idle(timeout_s=15):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        d = mquery()
        if d and d[2] == 0:      # state 字段
            return True
        time.sleep(0.2)
    return False

# 前置: 确认 IDLE + 回零就绪
d = mquery()
rec.check("M0", "电机初始 IDLE", d and d[2] == 0, f"state={d[2] if d else 'TO'}",
          f"state={d[2] if d else 'TO'}")

# ---- M1 SPEED/TORQUE 参数域 ----
cases = [
    ("M1a SPEED=1 (下界)", C.MOTOR_SPEED, [1, 0], 0),
    ("M1b SPEED=2000 (上界)", C.MOTOR_SPEED, [0xD0, 0x07], 0),
    ("M1c SPEED=0 (非法)", C.MOTOR_SPEED, [0, 0], 1),
    ("M1d SPEED=2001 (非法)", C.MOTOR_SPEED, [0xD1, 0x07], 1),
    ("M1e TORQUE=6 (下界)", C.MOTOR_TORQUE, [6], 0),
    ("M1f TORQUE=100 (上界)", C.MOTOR_TORQUE, [100], 0),
    ("M1g TORQUE=5 (非法)", C.MOTOR_TORQUE, [5], 1),
    ("M1h TORQUE=101 (非法)", C.MOTOR_TORQUE, [101], 1),
]
for name, sub, param, want in cases:
    d, to = tx(lk, C.FC_MOTOR_CTRL, [sub] + param, timeout_s=2)
    got = d[1] if d and not to else -1
    rec.check(name, "", got == want, f"err={got}", f"err={got} 期望 {want}")

# SPEED 读回: QUERY 帧里有没有速度字段? 用 SET 后 SET 回显确认 — 详见实现
# (MOTOR_QUERY 响应布局: cmd,err,state,fault,steps(3)); 速度不回读, 由回显判断

# ---- M2 MOVE 小步数 + STOP ----
tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_SPEED, 0xD0, 0x07])   # 2000 Hz
tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_TORQUE, 40])
base = mquery()
steps0 = base[6] | (base[7] << 8) if base and len(base) > 7 else 0
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_MOVE, 0x00, 100, 0, 0], timeout_s=2)  # 正转 100 微步
rec.check("M2a", "MOVE 100 微步 (dir=0)", d and not to and d[1] == 0,
          f"err=0", f"resp={d.hex() if d else 'TO'}")
time.sleep(0.5)
ok = wait_idle(5)
d = mquery()
steps1 = d[6] | (d[7] << 8) if d and len(d) > 7 else -1
rec.check("M2b", "MOVE 后自动到 IDLE (100步@2000Hz)", ok and d[2] == 0,
          f"IDLE, steps16={steps1} (Δ={(steps1-steps0)&0xFFFF})", f"state={d[2] if d else 'TO'} steps={steps1}")

d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_MOVE, 0x01, 200, 0, 0], timeout_s=2)  # 反转 200
rec.check("M2c", "MOVE 200 微步 (dir=1)", d and not to and d[1] == 0, "err=0",
          f"resp={d.hex() if d else 'TO'}")
ok = wait_idle(5)
d = mquery()
steps2 = d[6] | (d[7] << 8) if d and len(d) > 7 else -1
rec.check("M2d", "反向 MOVE 完成", ok, f"steps16={steps2} (Δ={(steps2-steps1)&0xFFFF})", "超时未 IDLE")

# MOVE 期间再 MOVE -> BUSY
tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_MOVE, 0x00, 4000, 0, 0])       # 4000 步 ~2s @2000Hz
time.sleep(0.3)
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_MOVE, 0x00, 100, 0, 0], timeout_s=2)
rec.check("M3a", "MOVE 进行中再 MOVE", d and not to and d[1] in (2, 3),
          f"err={d[1]} ({'BUSY' if d[1] == 3 else 'FAULT=拒绝重复启动'})",
          f"err={d[1] if d else 'TO'} (期望 2/3)")
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_TEST, 1], timeout_s=2)
rec.check("M3b", "MOVE 进行中下发 TEST", d and not to and d[1] == 3,
          f"err=3 BUSY" if d and d[1] == 3 else f"err={d[1] if d else 'TO'}",
          f"err={d[1] if d else 'TO'}")
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_STOP], timeout_s=2)
rec.check("M2e", "STOP 停止", d and not to and d[1] == 0, "err=0",
          f"resp={d.hex() if d else 'TO'}")
ok = wait_idle(5)
rec.check("M2f", "STOP 后 IDLE", ok, "OK", "未 IDLE")

# ---- M4 QUERY/HEALTH/STATS ----
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_QUERY], timeout_s=2)
rec.check("M4a", "QUERY 字段", d and not to and d[1] == 0, f"resp={d.hex()}", f"resp={d.hex() if d else 'TO'}")
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_HEALTH], timeout_s=2)
rec.check("M4b", "HEALTH 读", d and not to and d[1] == 0,
          f"resp={d.hex()}" if d else "TO", f"resp={d.hex() if d else 'TO'}")
h0 = d
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_STATS], timeout_s=2)
rec.check("M4c", "STATS 读", d and not to and d[1] == 0,
          f"resp={d.hex()}" if d else "TO", f"resp={d.hex() if d else 'TO'}")
if d and len(d) >= 8:
    runsec = d[2] | (d[3] << 8) | (d[4] << 16)
    starts = d[5] | (d[6] << 8)
    rec.pass_("M4d", f"STATS 数值合理性", f"runSec={runsec}s startCnt={starts} lastReason={d[7]}")
else:
    rec.fail("M4d", "STATS 数值合理性", "响应长度不足")

# ---- M5 CLEAR (无故障幂等) ----
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_CLEAR], timeout_s=2)
rec.check("M5", "CLEAR 无故障幂等", d and not to and d[1] == 0, "err=0",
          f"resp={d.hex() if d else 'TO'}")

# ---- M6 行程测试 (真实升降, passes=1) ----
t0 = time.time()
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_TEST, 1], timeout_s=3)
rec.check("M6a", "TEST 受理", d and not to and d[1] == 0, f"err=0 受理", f"resp={d.hex() if d else 'TO'}")
# 进行中 MOVE -> BUSY
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_MOVE, 0x00, 100, 0, 0], timeout_s=2)
rec.check("M6b", "TEST 进行中 MOVE", d and not to and d[1] == 3,
          f"err=3 BUSY" if d and d[1] == 3 else f"err={d[1] if d else 'TO'}",
          f"err={d[1] if d else 'TO'}")
# 等 TEST 完成 (往返 ~4300*2 步 @2000Hz ≈ 5s + 余量)
ok = wait_idle(20)
dt = time.time() - t0
d = mquery()
rec.check("M6c", "TEST 1 趟往返完成", ok,
          f"{dt:.1f}s 后 IDLE (steps16={d[6] | (d[7] << 8) if d else '?'})",
          f"{dt:.1f}s 未 IDLE")
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_TEST, 1], timeout_s=3)
rec.check("M6d", "TEST 后可再次 TEST", d and not to and d[1] == 0, "受理", f"resp={d.hex() if d else 'TO'}")
ok = wait_idle(20)
rec.check("M6e", "第二次 TEST 完成", ok, f"{time.time()-t0:.1f}s 后 IDLE", "未 IDLE")

# ---- M7 TEST 中途 STOP ----
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_TEST, 2], timeout_s=3)
time.sleep(0.5)
d, to = tx(lk, C.FC_MOTOR_CTRL, [C.MOTOR_STOP], timeout_s=2)
rec.check("M7a", "TEST 中途 STOP", d and not to and d[1] == 0, "err=0",
          f"resp={d.hex() if d else 'TO'}")
ok = wait_idle(10)
d = mquery()
rec.check("M7b", "STOP 后状态恢复", ok and d[2] == 0, f"IDLE", f"state={d[2] if d else 'TO'}")

# ---- M8 运动后行程基准仍完好 (自检) ----
d, to = tx(lk, FC_SELFTEST_CTRL, [SELFTEST_SUB_QUERY], timeout_s=3)
st = parse_selftest(d if not to else None)
rec.check("M8", "运动用例后自检位图仍干净", st is not None and (st["errBits"] & 0x3F) == 0,
          f"errBits=0x{st['errBits']:04X}({bits_str(st['errBits'])})" if st else "无响应",
          f"st={st}")

rec.flush()
lk.close()
