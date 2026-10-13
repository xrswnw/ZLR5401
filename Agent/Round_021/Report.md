# Round_021 报告：RF 解码器接入 — 一版固件参数双模（固件侧落地）

> 2026-10-13。用户双裁决（基数计数 + decoderType 双模）的实施轮。
> 产物：`Build/Firmware/ZLR5401_202610132113.hex`（待真机）。

## 一、交付内容

### 1. 架构（App_AM API 不变 → 分发层 → 双后端 → 共用 AM_HL）

```
0x11/0x0A 流程层、Dispatch(AM_CTRL 0x22)、BootSelfTest —— 继续走 App_AM API
        │
App_AM.c（新分发层：公共符号属主 + decoderType 单选 + 切换重建）
        │  DecoderBkOps_t 接口（App_AM_Backend.h）
        ├── App_AM_Backend.c = 原 App_AM.c 平移（git mv，AmBk_*，2A A2/cmd17/0x63/0x64）
        └── App_AM_RF.c = RF 后端新写（B5 A1 / 0x3A 计数 / 0x01 单帧查询 / 0x06 模式）
        │
共用 App_AM_HL（USART1 RS232 115200 + 环形缓冲 + 帧定位/分包）
  —— 帧头改运行时注入 AM_HL_SetHeader（AM 2A A2 / RF B5 A1）
```

关键文件变更：

| 文件 | 变更 |
|---|---|
| `Application/src/App_AM.c` | 重写为分发层（原实现平移走） |
| `Application/src/App_AM_Backend.c` | git mv 自 App_AM.c，符号 AmBk_*，逻辑零改动（见等价性审计） |
| `Application/src/App_AM_RF.c` | 新写 RF 后端 |
| `Application/inc/App_AM_Backend.h` | 新增后端接口 + RF 帧头常量 |
| `Application/inc/App_AM_HL.h` `.c` | 帧头参数化注入；PAYLOAD_MAX 16→40（RF 0x01 响应 39B），FRAME_MAX 22→47 |
| `App_Param.h/.c` | userParam 偏移 40 新增 DecParam（magic 'DC'，4B，type 0/1） |
| `App_CustomProtocol.h` | AM_SUB_GET_DECODER 0x0A / AM_SUB_SET_DECODER 0x0B 定义 |
| `App_Dispatch.c` | SET_MODE 改走逻辑接口；新增 0x0A/0x0B；RF 模式下 AM 专属码位（0x01/0x02/0x08/0x09）回 PARAM；busy 门禁放行 GET_DECODER |
| `App_LockerUnlock.c` | 5 处 `App_AM_SetParam(AM_CMD_MODE,…)` → `App_AM_SetMode(…)`（机械替换，语义不变：值域 0-2 两族一致） |
| `App_BootSelfTest.c` | probe_am 按 decoderType 显示 AM/RF 标签（链路位共用 SELF_ERR_AM_COMM） |
| `Agent/Round_021/test/decoder_dual.py` | 真机验收脚本（D1-D11，AM 侧全量可跑，RF 段按在场自动 SKIP） |

### 2. RF 协议事实（XLS V1.01 全 7 表逐帧核对，此前仅梳理级）

- **0x01 查询**：请求 `B5 A1 01 01 01 01 00 Sum_L`（datalen=1，与 AM 0x63 的 datalen=2 不同）；响应 `[版本2][ID5][报警次数4][N参数按命令升序]`，N=28（0x05..0x20 连续含预留），全帧 39B 数据 → HL 缓冲扩容依据。
- **写**：回显式 `B5 A1 cmd 01 01 01 <值> Sum_L`，参数均单字节；0x19 恢复出厂 / 0x1A 测试报警为动作帧（值 0，透传放行）。
- **无逐参读**：读只能走 0x01 全量后按位抽取（偏移 11+cmd-0x05）——GetParam 每次全量查询。
- **0x3A 报警**：`B5 A1 3A 01 01 05 年 月 日 序列H 序列L Sum_L`（设备 ID，无标签身份）；**0x04 心跳**：同 ID + 噪声字节（高 4 位线圈 1/低 4 位线圈 2），15min——固件与 AM 同口径静默丢弃。
- **0x06 状态**：0=检测解码/1=只检测/2=休眠，与 AM 0x50 值域一一对应 → 0x11 流程受理（切 0）/出口（切 1）/模式看门狗（幂等重申）经 `App_AM_SetMode` 原生复用，流程层零语义改动。

