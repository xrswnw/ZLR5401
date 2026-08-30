"""Round_009 闭环更新测试 — 共享配置.

复用 Agent/Round_008/test/zlr (hid_link / frame / crc32) 基础设施。
"""
import os

# ---- 设备 / 传输 ----
VID = 0x5377
PID = 0x5378
DEV_ADDR = 0x01

# App 区基址 (Boot_Config.h), 用于 DATA 偏移与向量断言
APP_FLASH_ORIGIN = 0x08006000

# ---- Boot 升级命令 (对应 Boot_CustomProtocol.h) ----
FC_HANDSHAKE = 0x01
FC_ENTER_BOOT = 0x02
FC_UPGRADE_START = 0x03
FC_FW_DATA = 0x04
FC_UPGRADE_VERIFY = 0x05
FC_UPGRADE_EXEC = 0x06
FC_DEVICE_INFO = 0x07
FC_RESET = 0x08
FC_EXIT_BOOT = 0x09
RSP = lambda fc: fc ^ 0xFF

# ---- START (0x03) 结果码 ----
UPG_NO_SPACE = 1
UPG_ERASE_FAIL = 2

# ---- DATA (0x04) 结果码 ----
DATA_FLASH_FAIL = 2
DATA_ADDR_ERR = 3

# ---- VERIFY (0x05) 结果码 ----
VERIFY_CRC_MISMATCH = 1
VERIFY_SIZE_MISMATCH = 2
VERIFY_VECTOR_INVALID = 3
VERIFY_BIND_FAIL = 4

# ---- EXIT_BOOT (0x09) 结果码 ----
EXIT_BOOT_NO_APP = 1
EXIT_BOOT_CRC_MISMATCH = 2

# ---- 校验级别 ----
VERIFY_LEVEL_BASIC = 1
VERIFY_LEVEL_MID = 2
VERIFY_LEVEL_FULL = 3

# ---- 升级参数 ----
# USB HID 单帧 data<=56；FC_FW_DATA 头 4B(seq2+addrOffset2) => 数据区 <=52。
# 取 48B（4 对齐）以免除跨字边界处理。
CHUNK = 48
# 块级重试次数; 帧静默丢弃时按超时处理
TX_TIMEOUT = 1.5
CHUNK_RETRY = 5
REQ_TIMEOUT = 2.0

# ---- 重枚举等待 ----
REOPEN_MAX = 40         # 每 0.2s 一次
REOPEN_SLEEP = 0.25
BUMP_WAIT = 1.0         # 进入/退出 boot 后的静置

# ---- 固件 ----
FW_BIN = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "Build", "app.bin"))


def handshake_upgrade_count(d):
    """握手 data: layer=[19], upgradeCount=[20..23] LE."""
    return int.from_bytes(d[20:24], "little")


def handshake_layer(d):
    return d[19] if len(d) > 19 else -1
