"""CRC32 (MPEG-2) — matches firmware App_Param.c:8.

poly=0x04C11DB7, init=0xFFFFFFFF, bit-wise MSB, no reflection, no final XOR.
CRC covers the bytes BEFORE the 4-byte CRC field.
"""


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= (b << 24) & 0xFFFFFFFF
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc
