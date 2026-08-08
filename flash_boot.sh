#!/bin/bash
# Flash Bootloader to GD32F303RCT6 using JLink Commander
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

JLinkExe="/Applications/SEGGER/JLink/JLinkExe"

echo "=========================================="
echo "Flashing Bootloader"
echo "Target: GD32F303RCT6"
echo "Address: 0x08000000"
echo "=========================================="

# Kill any existing servers
pkill -f JLinkGDBServer 2>/dev/null || true
pkill -f openocd 2>/dev/null || true
sleep 1

TMPFILE=$(mktemp /tmp/jlink_boot.XXXXXX)
cat > "$TMPFILE" <<JEOF
device GD32F303RCT6
si SWD
speed 4000
loadbin $SCRIPT_DIR/Build/boot.bin 0x08000000
r
g
exit
JEOF

"$JLinkExe" -CommandFile "$TMPFILE"
rm -f "$TMPFILE"

echo "Bootloader flashed successfully!"
