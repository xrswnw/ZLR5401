# ZLR5401 项目规则与环境

> 本文件为 ZLR5401 子项目专属规则，追加于工作区全局规则（`../CLAUDE.md`）之上，不与其冲突。
> 全局规则（TrashCan 删除、备份轮转、SDK 不可变等）在此不再重复，同等生效。

---

## 1. 工程概述

- **硬件**: GD32F303RCT6（Cortex-M4，256K Flash / 48K RAM），UHF RFID + AM 消磁 + 步进电机锁定机构 + RGB 灯带。
- **双区固件**: Bootloader @ `0x08000000`，App @ `0x08006000`（IAP 升级已验证）。
- **通信**: USB HID（PMA 方案），设备枚举 VID `0x5377` / PID `0x5378`；帧头 `53 77`，CRC-32/MPEG-2 小端。
- **协议版本**: v3（2026-09-03 定稿）：FC 重映射 MOTOR=`0x20`~IO_DIAG=`0x26`，`protoVer=3`；旧 `0x0A`~`0x10` 为废码；解锁通道统一为 `0x0A epcCnt=1` + `GET_PROGRESS`（`0x08 ONE_SHOT` 已废除）。
- **锁引脚定稿**（2026-08-30）：USB_EN=PA8，AM=RS232(PA9/PA10)，UHF=USART3(PB10/PB11)，RGB 蓝灯=PA6。

## 2. 目录结构

| 目录 | 内容 |
|---|---|
| `Application/` | App 固件源码（`src/` `inc/` `ld/`，DRV8434S 电机驱动、STM32_USB） |
| `Bootloader/` | Boot 固件源码（IAP 引导、0x09 自检） |
| `Sdk/` | **基准文件区**：STM32F10x StdPeriph、CherryUSB、arm-none-eabi 运行时、上位机 Python SDK（`zlr5401.py`、协议帧为 CRC-32/MPEG-2） |
| `Agent/Round_NNN/` | 每轮任务的 Demand/Plan/Report + `test/` 真机测试脚本 |
| `Protocol/` | 协议文档（HTML/PDF/docx/pptx）+ 文档生成脚本（Node） |
| `Build/` | cmake 构建目录与固件产物（git 忽略，备份排除） |
| `TrashCan/` | 可恢复删除回收站（全局规则 1） |

## 3. SDK 不可变（全局规则 3 的本项目细化）

- `Sdk/` 全部为基准文件，**严禁直接修改**。
- 确需改 SDK 文件时：复制到 `Application/AppSdk/` 保持相对目录结构，仅改副本，编译优先级取 `AppSdk/`（当前尚无 `AppSdk/`，首次使用时创建并在变更日志记录）。
- 例外注意：`Sdk/zlr5401.py` 是上位机 SDK，被 `Agent/Round_*/test/` 脚本 import 使用，同样不可改；测试需求在各自 `test/` 下另行封装。

## 4. 构建与烧录

```bash
./build_all.sh          # cmake -B Build + 编译 app/boot + gen_firmware.sh all
./build_app.sh           # 只编 App
./build_boot.sh          # 只编 Boot
./gen_firmware.sh all    # 产物 → Build/Firmware/: Boot.hex / App_<ts>.bin|hex / ZLR5401_<ts>.hex(合并)
```

- 工具链：`arm-none-eabi-gcc`（Homebrew，toolchain 见 `toolchain-arm-none-eabi.cmake`，Cortex-M4 `-mthumb`）。
- 烧录（JLink，目标 GD32F303RCT6，SWD）：
  - `./flash_all.sh` — 用最新合并 `ZLR5401_*.hex` 一次烧全片（Boot+App），烧后复位运行。
  - `./flash_app.sh` / `./flash_boot.sh` — JLinkGDBServer + `arm-none-eabi-gdb` 单区烧录。
- 调试：`.vscode/openocd_daplink.cfg`（注意该 cfg 按 STM32F103 target 配置，实际芯片为 GD32F303 兼容流）。

## 5. 上位机测试环境

- Python 3（Homebrew）+ `hidapi` + `pyserial`；工程根有 `.venv/`（已 git/备份排除）。
- SDK 入口 `Sdk/zlr5401.py`：USB HID 自动发现设备，UART 备选。
- 真机测试脚本在 `Agent/Round_NNN/test/`，跑真机闭环；**UHF 单轮盘点约 1/4 漏读属正常**，测试脚本须多轮重试；时序坑（迟到帧、0x22 忙窗口）已由固件侧修复。

## 6. 协议文档生成（Protocol/）

- Node 依赖：`docx` + `pptxgenjs`（`Protocol/package.json`；`node_modules/` 已 git 忽略、**备份白名单内排除**）。
- 生成脚本：`generate_docx.js`、`generate_pptx_v1.js`、`generate_pptx_v2.js`、`patch_footers.py`。
- 首次使用先 `cd Protocol && npm install`。

## 7. 备份专项（全局规则 2 的本项目细化）

- 每轮变更前**必须** `./backup.sh "本轮变更说明"`（脚本内含白名单、32 层轮转、500M 上限、验证与日志）。
- 备份卷为 **exFAT**：macOS 会为每个新写入的文件自动生成 `._*` 边车（provenance 机制，复制工具层面无法禁止），脚本在复制完成后统一清除（2026-09-04 用户授权）。
- `node_modules` 在白名单目录内排除；`Build/`、`.venv/`、`Sdk/` 不在白名单。

---

*创建: 2026-09-04（Round_109，环境补充轮）*
