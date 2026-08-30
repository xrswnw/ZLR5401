"""Layer A — protocol self-test (no hardware). Validates the script itself."""
from zlr.crc32 import crc32
from zlr.frame import build_frame, parse_frame, rsp_func
from zlr import const
from zlr.result import Verdict

SECTION = "A"


def run(results, link, args):
    # A1 CRC32 known-data
    r = _add(results, "A1", "CRC32 known data & coverage")
    hf = build_frame(0x01, const.FC_HANDSHAKE)
    # handshake frame must be: 53 77 01 00 05 00 01 [crc]
    if hf[:7] == bytes.fromhex("53770100050001") and len(hf) == 11:
        r.set(Verdict.PASS, "handshake frame header/payload correct: %s" % hf.hex())
    else:
        r.set(Verdict.FAIL, "frame header mismatch: %s" % hf.hex())

    # A2 frame encode/decode round-trip
    for name, fc, data in [
        ("empty", 0x01, b""),
        ("motor", 0x0A, bytes([0x02])),
        ("uhf", 0x0B, bytes([0x01])),
        ("locker", 0x0D, bytes([0x01, 0, 0, 1, 0])),
        ("rgb", 0x0E, bytes([0x01, 0x07, 0])),
    ]:
        r = _add(results, "A2", f"round-trip {name}")
        f = build_frame(0x01, fc, data)
        p, leftover = parse_frame(f)
        if p and p["func"] == fc and p["data"] == data and leftover == b"":
            r.set(Verdict.PASS, f"{name}: ok")
        else:
            r.set(Verdict.FAIL, f"{name}: mismatch p={p} leftover={leftover}")

    # A3 response func derivation
    r = _add(results, "A3", "response func = req ^ 0xFF")
    ok = (rsp_func(0x01) == 0xFE and rsp_func(0x0A) == 0xF5 and rsp_func(0x0D) == 0xF2)
    r.set(Verdict.PASS if ok else Verdict.FAIL,
          "0x01->0xFE, 0x0A->0xF5, 0x0D->0xF2" if ok else "derivation wrong")


def _add(results, name, desc):
    from zlr.result import Result
    r = Result(name, SECTION, "A")
    r.message = desc
    results.append(r)
    return r
