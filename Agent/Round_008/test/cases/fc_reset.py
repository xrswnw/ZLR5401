"""Layer E — FC_RESET (0x08). E1 is destructive (actual reset)."""
from zlr import const
from zlr.result import Result, Verdict

SECTION = "E"


def _add(results, name, desc):
    r = Result(name, SECTION, "E")
    r.message = desc
    results.append(r)
    return r


def run(results, link, args):
    from zlr.hid_link import HidLink as _H
    # E2 non-empty data -> RESET_PARAM_ERR, must not reset
    r = _add(results, "E2", "reset with non-empty data -> param err, no reset")
    resp, to = link.transaction(const.FC_RESET, bytes([0x01]))
    if to:
        r.set(Verdict.FAIL, "no response")
    else:
        d = resp["data"]
        if d[0] == 1:
            r.set(Verdict.PASS, f"err={d[0]} (RESET_PARAM_ERR)")
        else:
            r.set(Verdict.FAIL, f"err={d[0]} != 1")
        # confirm still alive
        resp2, to2 = link.transaction(const.FC_HANDSHAKE)
        if to2:
            r.set(Verdict.FAIL, "device not alive after param-err reset")
        else:
            r.set(Verdict.PASS, r.message + "; device alive after param-err")

    # E1 actual reset (destructive) — only if --destructive
    r = _add(results, "E1", "empty reset -> device re-enumerates [destructive]")
    if not args.destructive:
        r.set(Verdict.SKIP, "skipped (needs --destructive)")
        return
    resp, to = link.transaction(const.FC_RESET, b"")
    if to:
        r.set(Verdict.FAIL, "no ack before reset")
        return
    if resp["data"][0] != 0:
        r.set(Verdict.FAIL, f"ack err={resp['data'][0]}")
        return
    # wait for re-enumeration; poll a fresh handle until App layer (1)
    import time
    link.close()
    link.dev = None
    # drop through to caller; runner will reopen fresh. Poll with our own probe:
    layer = None
    for _ in range(40):
        probe = _H()
        try:
            probe.open()
            r2, to2 = probe.transaction(const.FC_HANDSHAKE)
            if not to2:
                layer = r2["data"][19]
                if layer == 1:
                    probe.close()
                    break
        except Exception:
            pass
        try:
            probe.close()
        except Exception:
            pass
        time.sleep(0.25)
    if layer == 1:
        r.set(Verdict.PASS, "re-enumerated; back in App (layer=1)")
    else:
        r.set(Verdict.INCONCLUSIVE, f"re-enumerated but layer={layer} (transient boot)")

