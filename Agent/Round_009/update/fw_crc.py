"""固件镜像 CRC — 必须与 Bootloader Crc32Calc 精确一致.

Boot_Param.c:8-20:
    uint32_t crc = 0xFFFFFFFF;
    for each byte: crc ^= byte<<24; 8x(msb? (crc<<1)^0x04C11DB7 : crc<<1)
    return crc;   // 无最终 XOR

即 CRC-32/MPEG-2 (MSB-first, init=0xFFFFFFFF, poly=0x04C11DB7, 无反射, 无 final XOR)。
注意与帧级 crc32 (MPEG-2"无反射"帧校验) 是【同一个算法】, 但用途不同:
- zlr/crc32.py 计算"协议帧"的 CRC;
- 本模块计算"固件镜像"的 CRC, 二者都是 MPEG-2, 可复用同一实现。
"""
from zlr import crc32

# zlr/crc32.py 的 crc32() 即 MPEG-2。为明确与独立, 这里再实现一份并断言一致。
def _mpeg2(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = (crc << 1) ^ 0x04C11DB7
            else:
                crc <<= 1
            crc &= 0xFFFFFFFF
    return crc


_checked = False


def ensure_crc_consistency():
    """自检: 本地 MPEG-2 实现须与 zlr/crc32.py 一致 (皆帧级 CRC 基底)。"""
    global _checked
    if _checked:
        return
    for payload in (b"", b"123456789", bytes(range(64))):
        assert _mpeg2(payload) == crc32.crc32(payload), "fw_crc 与 zlr/crc32 不一致!"
    _checked = True


def fw_crc(data: bytes) -> int:
    """固件镜像 Crc32Calc (MPEG-2, MSB-first, init 0xFFFFFFFF, 无 final XOR)."""
    ensure_crc_consistency()
    return _mpeg2(data)


if __name__ == "__main__":
    import sys
    ensure_crc_consistency()
    print("CRC-32/MPEG-2 self-check PASS")
    if len(sys.argv) > 1:
        blob = open(sys.argv[1], "rb").read()
        print(f"file={sys.argv[1]} size={len(blob)} crc=0x{fw_crc(blob):08X}")