### 3. 切换语义（0x0B）

- busy 门禁（0x10/0x11 流程中回 BUSY；GET_DECODER 0x0A 与 GET_STATUS 同为本地读放行）；
- 切换 = `AM_HL_DeInit/Init`（清半收帧）+ 后端重建（计数清零，两族不可混账）+ `DecParam_Save` 持久化 + 联动探链（回 `[cmd,err,type,link]`）；
- 接错外设 = 探链失败如实上报（link≠0），不自适应；幂等（同类型直回 OK）；
- 旧参数区（userParam[40..43] 无 'DC' magic）→ 读出 AM=0，**现网升级零回归锚点**；
- 上电按参数区取值；AM 参数持久化（AmParam）仅 AM 模式生效，RF 参数存于解码器自身（与上电零下发裁决一致）。

### 4. AM 侧零回归证据

1. **函数体等价性审计**（脚本对比 HEAD 原版 vs 平移版）：17 个公共函数 16 个逐字节一致；唯一差异 = Init 的结构性三处（AM_HL_Init 上移分发层 / AM_HL_SetHeader 注入 2A A2 / 探链字段显式复位——冷启动行为等价）；内部函数 am_cache_by_cmd / am_query_all 逐字节一致；AmBk_SetMode 等价原 `App_AM_SetParam(0x50, mode)`。
2. **一处有意回退**：GET_PARAM 的 PARAM 错误映射收紧被撤销——Round_098 `p5_am.py` AM4a 对非法 amCmd 断言 `err==0`（旧口径 PARAM 落空为 OK+值0）已固化，AM 回归红线优先；RF 模式同口径（err 落空），由协议文档双表约束主机。
3. Bootloader 文件区独立（Boot_Param/Boot_*），零影响；boot.elf 随 build_all 通过。
4. 编译告警仅剩预存在项（IO_DIAG 初始化元素超额 + LockerUnlock 0x0A 保留死码），与本次改动无交集（git diff 证实）。

### 5. 待样机确认项（固件侧已隔离为常量，不阻塞交付）

| # | 项 | 固件锚点 |
|---|---|---|
| 1 | 0x3A 突发节奏与去活化静默判据 | `RF_BURST_GAP_MS`（缺省 3000 同 AM，实测后单独重标定） |
| 2 | 0x01 报警次数口径（一帧一计/一报警一计） | 暂不参与结账，对账参考 |
| 3 | Sum_L 校验算法实测核对 | 算法同 AM（除头尾累加低位），不一致时仅改 AM_HL 内核 |
| 4 | 成对参数（阈值/增益/能量/特征/延时）默认值 | 上位机经 SET_PARAM 透传下发（0x05~0x20） |
| 5 | RF 自带 LED 配置为灭 | `SET_PARAM(0x16, 6)`（常亮组合 6=灭），host 侧配置 |

## 二、验证状态

- 编译：`build_all.sh` 通过，产物 `ZLR5401_202610132113.hex`（App 76KB text / 232KB 区域）。
- 真机：**未跑**（本轮收尾时真机不在线）——`decoder_dual.py` D1-D11 待真机执行；AM 侧全量回归 + RF 侧（样机到位后）为交付验收门槛。
- 备份：`Round_122_20261013_210923`（开工前快照）。

## 三、连带事项（未在本轮）

- 客户协议文档：decoderType 命令（0x0A/0x0B）+ RF 命令字双表章节 → 下轮文档轮。
- 合同条款草案按"一版固件双模"改写（删 ZLR5402 二选一）→ 客户目录 RF/。
- SDK `zlr5401.py`（基准区不可改）：测试脚本已本地定义新常量，不动 SDK。
