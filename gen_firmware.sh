#!/bin/bash
# ============================================================================
#  ZLR5401 固件生成脚本
#
#  用法:
#    ./gen_firmware.sh app     # 只生成 App 产物
#    ./gen_firmware.sh boot    # 只生成 Boot 产物
#    ./gen_firmware.sh all     # 生成 Boot + App + 合并 ZLR5401_*.hex
#
#  产物 (Build/Firmware/):
#    Boot.bin                  # BOOT 原始二进制
#    Boot.hex                  # BOOT HEX
#    App_<YYYYMMDDHHMM>.bin    # OTA 升级用固件
#    App_<YYYYMMDDHHMM>.hex    # App HEX
#    ZLR5401_<YYYYMMDDHHMM>.hex # Boot+App 合并 HEX (新 MCU 全新下载)
#
#  本脚本只出文件, 不烧录. 烧录由 flash_*.sh 独立负责.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/Build"
FW_DIR="${BUILD_DIR}/Firmware"
TS="$(date +%Y%m%d%H%M)"

MODE="${1:-all}"
if [[ "$MODE" != "app" && "$MODE" != "boot" && "$MODE" != "all" ]]; then
    echo "ERROR: mode must be app|boot|all, got: $MODE" >&2
    exit 1
fi

mkdir -p "$FW_DIR"

gen_app() {
    local src_bin="${BUILD_DIR}/app.bin"
    local src_hex="${BUILD_DIR}/app.hex"
    if [ ! -f "$src_bin" ] || [ ! -f "$src_hex" ]; then
        echo "ERROR: missing Build/app.bin or Build/app.hex, run build_app.sh first" >&2
        return 1
    fi
    local out_bin="${FW_DIR}/App_${TS}.bin"
    local out_hex="${FW_DIR}/App_${TS}.hex"
    cp -p "$src_bin" "$out_bin"
    cp -p "$src_hex" "$out_hex"
    echo "    [app ] $(basename "$out_bin")  ($(du -h "$out_bin" | cut -f1))"
    echo "    [app ] $(basename "$out_hex")  ($(du -h "$out_hex" | cut -f1))"
}

gen_boot() {
    local src_bin="${BUILD_DIR}/boot.bin"
    local src_hex="${BUILD_DIR}/boot.hex"
    if [ ! -f "$src_bin" ] || [ ! -f "$src_hex" ]; then
        echo "ERROR: missing Build/boot.bin or Build/boot.hex, run build_boot.sh first" >&2
        return 1
    fi
    local out_bin="${FW_DIR}/Boot.bin"
    local out_hex="${FW_DIR}/Boot.hex"
    cp -p "$src_bin" "$out_bin"
    cp -p "$src_hex" "$out_hex"
    echo "    [boot] $(basename "$out_bin")  ($(du -h "$out_bin" | cut -f1))"
    echo "    [boot] $(basename "$out_hex")  ($(du -h "$out_hex" | cut -f1))"
}

merge_firmware() {
    local boot_hex="${FW_DIR}/Boot.hex"
    local app_hex="${FW_DIR}/App_${TS}.hex"
    if [ ! -f "$boot_hex" ] || [ ! -f "$app_hex" ]; then
        echo "ERROR: missing Boot.hex or App_${TS}.hex for merge" >&2
        return 1
    fi
    local out="${FW_DIR}/ZLR5401_${TS}.hex"
    # Intel HEX 允许多段地址记录; Boot 从 0x08000000, App 从 0x08006000, 不重叠.
    # 去掉各文件末尾的 EOF 记录 (:00000001FF), 拼接后补一条 EOF.
    {
        grep -v '^:00000001FF$' "$boot_hex"
        grep -v '^:00000001FF$' "$app_hex"
        echo ':00000001FF'
    } > "$out"
    echo "    [zlr ] $(basename "$out")  ($(du -h "$out" | cut -f1))"
}

echo "==> 生成固件 (mode=${MODE}, timestamp=${TS})"
case "$MODE" in
    app)
        gen_app
        ;;
    boot)
        gen_boot
        ;;
    all)
        gen_boot
        gen_app
        merge_firmware
        ;;
esac

echo "==> 完成. 产物目录: ${FW_DIR}"
ls -1 "$FW_DIR" | sed 's/^/    /'
