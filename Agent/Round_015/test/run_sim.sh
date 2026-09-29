#!/bin/bash
# Round_015 宿主机模拟测试: 0x10/0x11 状态机黑盒断言 (无需真机)
# 增量: 4000Hz 时序 / 外来缓存 10 / 名单满载 / AM 挂起失败上报
cd "$(dirname "$0")"
clang -I ../../../Application/inc -Wall sim_stubs.c sim_main.c \
    ../../../Application/src/App_LockerUnlock.c -o sim || exit 1
./sim
