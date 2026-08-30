"""Layer J — FC_RGB_CTRL (0x0E)."""
from zlr import const
from zlr.result import Result, Verdict

SECTION = "J"


def _add(results, name, desc):
    r = Result(name, SECTION, "J")
    r.message = desc
    results.append(r)
    return r


def run(results, link, args):
    for data, exp_mask, name in [
        (bytes([0x01, 0x00, 0x00]), 0x00, "J1 all off"),
        (bytes([0x01, 0x07, 0x00]), 0x07, "J2 all on white"),
        (bytes([0x01, 0x01, 0xFF]), 0x01, "J4 reserved bits ignored"),
    ]:
        r = _add(results, name.split(" ")[0], name)
        resp, to = link.transaction(const.FC_RGB_CTRL, data)
        if to:
            r.set(Verdict.FAIL, "no response")
            continue
        d = resp["data"]
        if d[0] != const.RGB_SET or d[1] != const.RGB_ERR_OK:
            r.set(Verdict.FAIL, f"cmd={d[0]} err={d[1]}")
            continue
        if d[2] != exp_mask:
            r.set(Verdict.FAIL, f"echo mask={d[2]} != expected 0x{exp_mask:02x}")
            continue
        r.set(Verdict.PASS, f"mask echo 0x{d[2]:02x}")

    # J3 length insufficient -> PARAM
    r = _add(results, "J3", "rgb len<3 -> param err")
    resp, to = link.transaction(const.FC_RGB_CTRL, bytes([0x01, 0x01]))
    if to:
        r.set(Verdict.FAIL, "no response")
        return
    d = resp["data"]
    if d[1] == const.RGB_ERR_PARAM:
        r.set(Verdict.PASS, f"err={d[1]} (PARAM)")
    else:
        r.set(Verdict.INCONCLUSIVE, f"err={d[1]} != PARAM; cmd len 2 accepted?")
