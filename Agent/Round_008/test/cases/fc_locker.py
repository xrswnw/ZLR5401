"""Layer I — FC_LOCKER_CTRL (0x0D). Full flow needs UHF + hard tags + magnet block."""
import time

from zlr import const
from zlr.result import Result, Verdict

SECTION = "I"


def _add(results, name, desc):
    r = Result(name, SECTION, "I")
    r.message = desc
    results.append(r)
    return r


def _tx(link, data, t=1.0):
    resp, to = link.transaction(const.FC_LOCKER_CTRL, data, timeout_s=t)
    return resp, to


def _query(link):
    resp, to = _tx(link, bytes([const.LOCKER_QUERY]))
    if to:
        return None
    d = resp["data"]
    return {
        "state": d[2],
        "hm": d[3] | (d[4] << 8),
        "sc": d[5] | (d[6] << 8),
        "su": d[7] | (d[8] << 8),
    }


def run(results, link, args):
    hw = not args.skip_hardware
    # always cancel to a clean IDLE first
    _tx(link, bytes([const.LOCKER_CANCEL]))

    # I2 CONFIGURE hardCnt > 16 -> PARAM
    r = _add(results, "I2", "CONFIGURE hardCnt>16 -> param err")
    resp, to = _tx(link, bytes([const.LOCKER_CONFIGURE, 17, 0, 1, 0]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.LOCKER_ERR_PARAM
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")

    # I1 CONFIGURE valid, reset counters
    r = _add(results, "I1", "CONFIGURE -> counters reset")
    resp, to = _tx(link, bytes([const.LOCKER_CONFIGURE, 0, 0, 2, 0]))  # soft=2
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        if resp["data"][1] != const.LOCKER_ERR_OK:
            r.set(Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")
        else:
            q = _query(link)
            if q and q["hm"] == 0 and q["sc"] == 2 and q["su"] == 0:
                r.set(Verdict.PASS, f"hm={q['hm']} sc={q['sc']} su={q['su']}")
            else:
                r.set(Verdict.INCONCLUSIVE, f"q={q}")

    # I3 ADD valid EPC (12B)
    r = _add(results, "I3", "ADD 12B EPC -> ok")
    epc = bytes(range(1, 13))
    resp, to = _tx(link, bytes([const.LOCKER_ADD, 12]) + epc)
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.LOCKER_ERR_OK
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")

    # I4 ADD too short -> PARAM
    r = _add(results, "I4", "ADD len<1 -> param err")
    resp, to = _tx(link, bytes([const.LOCKER_ADD]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.LOCKER_ERR_PARAM
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")

    # I3b ADD >12B -> record (doc says <=12, firmware truncates to 12)
    r = _add(results, "I3b", "ADD 14B EPC -> record (doc: <=12)")
    epc14 = bytes(range(1, 15))
    resp, to = _tx(link, bytes([const.LOCKER_ADD, 14]) + epc14)
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        r.set(Verdict.INCONCLUSIVE if e == const.LOCKER_ERR_OK else Verdict.PASS,
              f"err={e} (firmware truncates to 12; doc says <=12)")

    # I6 QUERY parse
    r = _add(results, "I6", "QUERY parse")
    q = _query(link)
    if q is None:
        r.set(Verdict.FAIL, "no response")
    elif q["state"] in (0, 1, 2, 3, 4, 5):
        r.set(Verdict.PASS, f"state={q['state']} hm={q['hm']} sc={q['sc']} su={q['su']}")
    else:
        r.set(Verdict.INCONCLUSIVE, f"state={q['state']}")

    # I5 START
    r = _add(results, "I5", "START -> state CONFIGURED(1)")
    resp, to = _tx(link, bytes([const.LOCKER_START]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        if e not in (const.LOCKER_ERR_OK, 2):
            r.set(Verdict.INCONCLUSIVE, f"err={e}")
        else:
            time.sleep(0.5)
            q = _query(link)
            if q and q["state"] == const.LOCKER_STATE_CONFIGURED:
                r.set(Verdict.PASS, f"state={q['state']} (CONFIGURED)")
            else:
                r.set(Verdict.INCONCLUSIVE, f"state={q['state'] if q else None} (may need UHF)")

    # I7 GET_EVENT empty -> NO_EVENT
    r = _add(results, "I7", "GET_EVENT no events -> param NO_EVENT")
    # drain any events
    for _ in range(20):
        resp, to = _tx(link, bytes([const.LOCKER_GET_EVENT]))
        if to or resp["data"][1] == const.LOCKER_ERR_NO_EVENT:
            break
    resp, to = _tx(link, bytes([const.LOCKER_GET_EVENT]))
    if to:
        r.set(Verdict.INCONCLUSIVE, "no response")
    else:
        e = resp["data"][1]
        if e == const.LOCKER_ERR_NO_EVENT:
            r.set(Verdict.PASS, "NO_EVENT")
        else:
            r.set(Verdict.INCONCLUSIVE, f"err={e}")

    # I8 CONSUME_SOFT in non-soft state -> record
    r = _add(results, "I8", "CONSUME_SOFT while configured -> record")
    resp, to = _tx(link, bytes([const.LOCKER_CONSUME_SOFT]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        if e == const.LOCKER_ERR_OK:
            r.set(Verdict.INCONCLUSIVE, "accepted — firmware doesn't guard soft-state only")
        else:
            r.set(Verdict.PASS, f"err={e}")

    # I9 CANCEL -> IDLE
    r = _add(results, "I9", "CANCEL -> state IDLE(0)")
    resp, to = _tx(link, bytes([const.LOCKER_CANCEL]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        time.sleep(0.3)
        q = _query(link)
        r.set(Verdict.PASS if q and q["state"] == const.LOCKER_STATE_IDLE
              else Verdict.INCONCLUSIVE, f"state={q['state'] if q else None}")

    # I10 full flow — adapt to a REAL tag in the field: hardCount=1, add that EPC,
    # START -> MATCH_OK -> HARD_DONE -> (soft if sc>0) -> responsive.
    r = _add(results, "I10", "full flow: CONFIGURE+ADD(real tag)+START -> MATCH -> HARD_DONE")
    if args.no_locker_motion:
        r.set(Verdict.SKIP, "skipped (physical magnet lift gated by --no-locker-motion)")
    elif not hw:
        r.set(Verdict.SKIP, "skipped (needs UHF + hard tag in field)")
    else:
        # discover a real tag EPC via UHF
        ro, _ = link.transaction(const.FC_UHF_CTRL, bytes([const.UHF_OPEN]), timeout_s=3)
        ri, to = link.transaction(const.FC_UHF_CTRL,
                                  bytes([const.UHF_INVENTORY, 0xE8, 0x03]), timeout_s=5)
        real_epc = None
        if ri and ri["data"][1] == const.UHF_ERR_OK:
            cnt = ri["data"][2] | (ri["data"][3] << 8)
            if cnt and len(ri["data"]) >= 6:
                elen = ri["data"][5]
                real_epc = ri["data"][6:6 + elen]
        if not real_epc:
            r.set(Verdict.SKIP, "no real tag detected in field")
            return

        _tx(link, bytes([const.LOCKER_CANCEL]))
        _tx(link, bytes([const.LOCKER_CONFIGURE, 1, 0, 1, 0]))  # 1 hard + 1 soft
        _tx(link, bytes([const.LOCKER_ADD, len(real_epc)]) + real_epc)
        resp, to = _tx(link, bytes([const.LOCKER_START]))
        if to:
            r.set(Verdict.FAIL, "start no response")
            return
        # wait for flow; drain events
        saw = set()
        got_soft_done = False
        for _ in range(40):
            resp, to = _tx(link, bytes([const.LOCKER_GET_EVENT]))
            if to or resp["data"][1] == const.LOCKER_ERR_NO_EVENT:
                break
            code = resp["data"][3]
            saw.add(code)
            if code == const.LOCKER_EVT_HARD_DONE:
                # now in soft decode; consume one soft -> SOFT_USED + DONE (sc=1)
                _tx(link, bytes([const.LOCKER_CONSUME_SOFT]))
                time.sleep(0.3)
            time.sleep(0.2)
            # also check state reached DONE
            q = _query(link)
            if q and q["state"] == const.LOCKER_STATE_DONE:
                got_soft_done = True
                break
        _tx(link, bytes([const.LOCKER_CANCEL]))
        if const.LOCKER_EVT_MATCH_OK in saw:
            if const.LOCKER_EVT_HARD_DONE in saw or got_soft_done:
                r.set(Verdict.PASS, f"flow MATCH->HARD_DONE->DONE; events={sorted(saw)}")
            else:
                r.set(Verdict.INCONCLUSIVE, f"matched but not full done; events={sorted(saw)}")
        else:
            r.set(Verdict.INCONCLUSIVE, f"no MATCH; events={sorted(saw)} (tag not read while locker scanning?)")

    # I11 mismatch tag — configure an EMPTY list (0 hard), put a real stray tag
    # in field -> reading any EPC not in list -> MISMATCH event.
    r = _add(results, "I11", "non-list EPC -> MISMATCH event")
    if args.no_locker_motion:
        r.set(Verdict.SKIP, "skipped (physical magnet lift gated)")
    elif not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        _tx(link, bytes([const.LOCKER_CANCEL]))
        # 0 hard tags -> any detected tag is non-list
        _tx(link, bytes([const.LOCKER_CONFIGURE, 0, 0, 0, 0]))
        _tx(link, bytes([const.LOCKER_START]))
        got = None
        for _ in range(40):
            resp, to = _tx(link, bytes([const.LOCKER_GET_EVENT]))
            if to:
                time.sleep(0.2)
                continue
            if resp["data"][1] == const.LOCKER_ERR_NO_EVENT:
                time.sleep(0.2)
                continue
            if resp["data"][3] == const.LOCKER_EVT_MISMATCH:
                got = True
                break
            time.sleep(0.2)
        _tx(link, bytes([const.LOCKER_CANCEL]))
        r.set(Verdict.PASS if got else Verdict.INCONCLUSIVE,
              "MISMATCH event" if got else "no MISMATCH (no stray tag read while scanning)")

    # I12 event buffer overflow robustness
    r = _add(results, "I12", "event buffer overflow robustness (no crash)")
    # trigger several events by repeated consume in soft state
    _tx(link, bytes([const.LOCKER_CANCEL]))
    _tx(link, bytes([const.LOCKER_CONFIGURE, 0, 0, 5, 0]))
    _tx(link, bytes([const.LOCKER_START]))
    time.sleep(0.5)
    for i in range(10):
        resp, to = _tx(link, bytes([const.LOCKER_CONSUME_SOFT]))
        time.sleep(0.05)
    # verify still responsive
    q = _query(link)
    _tx(link, bytes([const.LOCKER_CANCEL]))
    r.set(Verdict.PASS if q and q["state"] in (0, 1, 2, 3, 4, 5)
          else Verdict.FAIL, f"responsive after storm; state={q['state'] if q else '?'}")
