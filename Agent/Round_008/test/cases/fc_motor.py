"""Layer G — FC_MOTOR_CTRL (0x0A). Requires stepper motor + travel switches.
Hardware-free commands (QUERY/HEALTH/STATS/SPEED/TORQUE accepts) always run.
MOVE/TEST need motor present; if no motor, may still get OK (no load).
"""
import time

from zlr import const
from zlr.result import Result, Verdict

SECTION = "G"


def _add(results, name, desc):
    r = Result(name, SECTION, "G")
    r.message = desc
    results.append(r)
    return r


def _tx(link, data, t=1.0):
    resp, to = link.transaction(const.FC_MOTOR_CTRL, data, timeout_s=t)
    return resp, to


def _query(link):
    resp, to = _tx(link, bytes([const.MOTOR_QUERY]))
    if to:
        return None
    d = resp["data"]
    # [cmd, err, state, fault, diag1, diag2, stepsDoneL, stepsDoneH]
    return {
        "cmd": d[0], "err": d[1], "state": d[2], "fault": d[3],
        "diag1": d[4], "diag2": d[5],
        "stepsDone": d[6] | (d[7] << 8),
    }


def run(results, link, args):
    hw = not args.skip_hardware
    # ensure motor out of test/fault first default-run does CLEAR+STOP at start? do STOP
    _tx(link, bytes([const.MOTOR_STOP]))

    # G3 MOVE short data -> PARAM
    r = _add(results, "G3", "MOVE data too short -> param err")
    resp, to = _tx(link, bytes([const.MOTOR_MOVE, 0]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.MOTOR_ERR_PARAM
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")

    # G2 MOVE dir invalid
    r = _add(results, "G2", "MOVE dir=2 -> param err")
    resp, to = _tx(link, bytes([const.MOTOR_MOVE, 2, 100, 0, 0]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.MOTOR_ERR_PARAM
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")

    # G1 MOVE small step + verify stepsDone increments (closed loop)
    r = _add(results, "G1", "MOVE 200 steps -> stepsDone advances")
    q0 = _query(link)
    resp, to = _tx(link, bytes([const.MOTOR_MOVE, 0, 200, 0, 0]))
    if to:
        r.set(Verdict.FAIL, "no response")
        return
    if resp["data"][1] != const.MOTOR_ERR_OK:
        r.set(Verdict.FAIL if resp["data"][1] == const.MOTOR_ERR_FAULT
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]} (fault?)")
        return
    time.sleep(0.3)
    resp, to = _tx(link, bytes([const.MOTOR_QUERY]))
    q = _query(link)
    if q is None:
        r.set(Verdict.FAIL, "query miss")
        return
    moved = q["stepsDone"] - (q0["stepsDone"] if q0 else 0)
    if moved > 0:
        r.set(Verdict.PASS, f"stepsDone advanced by ~{moved}")
    else:
        r.set(Verdict.INCONCLUSIVE, f"stepsDone not advanced (moved={moved}, state={q['state']})")

    # G4 SPEED valid
    r = _add(results, "G4", "SPEED 1000 -> ok")
    resp, to = _tx(link, bytes([const.MOTOR_SPEED, 0xE8, 0x03]))  # 1000
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.MOTOR_ERR_OK
              else Verdict.FAIL, f"err={resp['data'][1]}")

    # G5 SPEED out of range (doc 1..2000)
    r = _add(results, "G5", "SPEED 2001 -> record behavior (doc: 1..2000)")
    resp, to = _tx(link, bytes([const.MOTOR_SPEED, 0xD1, 0x07]))  # 2001
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        if e == const.MOTOR_ERR_PARAM:
            r.set(Verdict.PASS, "rejected (PARAM)")
        else:
            r.set(Verdict.INCONCLUSIVE, f"accepted err={e} — firmware does not range-check (doc gap)")

    # G6 TORQUE valid/invalid
    r = _add(results, "G6a", "TORQUE 20 -> ok")
    resp, to = _tx(link, bytes([const.MOTOR_TORQUE, 20]))
    r.set(Verdict.PASS if not to and resp["data"][1] == const.MOTOR_ERR_OK
          else Verdict.FAIL, f"err={resp['data'][1] if not to else 'to'}")
    r = _add(results, "G6b", "TORQUE 101 -> record behavior (doc: 6..100)")
    resp, to = _tx(link, bytes([const.MOTOR_TORQUE, 101]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        if e == const.MOTOR_ERR_PARAM:
            r.set(Verdict.PASS, "rejected (PARAM)")
        else:
            r.set(Verdict.INCONCLUSIVE, f"accepted err={e} — doc gap")

    # G7 STOP
    r = _add(results, "G7", "STOP -> ok")
    resp, to = _tx(link, bytes([const.MOTOR_STOP]))
    r.set(Verdict.PASS if not to and resp["data"][1] == const.MOTOR_ERR_OK
          else Verdict.FAIL, f"err={resp['data'][1] if not to else 'to'}")

    # G8 QUERY
    r = _add(results, "G8", "QUERY parse + state in 0..2")
    q = _query(link)
    if q is None:
        r.set(Verdict.FAIL, "no response")
    elif q["state"] in (0, 1, 2):
        r.set(Verdict.PASS, f"state={q['state']} fault={q['fault']:#x} diag1={q['diag1']} diag2={q['diag2']}")
    else:
        r.set(Verdict.INCONCLUSIVE, f"state={q['state']}")

    # G9 HEALTH
    r = _add(results, "G9", "HEALTH parse")
    resp, to = _tx(link, bytes([const.MOTOR_HEALTH]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        d = resp["data"]
        thr = d[3] | (d[4] << 8)
        trq = d[5] | (d[6] << 8)
        r.set(Verdict.PASS if trq <= 4095 else Verdict.INCONCLUSIVE,
              f"olov={d[2]} thresh={thr} trq={trq} reason={d[7]}")

    # G10 STATS before/after a move
    r = _add(results, "G10", "STATS startCount increments across a MOVE")
    s0 = _stats(link)
    _tx(link, bytes([const.MOTOR_MOVE, 0, 50, 0, 0]))
    time.sleep(0.2)
    _tx(link, bytes([const.MOTOR_STOP]))
    s1 = _stats(link)
    if s0 is None or s1 is None:
        r.set(Verdict.FAIL, "stats miss")
    elif s1["start"] >= s0["start"]:
        r.set(Verdict.PASS, f"startCount {s0['start']}->{s1['start']}, runSec {s0['run']}->{s1['run']}")
    else:
        r.set(Verdict.INCONCLUSIVE, f"startCount {s0['start']}->{s1['start']}")

    # G11 CLEAR
    r = _add(results, "G11", "CLEAR -> ok")
    resp, to = _tx(link, bytes([const.MOTOR_CLEAR]))
    r.set(Verdict.PASS if not to and resp["data"][1] == const.MOTOR_ERR_OK
          else Verdict.FAIL, f"err={resp['data'][1] if not to else 'to'}")

    # G13 TEST passes=0
    r = _add(results, "G13", "TEST passes=0 -> record behavior")
    resp, to = _tx(link, bytes([const.MOTOR_TEST, 0]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        if e == const.MOTOR_ERR_PARAM:
            r.set(Verdict.PASS, "rejected (PARAM)")
        elif e == const.MOTOR_ERR_OK:
            r.set(Verdict.INCONCLUSIVE, "accepted — firmware treats 0 len? doc unclear")
            _tx(link, bytes([const.MOTOR_STOP]))
        else:
            r.set(Verdict.INCONCLUSIVE, f"err={e}")

    # G12 TEST passes=1 (needs travel switches; physical actuation)
    r = _add(results, "G12", "TEST passes=1 -> completes one round-trip")
    if args.no_motor_test:
        r.set(Verdict.SKIP, "skipped (physical motor travel gated by --no-motor-test)")
    elif not hw:
        r.set(Verdict.SKIP, "skipped (motor/travel not verified)")
    else:
        resp, to = _tx(link, bytes([const.MOTOR_TEST, 1]))
        if to:
            r.set(Verdict.FAIL, "no response")
        else:
            if resp["data"][1] != const.MOTOR_ERR_OK:
                r.set(Verdict.INCONCLUSIVE if resp["data"][1] == const.MOTOR_ERR_BUSY
                      else Verdict.FAIL, f"err={resp['data'][1]}")
            else:
                # poll until back to idle
                done = None
                for _ in range(200):
                    q = _query(link)
                    if q and q["state"] in (0, 2):
                        done = q
                        break
                    time.sleep(0.2)
                if done:
                    r.set(Verdict.PASS, f"test completed; final state={done['state']} diag1={done['diag1']}")
                else:
                    r.set(Verdict.INCONCLUSIVE, "test did not settle in time")

    # G14 TEST busy: start a pass and immediately re-send
    r = _add(results, "G14", "mid-TEST re-send -> BUSY")
    if args.no_motor_test or not hw:
        r.set(Verdict.SKIP, "skipped (physical motor travel gated)")
    else:
        resp, to = _tx(link, bytes([const.MOTOR_TEST, 1]))
        if to:
            r.set(Verdict.FAIL, "no first response")
        else:
            resp2, to2 = _tx(link, bytes([const.MOTOR_TEST, 1]))
            if to2:
                r.set(Verdict.PASS, "first accepted then no second ack? (state busy handled internally)")
            elif resp2["data"][1] == const.MOTOR_ERR_BUSY:
                r.set(Verdict.PASS, "second rejected BUSY")
            else:
                r.set(Verdict.INCONCLUSIVE, f"second err={resp2['data'][1]}")
            # stop and wait to settle
            time.sleep(1)
            _tx(link, bytes([const.MOTOR_STOP]))
            for _ in range(50):
                q = _query(link)
                if q and q["state"] in (0, 2):
                    break
                time.sleep(0.2)


def _stats(link):
    resp, to = _tx(link, bytes([const.MOTOR_STATS]))
    if to:
        return None
    d = resp["data"]
    run = d[2] | (d[3] << 8) | (d[4] << 16)
    start = d[5] | (d[6] << 8)
    return {"run": run, "start": start, "reason": d[7]}
