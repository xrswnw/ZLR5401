"""Layer B — link negation: malformed frames must be silently dropped."""
from zlr.frame import build_frame
from zlr import const
from zlr.result import Result, Verdict

SECTION = "B"


def _add(results, name, desc):
    r = Result(name, SECTION, "B")
    r.message = desc
    results.append(r)
    return r


def run(results, link, args):
    def expect_silent(r, frame):
        link.send_bytes(frame)
        resp = link.recv_frame(timeout_s=0.8)
        if resp is None:
            r.set(Verdict.PASS, "device stayed silent (no response)")
            return True
        r.set(Verdict.FAIL, f"expected silence but got func=0x{resp['func']:02x}")
        return False

    # B1 CRC tamper
    r = _add(results, "B1", "CRC tamper -> silence")
    f = bytearray(build_frame(0x01, const.FC_HANDSHAKE))
    f[-1] ^= 0xFF
    expect_silent(r, bytes(f))

    # B2 reserved != 0x00
    r = _add(results, "B2", "reserved=0x01 -> silence")
    f = bytearray(build_frame(0x01, const.FC_HANDSHAKE))
    f[3] = 0x01
    f[-4:] = _crc(f[:len(f) - 4])  # recompute over modified (still valid CRC of that content)
    expect_silent(r, bytes(f))

    # B3 length too small (4)
    r = _add(results, "B3", "length<5 -> silence")
    f = bytearray(build_frame(0x01, const.FC_HANDSHAKE))
    f[4] = 4
    # rebuild crc over the 8 bytes before crc (6 header + func)
    f[-4:] = _crc(f[:len(f) - 4])
    expect_silent(r, bytes(f))

    # B4 length too large (1030)
    r = _add(results, "B4", "length>1029 -> silence")
    f = bytearray(build_frame(0x01, const.FC_HANDSHAKE))
    f[4] = (1030) & 0xFF
    f[5] = (1030 >> 8) & 0xFF
    f[-4:] = _crc(f[:len(f) - 4])
    expect_silent(r, bytes(f))

    # B5 foreign device addr 0x02 -> silence (no response)
    r = _add(results, "B5", "devAddr=0x02 -> silence")
    f = build_frame(0x02, const.FC_HANDSHAKE)
    expect_silent(r, f)

    # B6 broadcast addr 0xFF -> response
    r = _add(results, "B6", "devAddr=0xFF (broadcast) -> response")
    link.send_frame(const.BROADCAST, const.FC_HANDSHAKE)
    resp = link.recv_frame(timeout_s=1.0)
    if resp is not None and resp["func"] == const.RSP(const.FC_HANDSHAKE):
        r.set(Verdict.PASS, "broadcast answered on 0x0F func=%02x" % resp["func"])
    else:
        r.set(Verdict.FAIL, f"broadcast no resp={resp}")

    # B7 unknown FC 0x3A -> silence + later alive
    r = _add(results, "B7", "unknown FC -> silence then alive")
    f = build_frame(0x01, 0x3A)
    if not expect_silent(r, f):
        return
    # confirm alive
    link.send_frame(0x01, const.FC_HANDSHAKE)
    resp = link.recv_frame(timeout_s=1.0)
    if resp is not None and resp["func"] == const.RSP(const.FC_HANDSHAKE):
        r.set(Verdict.PASS, "silent for unknown FC and device still alive")
    else:
        r.set(Verdict.FAIL, "device not alive after unknown FC")

    # B8 EXIT_BOOT to App -> ignored (silence) + alive
    r = _add(results, "B8", "EXIT_BOOT(0x09) in App -> silence + alive")
    f = build_frame(0x01, const.FC_EXIT_BOOT)
    if not expect_silent(r, f):
        return
    link.send_frame(0x01, const.FC_HANDSHAKE)
    resp = link.recv_frame(timeout_s=1.0)
    if resp is not None and resp["func"] == const.RSP(const.FC_HANDSHAKE):
        r.set(Verdict.PASS, "EXIT_BOOT ignored in App; device alive")
    else:
        r.set(Verdict.FAIL, "device not alive after EXIT_BOOT")


def _crc(data):
    from zlr.crc32 import crc32
    c = crc32(data)
    return bytes([c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF])
