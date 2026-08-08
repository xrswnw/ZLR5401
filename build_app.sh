#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f Build/Makefile ]; then
    [ -L compile_commands.json ] && rm -f compile_commands.json
    cmake -B Build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain-arm-none-eabi.cmake
fi

cmake --build Build --target app.elf
"$SCRIPT_DIR/gen_firmware.sh" app

echo "Application build done + firmware generated."
