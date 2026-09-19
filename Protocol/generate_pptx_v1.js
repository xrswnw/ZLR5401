// ZLR5401 应用层通信协议 — 客户版 PPT (V1.0 未定稿)
// 灰白色系 · 每条命令一页 · 含解锁流程图与灯控映射表
const pptxgen = require("pptxgenjs");

const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE";
pres.author = "ZLR5401";
pres.title = "ZLR5401 应用层通信协议 V1.0";

// ───── 灰白配色 ─────
const C = {
  bg: "FFFFFF",
  ink: "262626",      // 正文
  dark: "3D3D3D",    // 标题
  giant: "111111",    // 焦点十六进制
  muted: "8A8A8A",
  faint: "B8B8B8",
  headFill: "E4E4E4",
  rowFill: "F7F7F7",
  border: "C9C9C9",
  panelFill: "F2F2F2",
  coverBg: "262626",
};
const F = "Microsoft YaHei";
const W = 13.33, H = 7.5, M = 0.6;

const TB = { pt: 0.75, color: C.border };

// 参数表: rows = [[字段, 字节, 说明], ...]
function paramTable(slide, rows, x, y, w, colW) {
  const header = [
    { text: "字段", options: { bold: true, fill: { color: C.headFill }, color: C.dark, fontSize: 12.5 } },
    { text: "字节", options: { bold: true, fill: { color: C.headFill }, color: C.dark, fontSize: 12.5 } },
    { text: "说明", options: { bold: true, fill: { color: C.headFill }, color: C.dark, fontSize: 12.5 } },
  ];
  const body = rows.map((r, i) => r.map((c, j) => ({
    text: String(c),
    options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12, align: j === 1 ? "center" : "left" },
  })));
  slide.addTable([header, ...body], {
    x, y, w, colW: colW || [1.55, 0.85, w - 2.4],
    border: TB, fontFace: F, margin: 0.06, valign: "middle", rowH: 0.34,
  });
}

// 双列小表 (错误码等)
function kvTable(slide, rows, x, y, w) {
  const header = [
    { text: "值", options: { bold: true, fill: { color: C.headFill }, color: C.dark, fontSize: 12.5, align: "center" } },
    { text: "含义", options: { bold: true, fill: { color: C.headFill }, color: C.dark, fontSize: 12.5 } },
  ];
  const body = rows.map((r, i) => [
    { text: String(r[0]), options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12, align: "center" } },
    { text: String(r[1]), options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12 } },
  ]);
  slide.addTable([header, ...body], {
    x, y, w, colW: [0.9, w - 0.9], border: TB, fontFace: F, margin: 0.06, valign: "middle", rowH: 0.32,
  });
}

// 页眉: 左标题 + 右侧小页标
function pageHead(slide, title, tag) {
  slide.addText(title, { x: M, y: 0.34, w: 9.6, h: 0.55, fontSize: 25, bold: true, color: C.dark, fontFace: F, margin: 0 });
  if (tag) slide.addText(tag, { x: W - M - 3.4, y: 0.42, w: 3.4, h: 0.4, fontSize: 12, color: C.muted, fontFace: F, align: "right", margin: 0 });
}

