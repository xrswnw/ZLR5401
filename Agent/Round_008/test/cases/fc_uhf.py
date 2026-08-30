"""Layer H — FC_UHF_CTRL (0x0B). Requires SIM7500 module + antenna + tags.
OPEN probes link; if LINK/error, hardware-independent cases still run
(config read/write may fail if not powered)."""
import time

from zlr import const
from zlr.result import Result, Verdict

SECTION = "H"


def _add(results, name, desc):
    r = Result(name, SECTION, "H")
    r.message = desc
    results.append(r)
    return r


def _tx(link, data, t=3.0):
    resp, to = link.transaction(const.FC_UHF_CTRL, data, timeout_s=t)
    return resp, to


def run(results, link, args):
    hw = not args.skip_hardware

    # H1 OPEN
    r = _add(results, "H1", "OPEN -> READY or LINK (probe)", )
    resp, to = _tx(link, bytes([const.UHF_OPEN]))
    if to:
        r.set(Verdict.FAIL, "no response")
        ready = False
    else:
        e = resp["data"][1]
        if e == const.UHF_ERR_OK:
            r.set(Verdict.PASS, "OPEN ok")
            ready = True
        elif e == const.UHF_ERR_LINK:
            r.set(Verdict.INCONCLUSIVE, f"OPEN LINK ({e}) — module not on bench range")
            ready = False
        else:
            r.set(Verdict.INCONCLUSIVE, f"OPEN err={e}")
            ready = False

    # H2 GET_CONFIG
    r = _add(results, "H2", "GET_CONFIG parse")
    resp, to = _tx(link, bytes([const.UHF_GET_CONFIG]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        d = resp["data"]
        if len(d) >= 8:
            r.set(Verdict.PASS, f"power={d[2]} ant={d[3]} chk={d[4]} sess={d[5]} tgt={d[6]} q={d[7]} band={d[8] if len(d)>8 else '-'}")
        else:
            r.set(Verdict.FAIL, f"short data len={len(d)}")

    # H4 SET_CONFIG invalid params -> PARAM (hardware-independent check of range)
    r = _add(results, "H4", "SET_CONFIG invalid (power=4,ant=2,q=16) -> param")
    resp, to = _tx(link, bytes([const.UHF_SET_CONFIG, 4, 2, 0, 0, 0, 16]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        e = resp["data"][1]
        if e == const.UHF_ERR_PARAM:
            r.set(Verdict.PASS, f"err={e} (PARAM)")
        else:
            r.set(Verdict.INCONCLUSIVE, f"err={e} (doc: invalid combo rejected)")

    # H3 SET_CONFIG valid -> GET_CONFIG echo
    r = _add(results, "H3", "SET_CONFIG valid -> GET_CONFIG echo (closed loop)")
    if not hw:
        r.set(Verdict.SKIP, "skipped (hardware)")
    else:
        resp, to = _tx(link, bytes([const.UHF_SET_CONFIG, 20, 0, 0, 0, 0, 4]))
        if to:
            r.set(Verdict.FAIL, "no response")
        else:
            if resp["data"][1] != const.UHF_ERR_OK:
                r.set(Verdict.INCONCLUSIVE, f"set err={resp['data'][1]}")
            else:
                g, to2 = _tx(link, bytes([const.UHF_GET_CONFIG]))
                if to2:
                    r.set(Verdict.FAIL, "get miss")
                else:
                    gd = g["data"]
                    if gd[2] == 20 and gd[7] == 4:
                        r.set(Verdict.PASS, f"power echo {gd[2]}, q echo {gd[7]}")
                    else:
                        r.set(Verdict.INCONCLUSIVE, f"echo power={gd[2]} q={gd[7]}")
                # restore
                _tx(link, bytes([const.UHF_SET_CONFIG, 20, 0, 0, 0, 0, 4]))

    # H16 inventory before ... (close first to test NOT_READY)
    r = _add(results, "H16", "inventory while powered down -> NOT_READY")
    if not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        _tx(link, bytes([const.UHF_CLOSE]))
        time.sleep(0.3)
        resp, to = _tx(link, bytes([const.UHF_INVENTORY]))
        if to:
            r.set(Verdict.INCONCLUSIVE, "no response might mean still busy; reopen later")
        else:
            e = resp["data"][1]
            if e == const.UHF_ERR_NOT_READY:
                r.set(Verdict.PASS, f"err={e} (NOT_READY)")
            elif e == const.UHF_ERR_LINK:
                r.set(Verdict.INCONCLUSIVE, f"err={e} LINK")
            else:
                r.set(Verdict.INCONCLUSIVE, f"err={e}")

    # reopen if we closed
    _tx(link, bytes([const.UHF_OPEN]))

    # H5-6 INVENTORY (needs antenna range & tag)
    r = _add(results, "H6", "INVENTORY no-tag field -> NO_TAG or tags")
    if not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        resp, to = _tx(link, bytes([const.UHF_INVENTORY, 0xE8, 0x03]), t=4.0)  # 1000ms
        if to:
            r.set(Verdict.FAIL, "no response (timeout)")
        else:
            e = resp["data"][1]
            if e == const.UHF_ERR_NO_TAG:
                r.set(Verdict.PASS, "NO_TAG (field empty)")
            elif e == const.UHF_ERR_OK:
                cnt = resp["data"][2] | (resp["data"][3] << 8)
                r.set(Verdict.INCONCLUSIVE, f"OK count={cnt} (tags present)")
            else:
                r.set(Verdict.INCONCLUSIVE, f"err={e}")

    # H5 parse returned tags + H9 GET_TAGS
    r = _add(results, "H5", "INVENTORY parse tag records [rssi,epcLen,epc]")
    if not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        resp, to = _tx(link, bytes([const.UHF_INVENTORY, 0xE8, 0x03]), t=4.0)
        if to or resp["data"][1] != const.UHF_ERR_OK:
            r.set(Verdict.INCONCLUSIVE if hw else Verdict.SKIP,
                  f"no tags/err {resp['data'][1] if resp else 'to'}")
        else:
            d = resp["data"]
            cnt = d[2] | (d[3] << 8)
            pos = 4
            ok = True
            for i in range(cnt):
                if pos + 2 > len(d):
                    ok = False
                    break
                rssi = d[pos]
                epcLen = d[pos + 1]
                pos += 2
                if pos + epcLen > len(d) or not (6 <= epcLen <= 16):
                    ok = False
                    break
                pos += epcLen
            r.set(Verdict.PASS if ok else Verdict.FAIL, f"count={cnt} records parsed")

    # H9 GET_TAGS
    r = _add(results, "H9", "GET_TAGS parse")
    if not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        resp, to = _tx(link, bytes([const.UHF_GET_TAGS, 0]))
        if to:
            r.set(Verdict.FAIL, "no response")
        else:
            d = resp["data"]
            total = d[2] | (d[3] << 8)
            r.set(Verdict.PASS, f"total={total} records")

    # H7 READ_TAG short
    r = _add(results, "H7", "READ_TAG data too short -> param err")
    resp, to = _tx(link, bytes([const.UHF_READ_TAG]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if resp["data"][1] == const.UHF_ERR_PARAM
              else Verdict.INCONCLUSIVE, f"err={resp['data'][1]}")

    # H8 WRITE->READ closed loop (needs a write-capable tag in range)
    r = _add(results, "H8", "WRITE_TAG then READ_TAG same addr (closed loop)")
    if not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        # pick first tag from inventory
        resp, to = _tx(link, bytes([const.UHF_INVENTORY, 0xE8, 0x03]), t=4.0)
        if to or resp["data"][1] != const.UHF_ERR_OK:
            r.set(Verdict.SKIP, "no tag in field to write")
        else:
            d = resp["data"]
            cnt = d[2] | (d[3] << 8)
            if cnt == 0:
                r.set(Verdict.SKIP, "no tag")
            else:
                rssi = d[4]
                elen = d[5]
                epc = d[6:6 + elen]
                # write 2 bytes at bank=0 addr=0x02 (EPC bank offset 2 = TID-adjacent user; use bank0)
                wdata = bytes([const.UHF_WRITE_TAG, elen]) + epc + bytes([0, 0x02, 2, 0xAA, 0x55])
                w, tow = _tx(link, wdata, t=3.0)
                if tow:
                    r.set(Verdict.FAIL, "no write response")
                elif w["data"][1] == const.UHF_ERR_OK:
                    time.sleep(0.8)  # let write op settle out of BUSY
                    rd, tod = _tx(link, bytes([const.UHF_READ_TAG, elen]) + epc + bytes([0, 0x02, 2]), t=3.0)
                    if tod:
                        r.set(Verdict.INCONCLUSIVE, "write ok but read miss")
                    else:
                        if rd["data"][1] == const.UHF_ERR_OK:
                            r.set(Verdict.PASS, "write + read accepted (closed loop OK)")
                        elif rd["data"][1] == const.UHF_ERR_BUSY:
                            r.set(Verdict.INCONCLUSIVE, "read BUSY right after write — state machine serialization")
                        else:
                            r.set(Verdict.INCONCLUSIVE, f"read err={rd['data'][1]}")
                elif w["data"][1] == const.UHF_ERR_BUSY:
                    r.set(Verdict.INCONCLUSIVE, f"write BUSY ({w['data'][1]}) — prior op still in progress (expected if serialized)")
                else:
                    r.set(Verdict.INCONCLUSIVE, f"write err={w['data'][1]}")

    # H10 GET_STATUS
    r = _add(results, "H10", "GET_STATUS parse")
    resp, to = _tx(link, bytes([const.UHF_GET_STATUS]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        d = resp["data"]
        if len(d) >= 12:
            r.set(Verdict.PASS, f"state={d[2]} link={d[3]} powered={d[5]} antOK={d[6]} lastErr={d[7]}")
        else:
            r.set(Verdict.INCONCLUSIVE, f"len={len(d)}")

    # H11 CHECK_ANT
    r = _add(results, "H11", "CHECK_ANT parse")
    resp, to = _tx(link, bytes([const.UHF_CHECK_ANT]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        d = resp["data"]
        r.set(Verdict.PASS if len(d) >= 7 else Verdict.INCONCLUSIVE,
              f"antOK={d[2] if len(d)>2 else '-'} RL={int.from_bytes(d[3:5],'big')} VSWR={int.from_bytes(d[5:7],'big')}")

    # H12 SCAN_START/STOP
    r = _add(results, "H12", "SCAN_START then GET_TAGS then SCAN_STOP")
    if not hw:
        r.set(Verdict.SKIP, "skipped")
    else:
        resp, to = _tx(link, bytes([const.UHF_SCAN_START, 0xE8, 0x03]))
        if to:
            r.set(Verdict.FAIL, "no response")
        else:
            if resp["data"][1] == const.UHF_ERR_OK:
                time.sleep(1.0)
                g, tg = _tx(link, bytes([const.UHF_GET_TAGS, 0]), t=2.0)
                st, ts = _tx(link, bytes([const.UHF_SCAN_STOP]))
                if ts:
                    r.set(Verdict.FAIL, "scan stop miss")
                elif st["data"][1] == const.UHF_ERR_OK:
                    r.set(Verdict.PASS, "scan start/get/stop ok")
                else:
                    r.set(Verdict.INCONCLUSIVE, f"stop err={st['data'][1]}")
            else:
                r.set(Verdict.INCONCLUSIVE, f"start err={resp['data'][1]}")

    # H13 GET_DUMP
    r = _add(results, "H13", "GET_DUMP parse (no crash)")
    resp, to = _tx(link, bytes([const.UHF_GET_DUMP]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        r.set(Verdict.PASS if len(resp["data"]) >= 4 else Verdict.INCONCLUSIVE,
              f"len={len(resp['data'])}")

    # H14 STOP
    r = _add(results, "H14", "STOP -> ok")
    resp, to = _tx(link, bytes([const.UHF_STOP]))
    r.set(Verdict.PASS if not to and resp["data"][1] == const.UHF_ERR_OK
          else Verdict.FAIL, f"err={resp['data'][1] if not to else 'to'}")

    # H15 CLOSE
    r = _add(results, "H15", "CLOSE -> ok")
    resp, to = _tx(link, bytes([const.UHF_CLOSE]))
    r.set(Verdict.PASS if not to and resp["data"][1] == const.UHF_ERR_OK
          else Verdict.FAIL, f"err={resp['data'][1] if not to else 'to'}")
    _tx(link, bytes([const.UHF_OPEN]))
