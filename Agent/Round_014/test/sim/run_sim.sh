#!/bin/bash
# Round_014 宿主机模拟测试: 0x10/0x11 状态机黑盒断言 (无需真机)
cd "$(dirname "$0")"
clang -I ../../../Application/inc -Wall sim_stubs.c sim_main.c \
    ../../../Application/src/App_LockerUnlock.c -o sim || exit 1
./sim
