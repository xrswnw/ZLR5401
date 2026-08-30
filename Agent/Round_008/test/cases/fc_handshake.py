"""Layer C — FC_HANDSHAKE (0x01)."""
from zlr import const
from zlr.crc32 import crc32
from zlr.result import Result, Verdict

SECTION = "C"


def _add(results, name, desc):
    r = Result(name, SECTION, "C")
    r.message = desc
    results.append(r)
    return r


def run(results, link, args):
    # C1 basic response
    r = _add(results, "C1", "handshake basic response")
    resp, to = link.transaction(const.FC_HANDSHAKE)
    if to:
        r.set(Verdict.FAIL, "no response")
        return
    d = resp["data"]
    if resp["func"] != const.RSP(const.FC_HANDSHAKE):
        r.set(Verdict.FAIL, f"func {resp['func']:#x} != 0xFE")
        return
    if d[0] != 0:
        r.set(Verdict.FAIL, f"result={d[0]}")
        return
    if d[1] != const.PROTO_VERSION:
        r.set(Verdict.INCONCLUSIVE, f"protoVer={d[1]} != 2 (doc) — check")
    if d[2] not in (0, 1, 2, 3):
        r.set(Verdict.FAIL, f"status={d[2]} not in 0..3")
        return
    if d[19] != 1:
        r.set(Verdict.FAIL, f"layer={d[19]} != 1 (App)")
        return
    baud = int.from_bytes(d[24:28], "little")
    r.set(Verdict.PASS, f"status={d[2]} layer=1 protoVer={d[1]} baud={baud}")

    # C2 UID <-> uidHash
    r = _add(results, "C2", "UID vs uidHash self-consistency")
    resp, to = link.transaction(const.FC_HANDSHAKE)
    if to:
        r.set(Verdict.FAIL, "no response")
        return
    d = resp["data"]
    uid = d[3:15]
    calc = crc32(uid)
    recv = int.from_bytes(d[15:19], "little")
    if calc != recv:
        r.set(Verdict.FAIL, f"uidHash mismatch calc=0x{calc:08x} recv=0x{recv:08x}")
    else:
        r.set(Verdict.PASS, f"uidHash matches UID (0x{recv:08x})")

    # C3 repeat -> stable
    r = _add(results, "C3", "two samples consistent (UID/upg/status stable)")
    s1 = _one(link)
    s2 = _one(link)
    if s1 is None or s2 is None:
        r.set(Verdict.FAIL, "missing sample")
        return
    if s1["uid"] == s2["uid"] and s1["upg"] == s2["upg"] and s1["status"] == s2["status"]:
        r.set(Verdict.PASS, f"UID/upgradeCount/status stable (UID {s1['uid'].hex()})")
    else:
        r.set(Verdict.INCONCLUSIVE, f"samples differ: {s1} vs {s2}")


def _one(link):
    resp, to = link.transaction(const.FC_HANDSHAKE)
    if to:
        return None
    d = resp["data"]
    return {
        "uid": d[3:15],
        "upg": int.from_bytes(d[20:24], "little"),
        "status": d[2],
    }