// 命令页统一模板
function cmdPage({ head, tag, hex, desc, reqRows, rspRows, note }) {
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, head, tag);
  // 左栏: 焦点十六进制 + 说明
  s.addText(hex, { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 60, bold: true, color: C.giant, fontFace: F, margin: 0 });
  s.addText(desc || "", { x: M, y: 2.55, w: 3.5, h: 3.3, fontSize: 13.5, color: C.ink, fontFace: F, margin: 0, valign: "top", lineSpacingMultiple: 1.25 });
  // 右栏: 请求 / 响应表
  let y = 1.25;
  const tw = W - M - 4.55;
  if (reqRows) {
    s.addText("请求 data", { x: 4.55, y, w: tw, h: 0.32, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
    paramTable(s, reqRows, 4.55, y + 0.36, tw);
    y += 0.36 + 0.34 * (reqRows.length + 1) + 0.28;
  }
  if (rspRows) {
    s.addText("响应 data", { x: 4.55, y, w: tw, h: 0.32, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
    paramTable(s, rspRows, 4.55, y + 0.36, tw);
    y += 0.36 + 0.34 * (rspRows.length + 1) + 0.28;
  }
  if (note) s.addText(note, { x: 4.55, y: Math.min(y, 6.75), w: tw, h: 0.62, fontSize: 11.5, color: C.muted, fontFace: F, margin: 0, valign: "top" });
  return s;
}

// ───────────────────────── 封面 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.coverBg };
  s.addText("ZLR5401", { x: M, y: 1.55, w: W - 2 * M, h: 1.5, fontSize: 88, bold: true, color: "FFFFFF", fontFace: F, charSpacing: 4, margin: 0 });
  s.addText("应用层通信协议", { x: M, y: 3.15, w: W - 2 * M, h: 0.9, fontSize: 44, color: "FFFFFF", fontFace: F, margin: 0 });
  s.addText([
    { text: "V1.0", options: { bold: true, color: "FFFFFF" } },
    { text: "（未定稿 · 客户版）", options: { color: "9E9E9E" } },
  ], { x: M, y: 4.35, w: 8, h: 0.6, fontSize: 24, fontFace: F, margin: 0 });
  s.addText("帧格式 · 系统层命令 · UHF 控制 · AM 控制 · 解锁命令 · 灯控映射", {
    x: M, y: 6.35, w: W - 2 * M, h: 0.45, fontSize: 14, color: "9E9E9E", fontFace: F, margin: 0,
  });
}

// ───────────────────────── 概述 + 帧格式 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "概述 · 通信帧格式", "第 1 章");
  const items = [
    "设备经 UART / USB / 以太网与上位机通信，采用统一的请求—响应帧协议。",
    "设备从哪个通道收到请求，就在哪个通道返回响应；设备亦会主动推送解锁过程事件帧。",
    "除特别说明外，多字节整数低字节在前（小端）；每帧以 CRC-32 校验结尾。",
  ];
  s.addText(items.map((t, i) => ({ text: t, options: { bullet: { code: "2013", indent: 12 }, color: C.ink, breakLine: i < items.length - 1 } })),
    { x: M, y: 1.05, w: W - 2 * M, h: 1.5, fontSize: 15, fontFace: F, paraSpaceAfter: 8, margin: 0 });

  // 帧结构图 (按字节宽度比例)
  const fx = M, fy = 3.05, fh = 1.05;
  const fields = [
    { n: "帧头", b: "2 B", t: "0x7753", wd: 1.5 },
    { n: "地址", b: "1 B", t: "0xFF 广播", wd: 1.35 },
    { n: "预留", b: "1 B", t: "0x00", wd: 1.35 },
    { n: "长度", b: "2 B", t: "data 长度", wd: 1.5 },
    { n: "功能码", b: "1 B", t: "见命令表", wd: 1.5 },
    { n: "数据", b: "0~1024 B", t: "随命令而定", wd: 3.1 },
    { n: "CRC32", b: "4 B", t: "校验", wd: 1.5 },
  ];
  let cx = fx;
  fields.forEach((f, i) => {
    s.addShape(pres.shapes.RECTANGLE, {
      x: cx, y: fy, w: f.wd, h: fh,
      fill: { color: i % 2 ? C.rowFill : C.panelFill },
      line: { color: C.border, width: 1 },
    });
    s.addText([
      { text: f.n + "\n", options: { bold: true, fontSize: 13.5, color: C.dark } },
      { text: f.b + "\n", options: { fontSize: 11, color: C.muted } },
      { text: f.t, options: { fontSize: 10.5, color: C.muted } },
    ], { x: cx, y: fy, w: f.wd, h: fh, align: "center", valign: "middle", fontFace: F, margin: 0 });
    cx += f.wd;
  });
  s.addText("请求功能码 = 命令码；响应功能码 = 请求功能码按位取反（FC ^ 0xFF），据此区分请求与响应。",
    { x: M, y: 4.4, w: W - 2 * M, h: 0.4, fontSize: 13.5, color: C.ink, fontFace: F, margin: 0 });

  // 通用响应约定表
  s.addText("通用约定", { x: M, y: 5.0, w: 6, h: 0.32, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  paramTable(s, [
    ["err（data[1]）", "1", "所有子命令响应的第 2 字节；0 = 成功，非 0 见各类错误码表"],
    ["cmd 回显", "1", "所有子命令响应回显请求的子命令码（data[0]）"],
    ["位图 bitmap", "1", "解锁类多标签结果位图；bit n = 第 n 张期望标签已确认"],
  ], M, 5.36, 6.1);
  s.addText("说明：本文件仅公开客户集成所需的系统层命令、UHF 控制、AM 控制与解锁命令；其余功能码为厂商内部使用，预留不开放。",
    { x: M, y: 6.9, w: W - 2 * M, h: 0.4, fontSize: 11.5, color: C.muted, fontFace: F, margin: 0 });
}

// ───────────────────────── 命令总览 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "命令总览", "第 2 章");
  const rows = [
    ["0x01", "系统层", "握手", "确认链路与设备在线，返回协议版本、UID 等信息"],
    ["0x07", "系统层", "设备信息", "读取设备地址与硬件 / 软件版本"],
    ["0x08", "系统层", "软件复位", "设备回 OK 后立即复位重启"],
    ["0x21", "UHF 控制", "超高频读写", "模块上下电、盘点、读写标签、配置、自动扫描"],
    ["0x22", "AM 控制", "消磁器控制", "配置读写、链路探测、状态监控、工作模式"],
    ["0x23", "解锁命令", "开锁业务", "标签清单管理、解锁流程、进度查询与事件推送"],
  ];
  const header = ["功能码", "类别", "名称", "用途"].map(t => ({ text: t, options: { bold: true, fill: { color: C.headFill }, color: C.dark, fontSize: 13 } }));
  const body = rows.map((r, i) => r.map((c, j) => ({
    text: c, options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 13, align: j < 2 ? "center" : "left" },
  })));
  s.addTable([header, ...body], {
    x: M, y: 1.3, w: W - 2 * M, colW: [1.3, 1.7, 2.2, 6.93],
    border: TB, fontFace: F, margin: 0.07, valign: "middle", rowH: 0.52,
  });
  s.addText("0x20~0x26 中未列出的功能码（电机、灯、自检、IO 等）为厂商调试接口，不对客户开放。",
    { x: M, y: 6.95, w: W - 2 * M, h: 0.35, fontSize: 11.5, color: C.muted, fontFace: F, margin: 0 });
}

// ───────────────────────── 章节分隔页 ─────────────────────────
function divider(num, title, sub) {
  const s = pres.addSlide();
  s.background = { color: C.coverBg };
  s.addText(num, { x: M, y: 2.1, w: 3, h: 1.6, fontSize: 96, bold: true, color: "6E6E6E", fontFace: F, margin: 0 });
  s.addText(title, { x: M, y: 3.85, w: W - 2 * M, h: 0.85, fontSize: 40, bold: true, color: "FFFFFF", fontFace: F, margin: 0 });
  s.addText(sub, { x: M, y: 4.85, w: W - 2 * M, h: 0.5, fontSize: 16, color: "9E9E9E", fontFace: F, margin: 0 });
}

// ───────────────────────── 系统层命令 ─────────────────────────
divider("01", "系统层命令", "握手 · 设备信息 · 软件复位");

cmdPage({
  head: "握手", tag: "系统层 · 0x01",
  hex: "0x01",
  desc: "确认通信链路与设备在线。用于上电初始化或断线重连后的设备发现。",
  reqRows: [["（无）", "0", "data 为空"]],
  rspRows: [
    ["result", "1", "0 = 成功"],
    ["ProtoVer", "1", "协议版本号"],
    ["Status", "1", "设备运行状态"],
    ["UID", "12", "芯片唯一 ID"],
    ["UidHash", "4", "UID 哈希（小端）"],
    ["Layer", "1", "1 = 应用层"],
    ["UpgradeCnt", "4", "累计升级次数（小端）"],
    ["BaudRate", "4", "当前波特率（小端）"],
  ],
});

cmdPage({
  head: "设备信息", tag: "系统层 · 0x07",
  hex: "0x07",
  desc: "读取设备通信地址与硬件、软件版本字符串，用于版本核对与资产管理。",
  reqRows: [["（无）", "0", "data 为空"]],
  rspRows: [
    ["result", "1", "0 = 成功"],
    ["Addr", "1", "设备通信地址"],
    ["hwVersion", "16", "硬件版本字符串"],
    ["swVersion", "16", "软件版本字符串"],
  ],
});

cmdPage({
  head: "软件复位", tag: "系统层 · 0x08",
  hex: "0x08",
  desc: "命令设备软复位。设备先回复 OK，随后立即重启；重启后需重新握手。复位期间请勿下发其他命令。",
  reqRows: [["（无）", "0", "data 为空"]],
  rspRows: [["result", "1", "0 = 成功（回帧后复位）"]],
});

// ───────────────────────── UHF 控制 ─────────────────────────
divider("02", "UHF 超高频控制", "功能码 0x21 · 子命令随 data[0] 下发");

const uhfErr = [
  ["0", "成功 OK"], ["1", "参数非法 PARAM"], ["2", "忙 BUSY"], ["3", "未就绪 NOT READY"],
  ["4", "链路故障 LINK"], ["5", "无标签 NO TAG"], ["6", "超时 TIMEOUT"],
];

cmdPage({
  head: "上电启动", tag: "UHF · 0x21 01",
  hex: "0x21\n·01", desc: "UHF 模块上电并下发保存的配置，进入就绪（READY）状态。所有读写操作前必须先启动。",
  reqRows: [["cmd", "1", "0x01"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "停止下电", tag: "UHF · 0x21 02",
  hex: "0x21\n·02", desc: "停止当前操作并为 UHF 模块下电，回到未就绪状态。",
  reqRows: [["cmd", "1", "0x02"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "同步盘点", tag: "UHF · 0x21 03",
  hex: "0x21\n·03",
  desc: "一次同步盘点：设备内部完成盘点并直接在响应帧内返回全部标签（含 RSSI），无主动上报。",
  reqRows: [
    ["cmd", "1", "0x03"],
    ["timeout", "2", "盘点时限 ms（小端）"],
  ],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["count", "2", "标签数（小端）"],
    ["{rssi, epcLen, epc…}", "count ×", "每张标签：RSSI(1) + EPC 长度(1) + EPC"],
  ],
});

cmdPage({
  head: "读标签", tag: "UHF · 0x21 04",
  hex: "0x21\n·04",
  desc: "按指定 EPC 选取标签，读取其存储区数据。",
  reqRows: [
    ["cmd", "1", "0x04"],
    ["epcLen + epc", "1+n", "目标 EPC"],
    ["bank", "1", "存储区：0 EPC / 1 TID / 2 USER / 3 RESD"],
    ["addr", "1", "起始地址（字）"],
    ["cnt", "1", "读取字数"],
  ],
  rspRows: [["cmd / err + 数据", "—", "回显 + 错误码 + 读回数据"]],
});

cmdPage({
  head: "写标签", tag: "UHF · 0x21 05",
  hex: "0x21\n·05",
  desc: "按指定 EPC 选取标签，向其存储区写入数据。",
  reqRows: [
    ["cmd", "1", "0x05"],
    ["epcLen + epc", "1+n", "目标 EPC"],
    ["bank / addr / len", "3", "存储区 / 起始地址 / 写入字数"],
    ["data", "len×", "写入数据"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 写入成功"]],
});

cmdPage({
  head: "停止当前操作", tag: "UHF · 0x21 06",
  hex: "0x21\n·06", desc: "中止进行中的盘点 / 读 / 写操作。不改变上电状态。",
  reqRows: [["cmd", "1", "0x06"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "链路状态查询", tag: "UHF · 0x21 07",
  hex: "0x21\n·07", desc: "查询 UHF 模块链路与工作状态，用于周期性健康检查。",
  reqRows: [["cmd", "1", "0x07"]],
  rspRows: [["cmd / err + 状态", "—", "回显 + 链路 / 就绪状态"]],
});

cmdPage({
  head: "读配置", tag: "UHF · 0x21 08",
  hex: "0x21\n·08", desc: "读取 UHF 模块当前工作配置（发射功率、天线、会话参数、频段等）。",
  reqRows: [["cmd", "1", "0x08"]],
  rspRows: [["cmd / err + 配置", "—", "回显 + 当前配置项（同 0x09 写配置字段）"]],
});

cmdPage({
  head: "写配置", tag: "UHF · 0x21 09",
  hex: "0x21\n·09",
  desc: "设置 UHF 模块工作配置并保存。自动扫描进行中时，新配置在下一轮盘点前生效。",
  reqRows: [
    ["cmd", "1", "0x09"],
    ["powerDbm", "1", "发射功率 dBm"],
    ["antenna", "1", "天线号"],
    ["checksumEn", "1", "CRC 校验使能"],
    ["session", "1", "盘点会话"],
    ["target", "1", "目标标志 A/B"],
    ["q", "1", "Q 值"],
    ["band", "1", "频段（按模块支持）"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "读取标签缓冲", tag: "UHF · 0x21 0A",
  hex: "0x21\n·0A",
  desc: "取走自动扫描缓冲中的标签（含 RSSI）。count = 0 取全部，> 0 取前 count 条。",
  reqRows: [
    ["cmd", "1", "0x0A"],
    ["count", "1", "0 = 全部；> 0 = 前 count 条"],
  ],
  rspRows: [["cmd / err + 标签", "—", "回显 + 错误码 + 标签列表（含 RSSI）"]],
});

cmdPage({
  head: "天线回波检测", tag: "UHF · 0x21 0C",
  hex: "0x21\n·0C", desc: "主动触发一次天线回波检测，验证天线连接是否正常。",
  reqRows: [["cmd", "1", "0x0C"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 检测通过"]],
});

cmdPage({
  head: "启动自动扫描", tag: "UHF · 0x21 0D",
  hex: "0x21\n·0D",
  desc: "连续盘点并写入缓冲，不主动上报；上位机周期性以 0x0A 拉取。识别到标签蜂鸣 50 ms 提示。",
  reqRows: [
    ["cmd", "1", "0x0D"],
    ["cycle", "2", "扫描周期 ms（小端）"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "停止自动扫描", tag: "UHF · 0x21 0E",
  hex: "0x21\n·0E", desc: "停止自动扫描并中止当前盘点。缓冲数据仍可用 0x0A 拉取。",
  reqRows: [["cmd", "1", "0x0E"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

{ // UHF 错误码
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "UHF 错误码（data[1]）", "UHF · 0x21");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 60, bold: true, color: C.giant, fontFace: F, margin: 0 });
  s.addText("所有 0x21 子命令的响应中，data[1] 为错误码；0 表示成功。", { x: M, y: 2.55, w: 3.5, h: 2, fontSize: 13.5, color: C.ink, fontFace: F, margin: 0, lineSpacingMultiple: 1.25 });
  kvTable(s, uhfErr, 4.55, 1.25, 7.5);
}

// ───────────────────────── AM 控制 ─────────────────────────
divider("03", "AM 消磁器控制", "功能码 0x22 · RS232 通道");

cmdPage({
  head: "读配置", tag: "AM · 0x22 01",
  hex: "0x22\n·01", desc: "读取消磁器当前全部工作配置，字段与写配置（0x02）一致。",
  reqRows: [["cmd", "1", "0x01"]],
  rspRows: [["cmd / err + 配置", "—", "回显 + 当前配置（同 0x02 字段）"]],
});

cmdPage({
  head: "写配置", tag: "AM · 0x22 02",
  hex: "0x22\n·02",
  desc: "设置消磁器工作参数。多字节参数为 16 位（低字节在前）。",
  reqRows: [
    ["cmd", "1", "0x02"],
    ["threshold", "2", "触发电平阈值"],
    ["hit", "2", "命中参数"],
    ["freq", "1", "工作频率"],
    ["delay", "2", "消磁延时"],
    ["len", "1", "脉冲长度"],
    ["invert", "1", "极性反转"],
    ["sync", "2", "同步参数"],
    ["volt", "1", "消磁电压档"],
    ["mode", "1", "工作模式（见 0x07）"],
    ["mains", "1", "电网制式"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "读单个参数", tag: "AM · 0x22 03",
  hex: "0x22\n·03",
  desc: "按消磁器命令字读取单个参数（返回本地缓存值）。",
  reqRows: [
    ["cmd", "1", "0x03"],
    ["amCmd", "1", "消磁器命令字"],
  ],
  rspRows: [["cmd / err + 值", "—", "回显 + 错误码 + 参数值（16 位）"]],
});

cmdPage({
  head: "写单个参数", tag: "AM · 0x22 04",
  hex: "0x22\n·04",
  desc: "按消磁器命令字写单个参数，值 16 位（低字节在前）。",
  reqRows: [
    ["cmd", "1", "0x04"],
    ["amCmd", "1", "消磁器命令字"],
    ["val", "2", "参数值（低字节在前）"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "链路探测", tag: "AM · 0x22 05",
  hex: "0x22\n·05", desc: "探测与消磁器的 RS232 链路是否在线，用于开机自检与排障。",
  reqRows: [["cmd", "1", "0x05"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 链路正常"]],
});

cmdPage({
  head: "状态监控", tag: "AM · 0x22 06",
  hex: "0x22\n·06",
  desc: "读取消磁器运行监控数据。消磁结果：0 空闲 / 1 成功 / 2 失败。",
  reqRows: [["cmd", "1", "0x06"]],
  rspRows: [
    ["cmd", "1", "0x06 回显（无 err 字节）"],
    ["link", "1", "0 = 链路正常"],
    ["evt", "4", "事件计数"],
    ["lastEvtMs", "4", "最近事件时刻 ms"],
    ["deact", "1", "0 空闲 / 1 消磁成功 / 2 消磁失败"],
    ["deactCnt / failCnt", "8", "消磁成功 / 失败累计次数（各 4 字节）"],
  ],
});

cmdPage({
  head: "设置工作模式", tag: "AM · 0x22 07",
  hex: "0x22\n·07",
  desc: "仅切换消磁器工作模式并持久化保存。",
  reqRows: [
    ["cmd", "1", "0x07"],
    ["mode", "1", "0 检测+消磁 / 1 仅检测 / 2 待机"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

{ // AM 错误码
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "AM 错误码（data[1]）", "AM · 0x22");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 60, bold: true, color: C.giant, fontFace: F, margin: 0 });
  s.addText("所有 0x22 子命令的响应中，data[1] 为错误码；0 表示成功。", { x: M, y: 2.55, w: 3.5, h: 2, fontSize: 13.5, color: C.ink, fontFace: F, margin: 0, lineSpacingMultiple: 1.25 });
  kvTable(s, [
    ["0", "成功 OK"], ["1", "参数非法 PARAM"], ["2", "忙 BUSY（解锁流程中）"],
    ["4", "链路故障 LINK"], ["6", "超时 TIMEOUT"],
  ], 4.55, 1.25, 7.5);
}

// ───────────────────────── 解锁命令 ─────────────────────────
divider("04", "解锁命令", "功能码 0x23 · 含解锁流程图与灯控映射");

cmdPage({
  head: "配置清单", tag: "解锁 · 0x23 01",
  hex: "0x23\n·01",
  desc: "设置硬标签 / 软标签期望数量并清零清单；清单条目经 0x02 逐条追加。",
  reqRows: [
    ["cmd", "1", "0x01"],
    ["hardCount", "2", "期望硬标签数（低字节在前）"],
    ["softCount", "2", "期望软标签数（低字节在前）"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "追加硬标签 EPC", tag: "解锁 · 0x23 02",
  hex: "0x23\n·02",
  desc: "向期望清单追加一条硬标签 EPC。多标签场景可多次调用累积构建；单帧亦可跨包传输。",
  reqRows: [
    ["cmd", "1", "0x02"],
    ["epcLen", "1", "EPC 字节长度"],
    ["epc", "epcLen", "EPC 内容"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "进入可开锁", tag: "解锁 · 0x23 03",
  hex: "0x23\n·03",
  desc: "UHF 上电并开始扫描，进入可开锁状态（等待解锁命令）。",
  reqRows: [["cmd", "1", "0x03"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "取消", tag: "解锁 · 0x23 04",
  hex: "0x23\n·04",
  desc: "取消当前操作：磁块安全回降，回到空闲。解锁流程进行中仅允许本命令与 0x09 进度查询。",
  reqRows: [["cmd", "1", "0x04"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功（回降后回帧）"]],
});

cmdPage({
  head: "状态查询", tag: "解锁 · 0x23 05",
  hex: "0x23\n·05",
  desc: "查询开锁业务状态机与计数快照。",
  reqRows: [["cmd", "1", "0x05"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["state", "1", "0 空闲 / 1 扫描 / 2 已开锁保持 / 3 软标解码 / 4 完成 / 5 故障 / 6 回降中"],
    ["hm / sc / su", "6", "硬标期望 / 软标期望 / 软标已解（各 2 字节）"],
    ["faultReason", "1", "最近故障原因诊断码，0 = 无"],
  ],
});

cmdPage({
  head: "进度查询", tag: "解锁 · 0x23 09",
  hex: "0x23\n·09",
  desc: "解锁流程中拉取实时进度；无流程时 phase = 0、计数全零。",
  reqRows: [["cmd", "1", "0x09"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["phase", "1", "0 无 / 1 等放标 / 2 校对 / 3 升起 / 4 回降 / 5 软标 / 6 完成"],
    ["holdMs", "3", "解锁窗 W 毫秒（3 字节小端）"],
    ["total / confirmed", "2", "期望标签数 / 已确认数"],
    ["bitmap", "1", "确认位图：bit n = 第 n 张已确认"],
    ["softCnt / softDone", "2", "软标已解数 / 软标完成标志"],
  ],
});

cmdPage({
  head: "解锁（多标签整合）· 参数", tag: "解锁 · 0x23 0A",
  hex: "0x23\n·0A",
  desc: "解锁主命令：单帧下发全部期望 EPC 与软标数，设备阻塞自治完成整个解锁流程（详见流程图页）。",
  reqRows: [
    ["cmd", "1", "0x0A"],
    ["tmo", "2", "光电等待放标超时 ms（低字节在前）"],
    ["hold", "2", "解锁窗基数 ms（低字节在前）"],
    ["softCnt", "1", "期望软标解码次数"],
    ["epcCnt", "1", "期望硬标签张数 m（≤ 4）；0 = 纯软标"],
    ["epcLen", "1", "每张 EPC 字节长度"],
    ["epc…", "m×n", "m 张期望 EPC，每张 epcLen 字节"],
  ],
  note: "解锁窗 W = hold 基数（首标签 2 min）+ 每多一张 30 s，绝对上限 4 min；纯软标（epcCnt=0 且 softCnt>0）跳过放标与校对。epcCnt 与 softCnt 均为 0 → err=2。",
});

cmdPage({
  head: "解锁 · 流程推送帧", tag: "解锁 · 0x23 0A",
  hex: "0x23\n·0A",
  desc: "解锁流程中设备主动推送的阶段事件帧（功能码同 0x23 响应方向，data[0] 为下述子码）；流程结束以 0x0A 终帧回显为标志。",
  rspRows: [
    ["0x0F", "1", "受理帧：流程已启动，含阶段与解锁窗 winMs（3 字节）"],
    ["0x0B", "1", "确认帧：某张期望 EPC 稳定确认（一张一帧 + 蜂鸣 200 ms）"],
    ["0x0C", "1", "失配帧：外来标签稳定确认（一张一帧，不终止流程）"],
    ["0x0D", "1", "硬标完成帧：硬标段结束（全部确认 / 窗满 / 取消 / 失联）"],
    ["0x0E", "1", "软标帧：软标解码计数 +1，含完成标志"],
    ["0x0A 终帧", "1", "流程结束：err=0 时含结束原因、位图与各段耗时"],
  ],
  note: "解锁态内（受理帧起）除 0x04 取消与 0x09 进度查询外，其余子命令及 UHF / AM 控制一律回 BUSY。",
});

{ // 解锁错误码 + 结束原因
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "解锁 · 错误码与结束原因", "解锁 · 0x23");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 60, bold: true, color: C.giant, fontFace: F, margin: 0 });
  s.addText("0x0A 终帧 err 字段（左）与 err=0 时的结束原因 endReason（右）。",
    { x: M, y: 2.55, w: 3.5, h: 2.5, fontSize: 13.5, color: C.ink, fontFace: F, margin: 0, lineSpacingMultiple: 1.25 });
  s.addText("错误码（终帧 / 各子命令）", { x: 4.55, y: 1.25, w: 3.9, h: 0.32, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  kvTable(s, [
    ["0", "成功"], ["1", "忙 BUSY（须先取消）"], ["2", "参数非法"],
    ["3 / 4", "UHF 启动 / 链路失败"], ["7", "未回零 / 行程开关错误"],
    ["8 / 9", "电机故障 / 电机超时"], ["10", "AM 链路故障"], ["11", "放标光电超时"],
  ], 4.55, 1.62, 3.9);
  s.addText("结束原因 endReason（err=0）", { x: 8.85, y: 1.25, w: 3.9, h: 0.32, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  kvTable(s, [
    ["1", "ALL_OK：全部标签确认完成"], ["2", "硬标窗满，部分确认（位图明示）"],
    ["4", "UHF 链路失联"], ["6", "被 0x04 取消（已安全回降）"],
    ["7", "软标窗 5 min 满未校验完成"],
  ], 8.85, 1.62, 3.9);
}

// ───────────────────────── 解锁流程图 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "解锁流程图（0x0A 下发至完成）", "解锁 · 0x23 0A");

  const bx = 4.7, bw = 4.1, bh = 0.52, gap = 0.235, y0 = 1.12;
  const steps = [
    "下发解锁命令 0x0A",
    "受理 · 推送 0x0F（解锁窗 W）",
    "等待放标（光电门控）",
    "持续盘点校对（在场 1.5 s + 命中 ≥2）",
    "全部确认 n == m",
    "磁块升起 + 消磁开启",
    "软标解码（5 min 窗）",
    "磁块回降 · 终帧 0x0A 结账",
  ];
  steps.forEach((t, i) => {
    const y = y0 + i * (bh + gap);
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
      x: bx, y, w: bw, h: bh, rectRadius: 0.06,
      fill: { color: i === steps.length - 1 ? C.headFill : C.panelFill },
      line: { color: i === steps.length - 1 ? C.dark : C.border, width: i === steps.length - 1 ? 1.5 : 1 },
    });
    s.addText(t, { x: bx, y, w: bw, h: bh, align: "center", valign: "middle", fontSize: 13, bold: i === 0 || i === 4 || i === steps.length - 1, color: C.ink, fontFace: F, margin: 0 });
    if (i < steps.length - 1) s.addShape(pres.shapes.LINE, {
      x: bx + bw / 2, y: y + bh, w: 0, h: gap,
      line: { color: C.faint, width: 1.75, endArrowType: "triangle" },
    });
  });

  // 左侧: 退出路径
  const exits = [
    ["步骤 3 超时", "err = 11 放标超时"],
    ["步骤 4 窗满", "endReason = 2 部分确认"],
    ["UHF 失联 5 s", "endReason = 4 磁块不动"],
    ["任意阶段 0x04", "安全回降 · endReason = 6"],
  ];
  s.addText("异常出口", { x: M, y: 1.12, w: 3.4, h: 0.35, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  exits.forEach((e, i) => {
    const y = 1.6 + i * 0.92;
    s.addShape(pres.shapes.RECTANGLE, { x: M, y, w: 3.4, h: 0.78, fill: { color: "FFFFFF" }, line: { color: C.border, width: 1 } });
    s.addText([
      { text: e[0] + "\n", options: { bold: true, fontSize: 12, color: C.dark } },
      { text: e[1], options: { fontSize: 11, color: C.muted } },
    ], { x: M + 0.12, y, w: 3.2, h: 0.78, valign: "middle", fontFace: F, margin: 0 });
  });

  // 右侧: 推送帧与灯语
  const right = [
    ["0x0B", "每张期望标签确认（蜂鸣 + 绿单闪）"],
    ["0x0C", "外来标签失配（红快闪 3 s，不终止）"],
    ["0x0D", "硬标段结束"],
    ["0x0E", "软标解码计数 +1"],
  ];
  s.addText("流程推送事件", { x: 9.25, y: 1.12, w: 3.5, h: 0.35, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  right.forEach((e, i) => {
    const y = 1.6 + i * 0.74;
    s.addShape(pres.shapes.RECTANGLE, { x: 9.25, y, w: 3.5, h: 0.6, fill: { color: C.rowFill }, line: { color: C.border, width: 0.75 } });
    s.addText([
      { text: e[0] + "  ", options: { bold: true, fontSize: 12, color: C.dark } },
      { text: e[1], options: { fontSize: 11, color: C.ink } },
    ], { x: 9.37, y, w: 3.3, h: 0.6, valign: "middle", fontFace: F, margin: 0 });
  });
  s.addText("灯语同步见下页映射表；步骤 6 升起后保持绿常亮直至回降。",
    { x: 9.25, y: 4.72, w: 3.5, h: 1.4, fontSize: 11, color: C.muted, fontFace: F, margin: 0, lineSpacingMultiple: 1.3 });
  s.addText("纯软标模式（epcCnt=0）：跳过步骤 3~5，受理即计时，直接进入升起 + 消磁 + 软解码。",
    { x: M, y: 6.9, w: 12.1, h: 0.4, fontSize: 11.5, color: C.muted, fontFace: F, margin: 0 });
}

// ───────────────────────── 灯控映射表 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.bg };
  pageHead(s, "灯控映射表（RGB 灯语）", "解锁 · 灯语");
  const rows = [
    ["FFFFFF", "白 · 慢闪", "等待放置标签"],
    ["4D7CC7", "蓝 · 慢闪", "盘点 / 校对进行中"],
    ["3FA34D", "绿 · 单闪", "一张期望标签确认成功"],
    ["E04545", "红 · 快闪 3 s", "外来（非期望）标签"],
    ["3FA34D", "绿 · 常亮", "已开锁（磁块升起保持）"],
    ["FFFFFF", "白 · 常亮", "软标解码等待 / 进行中"],
    ["3FA34D", "绿 · 三连闪", "解锁流程完成"],
    ["E04545", "红 · 双闪", "操作失败"],
    ["E04545", "红 · 常亮", "设备故障"],
    ["E8C33C", "黄 · 慢闪", "回零 / 自检 · 未放标提醒"],
  ];
  const y0 = 1.25, rh = 0.5, cw = 5.9;
  s.addText("颜色 / 节奏", { x: 9.0, y: y0 - 0.38, w: cw, h: 0.3, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  s.addText("含义", { x: 9.0 + 1.35, y: y0 - 0.38, w: 3, h: 0.3, fontSize: 13, bold: true, color: C.dark, fontFace: F, margin: 0 });
  rows.forEach((r, i) => {
    const y = y0 + i * (rh + 0.06);
    s.addShape(pres.shapes.OVAL, { x: 9.0, y: y + 0.08, w: 0.34, h: 0.34, fill: { color: r[0] }, line: { color: C.border, width: 0.75 } });
    s.addText(r[1], { x: 9.45, y, w: 2.0, h: rh, fontSize: 13, color: C.ink, fontFace: F, valign: "middle", margin: 0 });
    s.addText(r[2], { x: 11.5, y, w: 2.6, h: rh, fontSize: 13, color: C.ink, fontFace: F, valign: "middle", margin: 0 });
    s.addShape(pres.shapes.LINE, { x: 9.0, y: y + rh + 0.03, w: 5.0, h: 0, line: { color: "E4E4E4", width: 0.75 } });
  });

  // 左侧说明卡
  s.addShape(pres.shapes.RECTANGLE, { x: M, y: 1.25, w: 7.6, h: 5.3, fill: { color: C.panelFill }, line: { color: C.border, width: 1 } });
  s.addText("灯语说明", { x: M + 0.35, y: 1.55, w: 6.9, h: 0.4, fontSize: 16, bold: true, color: C.dark, fontFace: F, margin: 0 });
  const notes = [
    "灯语与解锁流程阶段一一对应，操作人员凭灯光颜色与节奏即可判断当前进度。",
    "确认（绿单闪）与失配（红快闪）为瞬时提示，闪毕回落当前阶段的稳态灯语。",
    "上位机可以 0x24 手动设置灯色（设备空闲时），10 s 后自动回收，业务灯语优先。",
    "任一时刻红灯常亮表示设备故障，请结合 0x23 05 状态查询定位原因。",
  ];
  s.addText(notes.map((t, i) => ({ text: t, options: { bullet: { code: "2013", indent: 12 }, breakLine: i < notes.length - 1 } })),
    { x: M + 0.35, y: 2.15, w: 6.9, h: 4.1, fontSize: 13.5, color: C.ink, fontFace: F, paraSpaceAfter: 14, margin: 0, valign: "top", lineSpacingMultiple: 1.3 });
}

// ───────────────────────── 结尾 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.coverBg };
  s.addText("V1.0（未定稿）", { x: M, y: 2.6, w: W - 2 * M, h: 0.8, fontSize: 40, bold: true, color: "FFFFFF", fontFace: F, margin: 0 });
  s.addText("本协议尚未定版，帧格式与命令参数以最终发布版为准；如有变更将随版本号同步更新。",
    { x: M, y: 3.6, w: W - 2 * M, h: 0.5, fontSize: 15, color: "9E9E9E", fontFace: F, margin: 0 });
}

pres.writeFile({ fileName: "/Users/swnw/Documents/Software/ZLR5401/Protocol/App_Protocol_V1.0.pptx" })
  .then(() => console.log("OK"));
