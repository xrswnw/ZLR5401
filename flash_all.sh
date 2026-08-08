#!/bin/bash
# Flash both Bootloader + Application to GD32F303RCT6
# 使用最新合并固件 Build/Firmware/ZLR5401_<ts>.hex 一次性烧全片
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

JLinkExe="/Applications/SEGGER/JLink/JLinkExe"

echo "=========================================="
echo "Flashing Bootloader + Application"
echo "Target: GD32F303RCT6"
echo "=========================================="

# 取最新的 ZLR5401_*.hex (build_all.sh 的产物)
LATEST_HEX=$(ls -1t "$SCRIPT_DIR/Build/Firmware/ZLR5401_"*.hex 2>/dev/null | head -1)
if [ -z "$LATEST_HEX" ]; then
    echo "Error: no ZLR5401_*.hex found in Build/Firmware/, run ./build_all.sh first" >&2
    exit 1
fi
echo "Using: $(basename "$LATEST_HEX")"

# Kill any existing servers
pkill -f JLinkGDBServer 2>/dev/null || true
pkill -f openocd 2>/dev/null || true
sleep 1

# 单 JLink 烧全片 (Boot+App), 烧后复位运行
TMPFILE=$(mktemp /tmp/jlink_all.XXXXXX)
cat > "$TMPFILE" <<JEOF
device GD32F303RCT6
si SWD
speed 4000
loadfile $LATEST_HEX
r
g
exit
JEOF

"$JLinkExe" -CommandFile "$TMPFILE"
rm -f "$TMPFILE"

echo ""
echo "=========================================="
echo "All flashed successfully!"
echo "Bootloader: 0x08000000"
echo "Application: 0x08006000"
echo "=========================================="
