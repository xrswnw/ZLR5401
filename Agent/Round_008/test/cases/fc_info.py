"""Layer D — FC_DEVICE_INFO (0x07)."""
from zlr import const
from zlr.result import Result, Verdict

SECTION = "D"


def _add(results, name, desc):
    r = Result(name, SECTION, "D")
    r.message = desc
    results.append(r)
    return r


def run(results, link, args):
    # D1 basic response
    r = _add(results, "D1", "device info basic")
    resp, to = link.transaction(const.FC_DEVICE_INFO)
    if to:
        r.set(Verdict.FAIL, "no response")
        return
    if resp["func"] != const.RSP(const.FC_DEVICE_INFO):
        r.set(Verdict.FAIL, f"func {resp['func']:#x}")
        return
    d = resp["data"]
    if d[0] != 0:
        r.set(Verdict.FAIL, f"result={d[0]}")
        return
    hw = d[2:18].split(b"\x00")[0]
    sw = d[18:34].split(b"\x00")[0]
    printable = all(32 <= c < 127 for c in hw + sw)
    r.set(Verdict.PASS if printable else Verdict.FAIL,
          f"addr={d[1]} hw={hw!r} sw={sw!r}")

    # D2 consistency with handshake (layer==App implies device info exists)
    r = _add(results, "D2", "device info while App layer")
    resp, to = link.transaction(const.FC_HANDSHAKE)
    if to:
        r.set(Verdict.FAIL, "handshake missing")
        return
    if resp["data"][19] == 1:
        r.set(Verdict.PASS, "layer=1 (App); DEVICE_INFO valid")
    else:
        r.set(Verdict.INCONCLUSIVE, f"layer={resp['data'][19]} not App")
