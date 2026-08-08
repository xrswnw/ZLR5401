# GDB init script for debugging Application (skip Bootloader)
# Use AIRCR SYSRESETREQ for system reset (ARM standard, no J-Link dependency)
# Then halt, load, reset again, and jump to App's Reset_Handler

# AIRCR = 0xE000ED0C, VECTKEY=0x05FA, SYSRESETREQ=bit2
# Writing 0x05FA0004 triggers a system reset

# 1. System reset via AIRCR
set *0xE000ED0C = 0x05FA0004

# 2. Halt the CPU after reset
monitor halt

# 3. Load the App ELF
load

# 4. System reset again — clean RCC state
set *0xE000ED0C = 0x05FA0004
monitor halt

# 5. Skip Bootloader: set VTOR, PC and SP
set *0xE000ED08 = 0x08006000
set $pc = Reset_Handler
set $sp = (unsigned long)_estack

# 6. Break at main and run
tbreak main
continue
