"""Layer F — FC_ENTER_BOOT (0x02) + FC_EXIT_BOOT (0x09). Fully destructive."""
import time

from zlr import const
from zlr.result import Result, Verdict

SECTION = "F"


def _add(results, name, desc):
    r = Result(name, SECTION, "F")
    r.message = desc
    results.append(r)
    return r


def _reopen(link, tries=30):
    link.close()
    time.sleep(1.2)
    for _ in range(tries):
        try:
            link.open()
            return True
        except Exception:
            time.sleep(0.2)
    return False


def run(results, link, args):
    boot_test = getattr(args, "boot_test", False)
    # F1 enter boot (destructive)
    r = _add(results, "F1", "enter boot -> layer=0 [destructive]")
    if not boot_test:
        r.set(Verdict.SKIP, "skipped (needs --boot-test)")
    else:
        resp, to = link.transaction(const.FC_ENTER_BOOT)
        if to:
            r.set(Verdict.FAIL, "no ack before enter-boot")
            return
        if resp["data"][0] != 0:
            r.set(Verdict.FAIL, f"ack err={resp['data'][0]}")
            return
        if not _reopen(link):
            r.set(Verdict.FAIL, "device did not re-enumerate after enter-boot")
            return
        time.sleep(0.3)
        resp2, to2 = link.transaction(const.FC_HANDSHAKE)
        if to2:
            r.set(Verdict.FAIL, "no handshake in boot")
            return
        layer = resp2["data"][19]
        if layer == 0:
            r.set(Verdict.PASS, "entered Boot (layer=0)")
        else:
            r.set(Verdict.FAIL, f"layer={layer} != 0 (not in boot)")
            return  # don't try exit if not in boot

    # F2 exit boot -> back to App (restore)
    r = _add(results, "F2", "exit boot -> back to App (restore)")
    if not boot_test:
        r.set(Verdict.SKIP, "skipped (needs --boot-test)")
        return
    resp, to = link.transaction(const.FC_EXIT_BOOT)
    if to:
        r.set(Verdict.FAIL, "no response to EXIT_BOOT")
        return
    if not _reopen(link):
        r.set(Verdict.FAIL, "no re-enum after exit boot")
        return
    time.sleep(0.3)
    resp2, to2 = link.transaction(const.FC_HANDSHAKE)
    if to2:
        r.set(Verdict.FAIL, "no handshake after exit boot")
        return
    layer = resp2["data"][19]
    if layer == 1:
        r.set(Verdict.PASS, "back to App (layer=1)")
    else:
        r.set(Verdict.FAIL, f"layer={layer} != 1 after exit boot")
