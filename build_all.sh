#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# 首次配置: 若 Build/Makefile 不存在则 cmake configure
if [ ! -f Build/Makefile ]; then
    # 清理可能存在的悬空 compile_commands.json 符号链接 (CMake configure 会重建)
    [ -L compile_commands.json ] && rm -f compile_commands.json
    cmake -B Build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain-arm-none-eabi.cmake
fi

# 编译 app + boot (默认 all target)
cmake --build Build

# 编译成功后调用固件生成 (all 模式: Boot + App + ZLR5401_*.hex)
"$SCRIPT_DIR/gen_firmware.sh" all

echo "All build done + firmware generated."
