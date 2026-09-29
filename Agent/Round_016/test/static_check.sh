#!/bin/bash
# Round_016 静态检查: 固件源码裁决项 (sim 不编译 Dispatch/参数头, 在此逐项校验)
# v3 statusBits 落地 + Round_015 裁决回归 (电机 2000 等)
cd "$(dirname "$0")/../../.."
fail=0
ck() { # ck <描述> <grep 表达式> <文件>
  if grep -q "$2" "$3"; then echo "  ok: $1"; else echo "  FAIL: $1"; fail=1; fi
}

echo "== Round_016 裁决静态检查 =="
ck "LSEEK_SPEED_HZ=2000"        'define LSEEK_SPEED_HZ *2000u'   Application/inc/App_LockerSeek.h
ck "HOMING_SPEED_HZ=2000"       'define HOMING_SPEED_HZ *2000u'  Application/src/App_MotorHoming.c
ck "MT_TEST_SPEED_HZ=2000"      'define MT_TEST_SPEED_HZ *2000u' Application/src/App_MotorTest.c
ck "SetSpeedHz 上限注释 2000"   '1~2000'                         Application/inc/App_Stepper.h
ck "外来缓存容量 10"            'define UNLK_MAX_FGN_TAGS *10u'  Application/inc/App_LockerUnlock.h
ck "EPC 名单容量 10"            'define EPC_MAX_TAGS *10u'        Application/inc/App_LockerUnlock.h

# ---- Round_016 v3 statusBits ----
ck "UNLK_ST_BUSY 定义"    'define UNLK_ST_BUSY *0x01u'      Application/inc/App_LockerUnlock.h
ck "UNLK_ST_UHF 定义"     'define UNLK_ST_UHF *0x80u'       Application/inc/App_LockerUnlock.h
if grep -q 'UNLK_FLG_' Application/inc/App_LockerUnlock.h Application/src/App_LockerUnlock.c Application/src/App_Dispatch.c 2>/dev/null; then
  echo "  FAIL: 旧 v2 flags 宏残留"; fail=1
else
  echo "  ok: 旧 v2 flags 已移除"
fi
ck "Dispatch 0x09 九字节" 'uint8_t r\[9\];'                 Application/src/App_Dispatch.c
ck "0x0F v3 status 注释"  'Round_016 v3: statusBits (原 phase)' Application/src/App_LockerUnlock.c

# 解锁期间仅放行 GET_PROGRESS / CANCEL (无 QUERY)
if grep -n 'App_LockerUnlock_IsBusy()' Application/src/App_Dispatch.c | head -1 >/dev/null && \
   sed -n '/App_LockerUnlock_IsBusy() &&/,+2p' Application/src/App_Dispatch.c | grep -q 'LOCKER_SUB_GET_PROGRESS' && \
   ! sed -n '/App_LockerUnlock_IsBusy() &&/,+2p' Application/src/App_Dispatch.c | grep -q 'LOCKER_SUB_QUERY'; then
  echo "  ok: 解锁门禁仅 GET_PROGRESS/CANCEL (QUERY 已 BUSY)"
else
  echo "  FAIL: 解锁门禁白名单异常"; fail=1
fi

exit $fail
