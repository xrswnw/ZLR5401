# JLink调试方案

## 问题
JLink的`loadfile`命令会忽略ELF/hex中的地址信息，始终烧录到默认的flash起始地址(0x08000000)。

## 解决方案

### 方案1：只烧录Bootloader到0x08000000，App暂时不烧录
Boot负责升级流程，调试时只需烧录Boot。

### 方案2：使用OpenOCD烧录（推荐）
OpenOCD可以正确处理ELF中的地址信息。

### 方案3：手动分步烧录
使用JLink的`w4`命令手动写入关键地址

## 推荐操作步骤

### 步骤1：烧录Bootloader（验证正常）
```bash
cat << 'EOF' | JLinkExe -device STM32F103C8 -if SWD -speed 1000 -jtagconf -1,-1
connect
h
erase
loadfile /Users/swnw/Documents/Software/Sccd/Build/boot.elf
g
qc
EOF
```

此时LED应该10ms闪烁一次。

### 步骤2：烧录App到0x08006000（使用GDB）

启动GDB Server:
```bash
JLinkGDBServer -device STM32F103C8 -if SWD -speed 1000 -port 2331
```

在另一个终端启动GDB:
```bash
arm-none-eabi-gdb /Users/swnw/Documents/Software/Sccd/Build/app.elf
```

在GDB中:
```
(gdb) target remote localhost:2331
(gdb) load
(gdb) set $pc=Reset_Handler
(gdb) c
```

### 步骤3：验证烧录
```bash
cat << 'EOF' | JLinkExe -device STM32F103C8 -if SWD -speed 1000 -jtagconf -1,-1
connect
h
mem32 0x08006000, 4
q
EOF
```
应该显示:
- 0x20005000 (Stack)
- 0x0800xxxx (Reset_Handler地址)

### 步骤4：完整流程
1. 上电后Boot运行（10ms LED闪烁）
2. 500ms后跳转到App
3. App中1s LED闪烁

## 调试脚本

### debug_app.jlink - 调试App
```
connect
h
loadfile /Users/swnw/Documents/Software/Sccd/Build/app.elf
setpc Reset_Handler
go
```

### flash_boot.jlink - 只烧录Boot
```
connect
h
erase
loadfile /Users/swnw/Documents/Software/Sccd/Build/boot.elf
g
qc
```
