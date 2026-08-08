#!/bin/bash
# Flash Application to GD32F303RCT6 using JLink GDB Server
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=========================================="
echo "Flashing Application"
echo "Target: GD32F303RCT6"
echo "Address: 0x08006000"
echo "=========================================="

# Kill any existing GDB servers
pkill -f JLinkGDBServer 2>/dev/null || true
sleep 1

# Start JLink GDB Server
JLinkGDBServer -device GD32F303RCT6 -if SWD -speed 1000 -port 2331 -noreset &
sleep 2

# Load App using GDB -> 复位 -> 运行 (烧录后直接复位运行)
arm-none-eabi-gdb -batch \
  -ex "target remote localhost:2331" \
  -ex "load" \
  -ex "monitor reset" \
  -ex "monitor go" \
  -ex "detach" \
  "$SCRIPT_DIR/Build/app.elf" 2>&1

# Cleanup
pkill -f JLinkGDBServer 2>/dev/null || true

echo "Application flashed successfully!"
