"""Frame build / parse + response func derivation (Protocol section 1-2)."""
from .crc32 import crc32
from . import const


def build_frame(dev_addr, func, data=b"", reserved=const.RESERVED):
    data = bytes(data)
    payload = 1 + len(data) + 4  # func + data + crc32
    total = 2 + 2 + 2 + payload  # header(2) + addr+reserved(2) + len(2) + payload
    b = bytearray(total)
    b[0] = 0x53
    b[1] = 0x77
    b[2] = dev_addr
    b[3] = reserved
    b[4] = payload & 0xFF
    b[5] = (payload >> 8) & 0xFF
    b[6] = func
    b[7:7 + len(data)] = data
    crc = crc32(bytes(b[:total - 4]))
    b[total - 4] = crc & 0xFF
    b[total - 3] = (crc >> 8) & 0xFF
    b[total - 2] = (crc >> 16) & 0xFF
    b[total - 1] = (crc >> 24) & 0xFF
    return bytes(b)


def parse_frame(buf):
    """Parse a protocol frame from a raw byte string (after report-ID strip).

    Returns a dict or None. buf may contain trailing bytes (we consume
    exactly one frame from the front and return the leftover).
    """
    n = len(buf)
    if n < 11:
        return None, buf
    if buf[0] != 0x53 or buf[1] != 0x77:
        return None, buf
    if buf[3] != 0x00:
        return None, buf
    payload = buf[4] | (buf[5] << 8)
    total = 6 + payload
    if payload < 5 or total > n:
        return None, buf
    recv_crc = (buf[total - 4] | (buf[total - 3] << 8)
                | (buf[total - 2] << 16) | (buf[total - 1] << 24))
    calc = crc32(bytes(buf[:total - 4]))
    if calc != recv_crc:
        return None, buf
    f = {
        "devAddr": buf[2],
        "reserved": buf[3],
        "length": payload,
        "func": buf[6],
        "data": bytes(buf[7:total - 4]),
        "crc32": recv_crc,
        "_raw": bytes(buf[:total]),
    }
    return f, buf[total:]


def rsp_func(req_func):
    return req_func ^ 0xFF
