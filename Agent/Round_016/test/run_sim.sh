#!/bin/bash
# Round_016 宿主机模拟测试: 0x10/0x11 状态机黑盒断言 (无需真机)
# 增量: 0x09 v3 九字节 statusBits 布局 (P 系列全部改位图断言)
#       + V3 系列: 0x0F 受理帧 status / 空闲全零 / 0x11 仅 BUSY|TAG
#       保留 Round_014/015 全部 E/A/M 行为断言回归
cd "$(dirname "$0")"
clang -I ../../../Application/inc -Wall sim_stubs.c sim_main.c \
    ../../../Application/src/App_LockerUnlock.c -o sim || exit 1
./sim
