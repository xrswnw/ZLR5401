

注意：此文档为目的需求，你仔细思考后，生成一个Plan.html于此文件夹，全部权限给你，不必追问过程，我只要结果，计划不执行，等我查看。
执行完成后生成Report.html于本文件夹
#有相关建议可以提出

#需求
#嵌入式
1.目前BOOT APP共有系统级别命令不动，保持
2.APP层做调整，命令从0x20起，如FC_MOTOR_CTRL为0x20,FC_UHF_CTRL为0x21,依次向后扩展
3.存储与持久化这类描述不用再=在协议中表述
4. 设备语义（实测）：这是 消磁器（检测标签→消磁/报警），不读标签唯一 ID。读回全部参数走总查询 0x63（真读回存储值，非回显）；逐参数命令单独发仍是回显。cmd17 是设备主动上报的消磁结果：2A A2 17 01 01 07 5B0 5B1 5B2 5B3 5B4 00 00 校验。5B 字段 FF FF FF FF FF = 消磁失败（标签无法消磁，持续连发重试上报）；单帧/非 FF 字段 = 消磁成功（仅一帧后停）。GET_STATUS 据此分类：≥3 帧连发 FF×5 判失败、单帧/静默判成功、静默 >300ms 复位空闲。链路断线自动探测恢复 (Round_098 优化 #10)：AM 链路状态由解码器突发帧 (cmd17 上报) 静默检测。检测模式运行中若链路已断 (无突发帧且 link≠0)，设备按 2s/4s/8s 指数退避自动发 0x63 探测，恢复成功即自动回到正常检测（上位机无需人工干预；期间 GET_STATUS 的 link 字段如实反映）。链路恢复后消磁/检测业务无感续跑。
波形语义（cmd 0x64）：发 2A A2 64 01 01 02 00 00 68 请求，解码器分 4 包背靠背返回一周期：2A A2 64 04 包次 66 <100点> EF FE（108B，无常规校验，EF FE 收尾）。50Hz 市电 400 点 / 20ms / 每点 50us；RX 窗首点附近即标签回波位置。USB 单帧 ≤52B（63−头11），故分页上传：48 点/页。经 GET_WAVE 触发同步采集（阻塞约 1s）后，用 GET_WAVE_PAGE 逐页拉取；有效点数可能 <400（实时背靠背丢包场景），以各响应 points 为准。等描述移除
5.13.1 0x08 单标签流程（Round_011 已废除）

0x08 子命令不再解析任何帧形状，收到一律回 [cmd, 2] (PARAM)。单标签场景统一改用 0x0A UNLOCK_MULTI：epcCnt=1 即单标，解锁窗 W=120000ms 恰为 2min。废除动机：单标/多标两条通道共享同一光电门控与"命中即升起"逻辑，语义重复且双通道互斥检查徒增状态机分支面；0x08 独有的"保持期 3s 稳定即结账成功"判据与 0x0A 的"逐张确认 + n==m 结束"业务语义不一致，易产生结账误判。保留 0x08 码位显式回 PARAM（区别于未知子命令），便于上位机探测协议升级。移除
Round_011 这类描述移除
地灯语（固件按业务状态自动驱动，不受本命令控制；协议层含义如下）
RGB 灯带由固件按业务状态自动指示（上位机不可控、亦无需控制；语义与硬件无关，仅映射流程状态）：等待放标=白慢闪（该你了）· 盘点校对中=蓝慢闪（该我了）· 解码到一张标签=蓝单闪 · 标签匹配=绿单闪 · 升起开锁=绿常亮 · 失配=红快闪 3s · 软标等待=白常亮 · 软标成功=白单闪 · 未放标/窗满未收齐=黄慢闪 2s · 链路断=红双闪 · 结账完成=绿三连闪+蜂鸣 · 故障=红常亮 · 设备自检类=黄系（回零慢闪/测试快闪/行程错误黄·粉红闪）。手动 SET 仅在空闲时叠加显示。全流程灯语时序详见 App_Unlock_Flow.html。。移除
与既有错误通道的关系
FC_MOTOR_CTRL QUERY/HEALTH 已提供电机细粒度诊断（switchErr、堵转、统计），FC_UHF/AM CTRL QUERY 提供各自链路状态；本命令把它们汇总为设备级一键健康位图，供上位机在发现设备（HANDSHAKE 后）或巡检时快速判断"哪里坏了"，无需逐个外设轮询。锁存位保证 POST 期/历史瞬时故障不丢失。。移除
附录 A. 元数据与常量

// 协议常量
PROTO_HEADER              = 0x7753           // "Sw" LE
PROTO_VERSION             = 2
DEFAULT_DEV_ADDR          = 0x01
BROADCAST_DEV_ADDR        = 0xFF
CRC32_POLY                = 0x04C11DB7 (MPEG-2, 无反射, 无最终 XOR)

// 状态枚举
PARAM_STATUS_IDLE         = 0
PARAM_STATUS_RUN          = 1
PARAM_STATUS_UPG          = 2   // 触发进入 Boot 升级
PARAM_STATUS_FAULT        = 3

// FC 码
FC_HANDSHAKE              = 0x01
FC_ENTER_BOOT             = 0x02
FC_UPGRADE_START          = 0x03   // Boot
FC_FW_DATA                = 0x04   // Boot
FC_UPGRADE_VERIFY         = 0x05   // Boot
FC_UPGRADE_EXEC           = 0x06   // Boot
FC_DEVICE_INFO            = 0x07
FC_RESET                  = 0x08
FC_EXIT_BOOT              = 0x09   // 退出升级 (Boot 处理)
FC_MOTOR_CTRL             = 0x0A   // 步进电机升降机构控制
FC_UHF_CTRL               = 0x0B   // UHF 超高频读写模块控制
FC_AM_CTRL                = 0x0C   // AM 消磁器控制
FC_LOCKER_CTRL            = 0x0D   // 开锁器业务编排
FC_RGB_CTRL               = 0x0E   // RGB 三色指示灯控制

// FC=0x0A 子命令码 (FC_MOTOR_CTRL)
MOTOR_SUB_MOVE            = 0x01   // 启动运动 [dir, steps 24bit]
MOTOR_SUB_STOP            = 0x02   // 停止并关断输出
MOTOR_SUB_SPEED           = 0x03   // 微步/秒 [hz 16bit]
MOTOR_SUB_TORQUE          = 0x04   // 转矩百分比 [pct u8]
MOTOR_SUB_QUERY           = 0x05   // 查询状态/故障/步数
MOTOR_SUB_CLEAR           = 0x06   // 清故障
MOTOR_SUB_TEST            = 0x07   // 电机行程测试 [passes 往返次数]; 进行中回 BUSY
MOTOR_SUB_HEALTH          = 0x08   // 读健康/堵转监测 [olovState, thresh(2), trq(2), reason]
MOTOR_SUB_STATS           = 0x09   // 读运行统计 [runSeconds(3), startCount(2), lastReason]
MOTOR_ERR_OK              = 0
MOTOR_ERR_PARAM           = 1
MOTOR_ERR_FAULT           = 2
MOTOR_ERR_BUSY            = 3      // TEST 进行中
MOTOR_STATE_IDLE          = 0
MOTOR_STATE_RUN           = 1
MOTOR_STATE_FAULT         = 2

// FC=0x0B 子命令码 (FC_UHF_CTRL)
UHF_SUB_OPEN              = 0x01   // 上电 + 下发配置 + 回波检测 -> READY
UHF_SUB_CLOSE             = 0x02   // 停止并下电
UHF_SUB_INVENTORY         = 0x03   // 同步盘点: 0x22多标签盘存(timeout) -> 0x29取回, 标签内联返回
UHF_SUB_READ_TAG          = 0x04   // 读标签 [epcLen, epc.., bank, addr, cnt]
UHF_SUB_WRITE_TAG         = 0x05   // 写标签 [epcLen, epc.., bank, addr, len, data..]
UHF_SUB_STOP              = 0x06   // 停止当前操作
UHF_SUB_QUERY             = 0x07   // 查询 [state, link, totalTags]
UHF_SUB_GET_CONFIG        = 0x08   // 读配置 [power, antenna, checksum, session, target, q, band]
UHF_SUB_SET_CONFIG        = 0x09   // 写配置并持久化 [.., (band)]
UHF_SUB_GET_TAGS          = 0x0A   // 取标签缓冲(数据面, 扫描/盘点取走) [count]
UHF_SUB_GET_STATUS        = 0x0B   // 状态/错误监控 [state, link, totalTags, powered, antennaOk, lastErr, antRl(2), antVswr(2)]
UHF_SUB_CHECK_ANT         = 0x0C   // 主动回波检测 [antennaOk, antRl(2), antVswr(2)]
UHF_SUB_SCAN_START        = 0x0D   // 启动自动扫描 [cycle(2)]: 连续盘点入缓冲, GET_TAGS 拉取, 不主动上报
UHF_SUB_SCAN_STOP         = 0x0E   // 停止自动扫描
UHF_SUB_GET_DUMP          = 0x10   // 0x22/0x29 原始字节诊断 (Round_050/051)
UHF_ERR_OK                = 0
UHF_ERR_PARAM             = 1
UHF_ERR_BUSY              = 2
UHF_ERR_NOT_READY         = 3
UHF_ERR_LINK              = 4
UHF_ERR_NO_TAG            = 5
UHF_ERR_TIMEOUT           = 6
UHF_STATE_IDLE            = 0
UHF_STATE_READY           = 1
UHF_STATE_INVENTORY       = 2
UHF_STATE_GETBUF          = 7
UHF_STATE_READ            = 3
UHF_STATE_WRITE           = 4
UHF_STATE_ERROR           = 5
UHF_PARM_MAGIC            = 0x5548  // userParam UHF 配置魔数

// FC=0x0C 子命令码 (FC_AM_CTRL)
AM_SUB_GET_CONFIG         = 0x01   // 读配置(本地缓存) [thr,hit,freq,delay,len,invert,sync,volt,mode,mains]
AM_SUB_SET_CONFIG         = 0x02   // 写配置并持久化 (同上字段, mains 可省略)
AM_SUB_GET_PARAM          = 0x03   // 读单个参数(本地缓存) [amCmd]
AM_SUB_SET_PARAM          = 0x04   // 写单个参数 [amCmd, valH,valL]
AM_SUB_QUERY              = 0x05   // 探测链路 [link]
AM_SUB_GET_STATUS         = 0x06   // 监控: [link, evt(4B), lastEventMs(4B), deact(0空闲/1成功/2失败)]
AM_SUB_SET_MODE           = 0x07   // 仅切工作模式并持久化 [mode]
AM_SUB_GET_WAVE           = 0x08   // 同步波形采集(0x64, 阻塞~1s): [err, pointsH, pointsL]
AM_SUB_GET_WAVE_PAGE      = 0x09   // 取一页波形(48点/页): [page, err, pointsH, pointsL, w(≤48)]
AM_ERR_OK                 = 0
AM_ERR_PARAM              = 1
AM_ERR_LINK               = 4
AM_ERR_TIMEOUT            = 6

// FC=0x0D 子命令码 (FC_LOCKER_CTRL)
LOCKER_SUB_CONFIGURE      = 0x01   // 清清单/置软标 [hardCnt, softCnt]
LOCKER_SUB_ADD            = 0x02   // 追加硬标签 EPC [epcLen, epc..]
LOCKER_SUB_START          = 0x03   // 上电UHF+开扫
此表移除
5.仅须保留一个干净的协议文档
6.在解锁命令后绘制解锁命令框图，方正简洁，
7，在协议尾部添加RGB灯状态显示
9，协议尾部添加CRC算法和部分示例帧，如握手，盘点、解锁等（1,2,3标签）可扩展