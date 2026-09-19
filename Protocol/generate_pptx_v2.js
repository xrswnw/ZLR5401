// ZLR5401 应用层通信协议 — 客户版 PPT (V1.0 定版 · 2026-08-24)
// 风格: 晕染水彩 (白纸底 · 青绿晕染 · 金色点缀 · 宋体标题) — 参照水彩风模板 (17)
const pptxgen = require("pptxgenjs");

const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE";
pres.author = "ZLR5401";
pres.title = "ZLR5401 应用层通信协议 V1.0";

const A = "assets"; // 水彩素材 (相对本脚本目录)

// ───── 水彩风配色 ─────
const C = {
  paper: "F9F8F4",      // 纸面底色
  ink: "3A3A38",       // 正文
  green: "3F5A52",     // 标题墨绿
  giant: "3F5A52",     // 焦点十六进制
  gold: "B8965A",      // 金色点缀
  muted: "8A9188",     // 灰绿辅助
  headFill: "EDF2EE",  // 表头淡青
  rowFill: "F5F7F4",   // 隔行
  border: "C9C2AD",    // 暖金灰边框
  goldLine: "C8B27E",  // 金色细线
};
const FT = "宋体";        // 标题衬线
const FB = "微软雅黑";     // 正文
const W = 13.33, H = 7.5, M = 0.6;

const TB = { pt: 0.75, color: C.border };

// 内容页通用装饰: 右上角水彩 (低透明度)
function ornament(s) {
  s.addImage({ path: `${A}/wash_right.png`, x: W - 2.35, y: 0, w: 2.35, h: 1.35, transparency: 42, flipH: true });
}

function paramTable(slide, rows, x, y, w) {
  const header = ["字段", "字节", "说明"].map(t => ({ text: t, options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5 } }));
  const body = rows.map((r, i) => r.map((c, j) => ({
    text: String(c),
    options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12, align: j === 1 ? "center" : "left" },
  })));
  slide.addTable([header, ...body], {
    x, y, w, colW: [1.55, 0.85, w - 2.4],
    border: TB, fontFace: FB, margin: 0.06, valign: "middle", rowH: 0.34,
  });
}

function kvTable(slide, rows, x, y, w) {
  const header = [
    { text: "值", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5, align: "center" } },
    { text: "含义", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5 } },
  ];
  const body = rows.map((r, i) => [
    { text: String(r[0]), options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12, align: "center" } },
    { text: String(r[1]), options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12 } },
  ]);
  slide.addTable([header, ...body], {
    x, y, w, colW: [0.9, w - 0.9], border: TB, fontFace: FB, margin: 0.06, valign: "middle", rowH: 0.32,
  });
}

function pageHead(slide, title, tag) {
  slide.addText(title, { x: M, y: 0.34, w: 9.6, h: 0.6, fontSize: 26, bold: true, color: C.green, fontFace: FT, margin: 0 });
  if (tag) slide.addText(tag, { x: W - M - 3.4, y: 7.02, w: 3.4, h: 0.35, fontSize: 11, color: C.muted, fontFace: FB, align: "right", margin: 0 });
}

// 测试帧十六进制着色: 帧头/长度/CRC 灰绿 · 主命令墨绿 · 子命令金色 · 数据墨色
function hexRich(bytes) {
    const hasSub = ![0x01, 0x07, 0x08, 0xFE, 0xF8, 0xF7].includes(bytes[6]);
  const n = bytes.length;
  return bytes.map((b, i) => {
    let color = C.ink, bold = false;
    if (i < 6 || i >= n - 4) color = C.muted;
    else if (i === 6) { color = C.green; bold = true; }
    else if (hasSub && i === 7) { color = C.gold; bold = true; }
    return { text: b.toString(16).padStart(2, "0").toUpperCase() + (i < n - 1 ? " " : ""), options: { color, bold } };
  });
}

// 测试帧示例框 (左栏 desc 下方): 金边白底 · 请求帧 + 响应帧(实测) + 各一句解释
function testFrameBox(s, frames, y0) {
  const bw = 3.5, ix = M + 0.12, iw = bw - 0.24;
  let y = y0;
  frames.forEach(f => {
    const bytes = f.hex.split(" ").map(h => parseInt(h, 16));
    const hexLines = Math.ceil((bytes.length * 3 - 1) / 44);
    const expLines = f.exp ? Math.ceil(f.exp.length / 26) : 0;
    let rsp = null, bh = 0.04 + 0.24 + hexLines * 0.17 + 0.02 + (expLines ? expLines * 0.19 + 0.02 : 0);
    if (f.rsp) {
      const rb = f.rsp.hex.split(" ").map(h => parseInt(h, 16));
      const rl = Math.ceil((rb.length * 3 - 1) / 44);
      const rel = Math.ceil(f.rsp.exp.length / 26);
      bh += 0.21 + rl * 0.17 + 0.02 + rel * 0.19;
      rsp = { rb, rl, rel };
    }
    bh += 0.05;
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
      x: M, y, w: bw, h: bh, rectRadius: 0.05,
      fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1 },
    });
    let cy = y + 0.04;
    s.addText(f.tag, { x: ix, y: cy, w: iw, h: 0.22, fontSize: 10.5, bold: true, color: C.green, fontFace: FB, margin: 0 });
    cy += 0.24;
    s.addText(hexRich(bytes), { x: ix, y: cy, w: iw, h: hexLines * 0.17 + 0.04, fontSize: 8.5, fontFace: "Courier New", margin: 0, valign: "top", lineSpacingMultiple: 1.15 });
    cy += hexLines * 0.17 + 0.02;
    if (f.exp) {
      s.addText(f.exp, { x: ix, y: cy, w: iw, h: expLines * 0.19 + 0.04, fontSize: 9.5, color: C.muted, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.15 });
      cy += expLines * 0.19 + 0.02;
    }
    if (rsp) {
      s.addText("响应帧（实测）", { x: ix, y: cy, w: iw, h: 0.19, fontSize: 9, bold: true, color: C.gold, fontFace: FB, margin: 0 });
      cy += 0.21;
      s.addText(hexRich(rsp.rb), { x: ix, y: cy, w: iw, h: rsp.rl * 0.17 + 0.04, fontSize: 8.5, fontFace: "Courier New", margin: 0, valign: "top", lineSpacingMultiple: 1.15 });
      cy += rsp.rl * 0.17 + 0.02;
      s.addText(f.rsp.exp, { x: ix, y: cy, w: iw, h: rsp.rel * 0.19 + 0.04, fontSize: 9.5, color: C.muted, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.15 });
    }
    y += bh + 0.1;
  });
}

function cmdPage({ head, tag, hex, desc, reqRows, rspRows, note, test, testY }) {
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, head, tag);
  // 主命令墨绿 / 子命令金色 双色区分
  const hexParts = hex.split("\n");
  s.addText(
    hexParts.map((t, i) => ({ text: t, options: { color: i === 0 ? C.giant : C.gold, breakLine: i < hexParts.length - 1 } })),
    { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 54, bold: true, fontFace: FT, margin: 0, valign: "top" }
  );
  s.addText(desc || "", { x: M, y: 2.55, w: 3.5, h: 3.3, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.25 });
  let y = 1.25;
  const tw = W - M - 4.55;
  if (reqRows) {
    s.addText("请求 data", { x: 4.55, y, w: tw, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
    paramTable(s, reqRows, 4.55, y + 0.36, tw);
    y += 0.36 + 0.34 * (reqRows.length + 1) + 0.28;
  }
  if (rspRows) {
    s.addText("响应 data", { x: 4.55, y, w: tw, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
    paramTable(s, rspRows, 4.55, y + 0.36, tw);
    y += 0.36 + 0.34 * (rspRows.length + 1) + 0.28;
  }
  if (note) s.addText(note, { x: 4.55, y: Math.min(y, 6.7), w: tw, h: 0.68, fontSize: 11.5, color: C.muted, fontFace: FB, margin: 0, valign: "top" });
  if (test && test.length) testFrameBox(s, test, testY !== undefined ? testY : 4.48);
  return s;
}

// ───────────────────────── 封面 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  s.addImage({ path: `${A}/wash_left.png`, x: -0.7, y: -0.75, w: 5.4, h: 4.05, transparency: 8 });
  s.addImage({ path: `${A}/wash_left.png`, x: W - 4.7, y: H - 3.35, w: 5.4, h: 4.05, transparency: 8, flipH: true, flipV: true });
  // 中央金边六角白框
  s.addShape(pres.shapes.HEXAGON, {
    x: 3.87, y: 1.35, w: 5.6, h: 4.8, fill: { color: "FDFCFA", transparency: 8 },
    line: { color: C.goldLine, width: 1.5 },
  });
  s.addText("ZLR5401", { x: 4.3, y: 2.0, w: 4.75, h: 0.85, fontSize: 44, bold: true, color: C.green, fontFace: FT, align: "center", margin: 0, charSpacing: 3 });
  s.addText("应用层通信协议", { x: 4.3, y: 2.9, w: 4.75, h: 0.75, fontSize: 32, color: C.ink, fontFace: FT, align: "center", margin: 0 });
  s.addText([
    { text: "V1.0", options: { bold: true, color: C.gold } },
    { text: "（定版 · 2026-08-24）", options: { color: C.muted } },
  ], { x: 4.3, y: 4.0, w: 4.75, h: 0.5, fontSize: 18, fontFace: FT, align: "center", margin: 0 });
  s.addText("帧格式 · 系统层命令 · UHF 控制 · AM 控制 · 解锁命令 · 灯控映射", {
    x: 2.67, y: 6.75, w: 8, h: 0.4, fontSize: 13, color: C.muted, fontFace: FB, align: "center", margin: 0,
  });
}

// ───────────────────────── 概述 + 帧格式 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "概述 · 通信帧格式", "第 1 章");
  const items = [
    "设备经 UART / USB / 以太网与上位机通信，采用统一的请求—响应帧协议。",
    "设备从哪个通道收到请求，就在哪个通道返回响应；设备亦会主动推送解锁过程事件帧。",
    "除特别说明外，多字节整数低字节在前（小端）；每帧以 CRC-32 校验结尾。",
  ];
  s.addText(items.map((t, i) => ({ text: t, options: { bullet: { code: "2013", indent: 12 }, color: C.ink, breakLine: i < items.length - 1 } })),
    { x: M, y: 1.05, w: W - 2 * M, h: 1.5, fontSize: 15, fontFace: FB, paraSpaceAfter: 8, margin: 0 });

  const fx = M, fy = 3.05, fh = 1.05;
  const fields = [
    { n: "帧头", b: "2 B", t: "0x7753", wd: 1.5 },
    { n: "地址", b: "1 B", t: "0xFF 广播", wd: 1.35 },
    { n: "预留", b: "1 B", t: "0x00", wd: 1.35 },
    { n: "长度", b: "2 B", t: "载荷长度", wd: 1.5 },
    { n: "功能码", b: "1 B", t: "见命令表", wd: 1.5 },
    { n: "数据", b: "0~1024 B", t: "随命令而定", wd: 3.1 },
    { n: "CRC32", b: "4 B", t: "校验", wd: 1.5 },
  ];
  let cx = fx;
  fields.forEach((f, i) => {
    s.addShape(pres.shapes.RECTANGLE, {
      x: cx, y: fy, w: f.wd, h: fh,
      fill: { color: i % 2 ? C.rowFill : "FFFFFF" },
      line: { color: i === 0 || i === fields.length - 1 ? C.goldLine : C.border, width: 1 },
    });
    s.addText([
      { text: f.n + "\n", options: { bold: true, fontSize: 13.5, color: C.green } },
      { text: f.b + "\n", options: { fontSize: 11, color: C.muted } },
      { text: f.t, options: { fontSize: 10.5, color: C.muted } },
    ], { x: cx, y: fy, w: f.wd, h: fh, align: "center", valign: "middle", fontFace: FB, margin: 0 });
    cx += f.wd;
  });
  s.addText("长度 = 功能码(1) + 数据 + CRC(4) 的总字节数（小端）。请求功能码 = 命令码；响应功能码 = 请求功能码按位取反（FC ^ 0xFF），据此区分请求与响应。",
    { x: M, y: 4.4, w: W - 2 * M, h: 0.4, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0 });

  s.addText("通用约定", { x: M, y: 5.0, w: 6, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  paramTable(s, [
    ["err（data[1]）", "1", "所有子命令响应的第 2 字节；0 = 成功，非 0 见各类错误码表"],
    ["cmd 回显", "1", "所有子命令响应回显请求的子命令码（data[0]）"],
    ["位图 bitmap", "1", "解锁类多标签结果位图；bit n = 第 n 张期望标签已确认"],
  ], M, 5.36, 6.1);
  // 版本说明日志
  s.addText("版本说明", { x: 7.0, y: 5.0, w: 4, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  const vh = [
    { text: "版本", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5, align: "center" } },
    { text: "日期", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5, align: "center" } },
    { text: "说明", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5 } },
  ];
  s.addTable([
    vh,
    ["V1.0", "2026-08-24", "初版定版"].map((c, j) => ({
      text: c, options: { fill: { color: C.rowFill }, color: C.ink, fontSize: 12, align: j < 2 ? "center" : "left" },
    })),
  ], { x: 7.0, y: 5.36, w: W - M - 7.0, colW: [0.9, 1.5, 3.73], border: TB, fontFace: FB, margin: 0.06, valign: "middle", rowH: 0.34 });
}

// ───────────────────────── 命令总览 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "命令总览", "第 2 章");
  const rows = [
    ["0x01", "系统层", "握手", "确认链路与设备在线，返回协议版本、UID 等信息"],
    ["0x07", "系统层", "设备信息", "读取设备地址与硬件 / 软件版本"],
    ["0x08", "系统层", "软件复位", "设备回 OK 后立即复位重启"],
    ["0x21", "UHF 控制", "超高频读写", "盘点、读写标签、配置、链路状态、自动扫描"],
    ["0x22", "AM 控制", "消磁器控制", "配置读写、链路探测、状态监控、工作模式"],
    ["0x23", "解锁命令", "开锁业务", "解锁流程、取消、状态与进度查询、事件推送"],
  ];
  const header = ["功能码", "类别", "名称", "用途"].map(t => ({ text: t, options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 13 } }));
  const body = rows.map((r, i) => r.map((c, j) => ({
    text: c, options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 13, align: j < 2 ? "center" : "left" },
  })));
  s.addTable([header, ...body], {
    x: M, y: 1.3, w: W - 2 * M, colW: [1.3, 1.7, 2.2, 6.93],
    border: TB, fontFace: FB, margin: 0.07, valign: "middle", rowH: 0.52,
  });
  s.addText("本文件仅公开客户集成所需的系统层、UHF、AM 与解锁命令；其余功能码（电机、灯、自检、IO 等）为厂商内部接口，不对客户开放。",
    { x: M, y: 6.95, w: W - 2 * M, h: 0.35, fontSize: 11.5, color: C.muted, fontFace: FB, margin: 0 });
}

// ───────────────────────── 章节分隔页 ─────────────────────────
function divider(num, title, sub) {
  const s = pres.addSlide();
  s.background = { color: C.paper };
  s.addImage({ path: `${A}/wash_left.png`, x: W - 4.15, y: 0.98, w: 3.55, h: 5.55, transparency: 10 });
  s.addText(num, { x: M, y: 2.0, w: 3, h: 1.55, fontSize: 88, bold: true, color: C.gold, fontFace: FT, margin: 0 });
  s.addText(title, { x: M, y: 3.7, w: 7.6, h: 0.85, fontSize: 38, bold: true, color: C.green, fontFace: FT, margin: 0 });
  s.addText(sub, { x: M, y: 4.7, w: 7.6, h: 0.5, fontSize: 15, color: C.muted, fontFace: FB, margin: 0 });
  s.addShape(pres.shapes.LINE, { x: M, y: 5.5, w: 2.4, h: 0, line: { color: C.goldLine, width: 1.5 } });
}

// ───────────────────────── 系统层命令 ─────────────────────────
divider("壹", "系统层命令", "握手 · 设备信息 · 软件复位");

cmdPage({
  head: "握手", tag: "系统层 · 0x01",
  hex: "0x01",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 05 00 01 C5 69 2F 26",
    exp: "data 为空；设备回 0xFE，含协议版本与 UID，用于确认在线。",
    rsp: { hex: "53 77 01 00 21 00 FE 00 03 01 1C D9 37 30 38 33 0B 00 39 35 37 39 9D 43 A2 4D 01 00 00 00 00 00 C2 01 00 25 56 0B 38",
      exp: "result=0 · ProtoVer=3 · Layer=1 · Baud=115200（实测）" } }],
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
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 05 00 07 77 24 A9 3C",
    exp: "data 为空；回帧含设备地址与硬件 / 软件版本字符串。",
    rsp: { hex: "53 77 01 00 27 00 F8 00 01 43 38 54 36 5F 56 31 2E 30 00 00 00 00 00 00 00 5A 4C 52 35 34 30 31 5F 56 31 2E 30 00 00 00 00 72 DB 9E 3C",
      exp: "result=0 · Addr=1 · 硬件 C8T6_V1.0（实测）" } }],
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
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 05 00 08 CA 99 E6 04",
    exp: "设备回 result=0 后立即复位重启；重启后需重新握手。",
    rsp: { hex: "53 77 01 00 06 00 F7 00 7B 2A 55 DB",
      exp: "result=0，回帧后设备复位重启（实测）" } }],
  desc: "命令设备软复位。设备先回复 OK，随后立即重启；重启后需重新握手。复位期间请勿下发其他命令。",
  reqRows: [["（无）", "0", "data 为空"]],
  rspRows: [["result", "1", "0 = 成功（回帧后复位）"]],
});

// ───────────────────────── UHF 控制 ─────────────────────────
divider("贰", "UHF 超高频控制", "功能码 0x21 · 子命令随 data[0] 下发");

cmdPage({
  head: "同步盘点", tag: "UHF · 0x21 03",
  hex: "0x21\n·03",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 08 00 21 03 E8 03 3D 41 A4 CB",
    exp: "盘点时限 1000 ms（E8 03 小端）；响应帧内直接返回全部标签。",
    rsp: { hex: "53 77 01 00 25 00 DE 03 00 02 00 FF 0C 33 55 34 63 A4 00 01 58 EB 7E 75 07 FF 0C 4F 19 54 79 21 3F 00 50 00 00 00 05 50 F5 C6 A1",
      exp: "err=0 · 2 张在位标签，rssi 在前（实测）" } }],
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
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 16 00 21 04 0C E2 80 68 94 00 00 12 34 56 78 90 1A 03 00 04 CE 81 A5 6A",
    exp: "读 USER 区（bank=3）地址 0 起 4 字；回帧仅表受理，数据经 0x0A 拉取。",
    rsp: { hex: "53 77 01 00 07 00 DE 04 00 F3 E5 DB 57",
      exp: "err=0 = 已受理；读回数据经 0x0A 拉取（实测）" } }],
  desc: "读取标签存储区数据。异步执行：响应只表示已受理，读回数据进入标签缓冲，经 0x0A 拉取。",
  reqRows: [
    ["cmd", "1", "0x04"],
    ["epcLen + epc", "1+n", "目标 EPC（当前版本读首张标签，EPC 为预留）"],
    ["bank", "1", "存储区：1 EPC / 2 TID / 3 USER / 0 保留"],
    ["addr", "1", "起始地址（字）"],
    ["cnt", "1", "读取字数"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 已受理"]],
  note: "读回数据（3~16 字节）完成后进入缓冲，以 0x0A 拉取：该项 rssi=0、epcLen=数据长度、epc=数据内容。",
});

cmdPage({
  head: "写标签", tag: "UHF · 0x21 05",
  hex: "0x21\n·05",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 1A 00 21 05 0C E2 80 68 94 00 00 12 34 56 78 90 1A 03 00 04 12 34 56 78 C6 A5 D4 A1",
    exp: "向首张标签 USER 区地址 0 写 4 字节 12 34 56 78（受理即回帧）。",
    rsp: { hex: "53 77 01 00 07 00 DE 05 00 2F 24 C2 85",
      exp: "err=0 = 命令已受理下发（实测）" } }],
  desc: "向标签存储区写入数据。异步下发：响应只表示命令已受理，写结果建议读回核验。",
  reqRows: [
    ["cmd", "1", "0x05"],
    ["epcLen + epc", "1+n", "目标 EPC（当前版本写首张标签，EPC 为预留）"],
    ["bank / addr / len", "3", "存储区 / 起始地址（字）/ 写入字节数"],
    ["data", "len×", "写入数据"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 已受理"]],
  note: "bank 映射同 0x04：1 EPC / 2 TID / 3 USER / 0 保留。",
});

cmdPage({
  head: "停止当前操作", tag: "UHF · 0x21 06",
  hex: "0x21\n·06",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 21 06 A8 F7 9E F7",
    exp: "中止进行中的盘点 / 读 / 写操作。",
    rsp: { hex: "53 77 01 00 07 00 DE 06 00 FC 7B 29 F7",
      exp: "err=0 已停止（实测）" } }],
  desc: "中止进行中的盘点 / 读 / 写操作。",
  reqRows: [["cmd", "1", "0x06"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "链路状态查询", tag: "UHF · 0x21 07",
  hex: "0x21\n·07",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 21 07 1F EA 5F F3",
    exp: "查询链路；回帧含链路通断与就绪状态。",
    rsp: { hex: "53 77 01 00 0A 00 DE 07 00 01 00 02 64 80 D8 52",
      exp: "err=0 · state=1 · link=0 · total=2（实测）" } }],
  desc: "查询 UHF 模块链路与工作状态，用于周期性健康检查。",
  reqRows: [["cmd", "1", "0x07"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码（未上电回 3 NOT READY）"],
    ["state", "1", "模块状态"],
    ["link", "1", "0 = 链路正常"],
    ["totalTags", "1", "累计读到标签数（≤255）"],
  ],
});

cmdPage({
  head: "读配置", tag: "UHF · 0x21 08",
  hex: "0x21\n·08",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 21 08 A2 57 10 CB",
    exp: "读取当前配置；回帧字段与 0x09 写配置一一对应。",
    rsp: { hex: "53 77 01 00 0E 00 DE 08 00 14 00 01 00 00 00 01 8B 47 75 27",
      exp: "err=0 · 返回当前 7 字节配置（实测）" } }],
  desc: "读取 UHF 模块当前工作配置（发射功率、天线、会话参数、频段等）。",
  reqRows: [["cmd", "1", "0x08"]],
  rspRows: [["cmd / err + 配置", "—", "回显 + 当前配置项（同 0x09 写配置字段）"]],
});

cmdPage({
  head: "写配置", tag: "UHF · 0x21 09",
  hex: "0x21\n·09",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 0D 00 21 09 1A 01 01 01 00 04 01 73 DD C9 61",
    exp: "功率 26 dBm · 天线 1 · 校验开 · Session 1 · 目标 A · Q=4 · 频段 1。",
    rsp: { hex: "53 77 01 00 07 00 DE 09 00 D4 46 AE 4A",
      exp: "err=0，配置已接受并保存（实测）" } }],
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
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 07 00 21 0A 00 7A 00 69 8F",
    exp: "count=0：取走自动扫描缓冲内全部标签（含 RSSI）。",
    rsp: { hex: "53 77 01 00 25 00 DE 0A 00 02 00 0C FF 4F 19 54 79 21 3F 00 50 00 00 00 05 0C FF 33 55 34 63 A4 00 01 58 EB 7E 75 07 93 91 DD 01",
      exp: "err=0 · 2 张缓冲标签，epcLen 在前（实测）" } }],
  desc: "取走自动扫描缓冲中的标签（含 RSSI）。count = 0 取全部，> 0 取前 count 条。",
  reqRows: [
    ["cmd", "1", "0x0A"],
    ["count", "1", "0 = 全部；> 0 = 前 count 条"],
  ],
  rspRows: [
    ["cmd / err + count", "4", "回显 + 错误码 + 标签数（小端）"],
    ["{epcLen, rssi, epc…}", "count ×", "每张：EPC 长度(1) + RSSI(1) + EPC（顺序与 0x03 相反）"],
  ],
});

cmdPage({
  head: "天线回波检测", tag: "UHF · 0x21 0C",
  hex: "0x21\n·0C",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 21 0C 7E 21 14 D8",
    exp: "触发一次天线回波检测；err=0 表示天线连接正常。",
    rsp: { hex: "53 77 01 00 0C 00 DE 0C 00 01 00 5A 00 D2 50 16 8F 97",
      exp: "err=0 · antOk=1 · antRl=90 · VSWR=210（实测）" } }],
  desc: "主动触发一次天线回波检测，验证天线连接是否正常。",
  reqRows: [["cmd", "1", "0x0C"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 0 = 检测通过"],
    ["antennaOk", "1", "1 = 天线正常"],
    ["antRl / antVswr", "2+2", "回波损耗 / 驻波比（高字节在前）"],
  ],
});

cmdPage({
  head: "启动自动扫描", tag: "UHF · 0x21 0D",
  hex: "0x21\n·0D",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 08 00 21 0D E8 03 17 CE B2 C1",
    exp: "以 1000 ms 周期连续盘点写入缓冲，配合 0x0A 拉取。",
    rsp: { hex: "53 77 01 00 07 00 DE 0D 00 7D 67 8A 0F",
      exp: "err=0 扫描已启动（实测）" } }],
  desc: "连续盘点并写入缓冲，不主动上报；上位机周期性以 0x0A 拉取。识别到标签蜂鸣 50 ms 提示。",
  reqRows: [
    ["cmd", "1", "0x0D"],
    ["cycle", "2", "扫描周期 ms（小端）"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

cmdPage({
  head: "停止自动扫描", tag: "UHF · 0x21 0E",
  hex: "0x21\n·0E",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 21 0E 10 1A 96 D1",
    exp: "停止自动扫描；缓冲数据仍可用 0x0A 拉取。",
    rsp: { hex: "53 77 01 00 07 00 DE 0E 00 AE 38 61 7D",
      exp: "err=0 扫描已停止（实测）" } }],
  desc: "停止自动扫描并中止当前盘点。缓冲数据仍可用 0x0A 拉取。",
  reqRows: [["cmd", "1", "0x0E"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

{ // UHF 错误码
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "UHF 错误码（data[1]）", "UHF · 0x21");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 54, bold: true, color: C.giant, fontFace: FT, margin: 0 });
  s.addText("所有 0x21 子命令的响应中，data[1] 为错误码；0 表示成功。", { x: M, y: 2.55, w: 3.5, h: 2, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0, lineSpacingMultiple: 1.25 });
  kvTable(s, [
    ["0", "成功 OK"], ["1", "参数非法 PARAM"], ["2", "忙 BUSY"], ["3", "未就绪 NOT READY"],
    ["4", "链路故障 LINK"], ["5", "无标签 NO TAG"], ["6", "超时 TIMEOUT"],
  ], 4.55, 1.25, 7.5);
}

// ───────────────────────── AM 控制 ─────────────────────────
divider("叁", "AM 消磁器控制", "功能码 0x22 · RS232 通道");

cmdPage({
  head: "读配置", tag: "AM · 0x22 01",
  hex: "0x22\n·01",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 22 01 7E F8 32 9B",
    exp: "读取消磁器全部工作配置；字段与 0x02 写配置一致。",
    rsp: { hex: "53 77 01 00 15 00 DD 01 00 00 0A 00 05 00 00 00 00 00 00 00 01 02 00 35 A9 41 0C",
      exp: "err=0 · thr=10 · hit=5 · mode=2（实测）" } }],
  desc: "读取消磁器当前全部工作配置，字段与写配置（0x02）一致。",
  reqRows: [["cmd", "1", "0x01"]],
  rspRows: [["cmd / err + 配置", "—", "回显 + 当前配置（同 0x02 字段）"]],
});

cmdPage({
  head: "写配置", tag: "AM · 0x22 02",
  hex: "0x22\n·02",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 14 00 22 02 00 64 00 03 32 00 64 0A 00 00 00 02 00 00 31 C1 BF B6",
    exp: "阈值 100 · 命中 3 · 频率 50 Hz · 延时 100 ms · 脉宽 10 · 电压档 2 · 模式 0。",
    rsp: { hex: "53 77 01 00 07 00 DD 02 00 DC AF 64 B0",
      exp: "err=0 配置已写入并保存（实测）" } }],
  desc: "设置消磁器工作参数。多字节参数为 16 位（高字节在前）。",
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
  head: "链路探测", tag: "AM · 0x22 05",
  hex: "0x22\n·05",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 22 05 A2 8E 36 88",
    exp: "探测 RS232 链路；err=0 表示消磁器在线。",
    rsp: { hex: "53 77 01 00 08 00 DD 05 00 00 43 5F 02 44",
      exp: "err=0 · link=0 链路正常（实测）" } }],
  desc: "探测与消磁器的 RS232 链路是否在线，用于开机自检与排障。",
  reqRows: [["cmd", "1", "0x05"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["link", "1", "0 = 链路正常"],
  ],
});

cmdPage({
  head: "状态监控", tag: "AM · 0x22 06",
  hex: "0x22\n·06",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 22 06 7B A8 75 85",
    exp: "读取运行监控：链路 / 事件计数 / 消磁成败统计（无 err 字节）。",
    rsp: { hex: "53 77 01 00 18 00 DD 06 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 4F 75 0D 0B",
      exp: "link=0 · 空闲态计数全 0（实测）" } }],
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
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 07 00 22 07 00 D4 56 75 90",
    exp: "切换为模式 0（检测 + 消磁）并持久化保存。",
    rsp: { hex: "53 77 01 00 07 00 DD 07 00 A9 4F 59 27",
      exp: "err=0 模式已切换并保存（实测）" } }],
  desc: "仅切换消磁器工作模式并持久化保存。",
  reqRows: [
    ["cmd", "1", "0x07"],
    ["mode", "1", "0 检测+消磁 / 1 仅检测 / 2 待机"],
  ],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功"]],
});

{ // AM 错误码
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "AM 错误码（data[1]）", "AM · 0x22");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 54, bold: true, color: C.giant, fontFace: FT, margin: 0 });
  s.addText("所有 0x22 子命令的响应中，data[1] 为错误码；0 表示成功。", { x: M, y: 2.55, w: 3.5, h: 2, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0, lineSpacingMultiple: 1.25 });
  kvTable(s, [
    ["0", "成功 OK"], ["1", "参数非法 PARAM"], ["2", "忙 BUSY（解锁流程中）"],
    ["4", "链路故障 LINK"], ["6", "超时 TIMEOUT"],
  ], 4.55, 1.25, 7.5);
}

// ───────────────────────── 解锁命令 ─────────────────────────
divider("肆", "解锁命令", "功能码 0x23 · 含解锁流程图与灯控映射");

cmdPage({
  head: "取消", tag: "解锁 · 0x23 04",
  hex: "0x23\n·04",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 23 04 C9 52 EE 5E",
    exp: "取消当前解锁流程；磁块安全回降后回帧。",
    rsp: { hex: "53 77 01 00 07 00 DC 04 00 FD BC 6A 54",
      exp: "err=0（实测，空闲态取消）" } }],
  desc: "取消当前操作：磁块安全回降，回到空闲。解锁流程进行中仅允许本命令、0x05 状态查询与 0x09 进度查询；解锁中取消立即回 OK，回降完成由 0x0A 终帧（endReason=6）报告。",
  reqRows: [["cmd", "1", "0x04"]],
  rspRows: [["cmd / err", "2", "回显 + 0 = 成功（立即回帧）"]],
});

cmdPage({
  head: "状态查询", tag: "解锁 · 0x23 05",
  hex: "0x23\n·05",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 23 05 7E 4F 2F 5A",
    exp: "查询状态机；回帧含 state 与硬 / 软标计数快照。",
    rsp: { hex: "53 77 01 00 0F 00 DC 05 00 00 00 00 00 00 00 00 00 0A 43 61 7E",
      exp: "err=0 · state=0 空闲 · 计数全 0（实测）" } }],
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
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 23 09 1A D4 23 6F",
    exp: "拉取实时进度：phase / 解锁窗 / 确认位图 / 软标计数。",
    rsp: { hex: "53 77 01 00 10 00 DC 09 00 00 00 00 00 00 00 00 00 00 A3 43 E9 0C",
      exp: "err=0 · phase=0 · 全零（实测无流程）" } }],
  desc: "解锁流程中拉取实时进度；无流程时 phase = 0、计数全零。",
  reqRows: [["cmd", "1", "0x09"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["phase", "1", "0 无 / 1 等放标 / 2 校对 / 3 升起 / 4 回降 / 5 软标 / 6 完成"],
    ["holdMs", "3", "自放标触发已过 ms（3 字节小端，未触发为 0）"],
    ["total / confirmed", "2", "期望标签数 / 已确认数"],
    ["bitmap", "1", "确认位图：bit n = 第 n 张已确认"],
    ["softCnt / softDone", "2", "软标已解数 / 软标完成标志"],
  ],
});

cmdPage({
  head: "解锁（多标签整合）· 参数", tag: "解锁 · 0x23 0A",
  hex: "0x23\n·0A",
  testY: 3.42,
  test: [
    { tag: "测试帧 · 单标签", hex: "53 77 01 00 19 00 23 0A E8 03 30 75 00 01 0C E2 80 68 94 00 00 12 34 56 78 90 1A 2D C6 99 85",
      exp: "盘点 1 s · 窗 30 s · 1 张硬标。",
      rsp: { hex: "53 77 01 00 13 00 DC 0A 00 02 00 00 01 00 00 00 00 00 00 6C 75 25 A2 22 7C",
        exp: "终帧 err=0 · endReason=2（实测）" } },
    { tag: "测试帧 · 双标签", hex: "53 77 01 00 25 00 23 0A E8 03 30 75 00 02 0C E2 80 68 94 00 00 12 34 56 78 90 1A E2 80 68 94 00 00 12 34 56 78 90 1B E8 DB 7F 22",
      exp: "同参数 · 期望 2 张（1A / 1B）。",
      rsp: { hex: "53 77 01 00 13 00 DC 0A 00 02 00 00 02 00 00 00 00 00 00 6E 75 37 DF 10 CE",
        exp: "终帧 err=0 · endReason=2（实测）" } },
  ],
  desc: "解锁主命令：单帧下发全部期望 EPC 与软标数，设备阻塞自治完成整个解锁流程（详见流程图页）。",
  reqRows: [
    ["cmd", "1", "0x0A"],
    ["tmo", "2", "每轮盘点超时 ms（缺省 500，小端）"],
    ["hold", "2", "解锁窗 W 上限 ms（0 = 公式默认，小端）"],
    ["softCnt", "1", "期望软标解码次数"],
    ["epcCnt", "1", "期望硬标签张数 m（≤ 4）；0 = 纯软标"],
    ["epcLen", "1", "每张 EPC 字节长度（≤ 12）"],
    ["epc…", "m×n", "m 张期望 EPC，每张 epcLen 字节"],
  ],
  note: "解锁窗 W：hold = 0 时按公式（首标签 2 min + 每多一张 30 s，上限 4 min）；hold ≠ 0 时取 min(hold, 4 min)。W 兼任放标等待窗（满窗未放标 err=11）；tmo 上限 10 s。纯软标（epcCnt=0 且 softCnt>0）跳过放标与校对，窗口即软标窗 5 min；epcCnt 与 softCnt 均为 0 → err=2。",
});

cmdPage({
  head: "解锁 · 流程推送帧", tag: "解锁 · 0x23 0A",
  hex: "0x23\n·0A",
  testY: 3.9,
  test: [
    { tag: "推送帧 · 0x0F 受理（实测捕获）", hex: "53 77 01 00 0B 00 DC 0F 00 01 30 75 00 7E 02 9F 7C",
      exp: "单标签受理推送：phase=1（等放标），窗 W=30 s（30 75 00 小端）。" },
    { tag: "推送帧 · 0x0C 失配（实测捕获）", hex: "53 77 01 00 14 00 DC 0C 0C 33 55 34 63 A4 00 01 58 EB 7E 75 07 02 F6 09 F6 BA",
      exp: "外来标签稳定确认（hits=2）一张一帧，不终止流程。" },
  ],
  desc: "解锁流程中设备主动推送的阶段事件帧（功能码同 0x23 响应方向，data[0] 为下述子码）；流程结束以 0x0A 终帧回显为标志。",
  rspRows: [
    ["0x0F", "1", "受理帧：流程已启动，含阶段与解锁窗 winMs（3 字节）"],
    ["0x0B", "1", "确认帧：某张期望 EPC 稳定确认（一张一帧 + 蜂鸣 200 ms）"],
    ["0x0C", "1", "失配帧：外来标签稳定确认（一张一帧，不终止流程）"],
    ["0x0D", "1", "硬标完成帧：硬标段结束（全部确认 / 窗满 / 取消 / 失联）"],
    ["0x0E", "1", "软标帧：软标解码累计数（达期望数即完成）"],
    ["0x0A 终帧", "1", "流程结束：err=0 时含结束原因、位图与各段耗时"],
  ],
  note: "解锁态内（受理帧起）除 0x04 取消与 0x05 / 0x09 查询外，其余子命令及 UHF / AM 控制一律回 BUSY。",
});

{ // 解锁错误码 + 结束原因
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "解锁 · 错误码与结束原因", "解锁 · 0x23");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 54, bold: true, color: C.giant, fontFace: FT, margin: 0 });
  s.addText("0x0A 终帧 err 字段（左）与 err=0 时的结束原因 endReason（右）。",
    { x: M, y: 2.55, w: 3.5, h: 2.5, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0, lineSpacingMultiple: 1.25 });
  s.addText("错误码（0x0A 终帧）", { x: 4.55, y: 1.25, w: 3.9, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  kvTable(s, [
    ["0", "成功"], ["1", "忙 BUSY（须先取消）"], ["2", "参数非法"],
    ["3 / 4", "UHF 启动 / 链路失败"], ["7", "未回零 / 行程开关错误"],
    ["8 / 9", "电机故障 / 电机超时"], ["10", "AM 链路故障"], ["11", "放标光电超时"],
  ], 4.55, 1.62, 3.9);
  s.addText("结束原因 endReason（err=0）", { x: 8.85, y: 1.25, w: 3.9, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  kvTable(s, [
    ["1", "ALL_OK：全部标签确认完成"], ["2", "硬标窗满，部分确认（位图明示）"],
    ["4", "UHF 链路失联"], ["6", "被 0x04 取消（已安全回降）"],
    ["7", "软标窗 5 min 满未校验完成"],
  ], 8.85, 1.62, 3.9);
}

// ───────────────────────── 解锁详细流程图 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "解锁流程图（0x0A 下发至完成）", "解锁 · 0x23 0A");

  const bx = 4.42, bw = 3.05, bh = 0.38, pitch = 0.5, y0 = 0.95;
  const NLAST = 11;
  // 主链
  const steps = [
    ["前置检查（空闲 · 已回零）", 0],
    ["下发解锁命令 0x0A（EPC×m · 软标数）", 0],
    ["受理 · 推送 0x0F（解锁窗 W）", 0],
    ["等待放标（光电门控 · 去抖 200 ms）", 0],
    ["周期盘点 · 读取 EPC", 0],
    ["EPC ∈ 期望清单？", 1],
    ["稳定确认：在场 1.5 s + 命中 ≥ 2", 0],
    ["确认 n+1 · 推送 0x0B（绿单闪）", 0],
    ["n == m（全部确认）？", 1],
    ["升起磁块 + 消磁开启（绿常亮）", 0],
    ["软标解码 · 推送 0x0D / 0x0E（5 min）", 0],
    ["回降寻触 · 终帧 0x0A 结账", 0],
  ];
  const centerY = i => y0 + i * pitch + bh / 2;
  steps.forEach((t, i) => {
    const y = y0 + i * pitch;
    if (t[1]) { // 判定菱形
      s.addShape(pres.shapes.DIAMOND, {
        x: bx + 0.35, y: y - 0.09, w: bw - 0.7, h: bh + 0.18,
        fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1.25 },
      });
    } else {
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
        x: bx, y, w: bw, h: bh, rectRadius: 0.05,
        fill: { color: i === NLAST ? C.headFill : "FFFFFF" },
        line: { color: i === NLAST ? C.green : C.border, width: i === NLAST ? 1.5 : 1 },
      });
    }
    s.addText(t[0], { x: bx - 0.3, y: y - (t[1] ? 0.09 : 0), w: bw + 0.6, h: bh + (t[1] ? 0.18 : 0), align: "center", valign: "middle", fontSize: 10, bold: t[1] === 1 || i === 0 || i === NLAST, color: C.ink, fontFace: FB, margin: 0 });
    if (i < steps.length - 1) s.addShape(pres.shapes.LINE, {
      x: bx + bw / 2, y: y + bh + (t[1] ? 0.09 : 0), w: 0, h: pitch - bh - (t[1] ? 0.18 : 0),
      line: { color: C.muted, width: 1.5, endArrowType: "triangle" },
    });
  });

  // 右分支: 外来 EPC (0x0C) — 判定①否
  const d1 = centerY(5), inv = centerY(4);
  s.addShape(pres.shapes.LINE, { x: bx + bw + 0.35, y: d1, w: 1.35, h: 0, line: { color: C.muted, width: 1.5, endArrowType: "triangle" } });
  s.addText("否", { x: bx + bw + 0.38, y: d1 - 0.32, w: 0.6, h: 0.28, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 8.85, y: d1 - 0.55, w: 3.85, h: 1.1, rectRadius: 0.06,
    fill: { color: C.rowFill }, line: { color: C.border, width: 1 },
  });
  s.addText([
    { text: "外来标签（非期望 EPC）\n", options: { bold: true, fontSize: 12.5, color: C.ink } },
    { text: "推送 0x0C 失配帧 · 红快闪 3 s\n", options: { fontSize: 11, color: C.ink } },
    { text: "不计入确认 · 不终止流程", options: { fontSize: 10.5, color: C.muted } },
  ], { x: 8.98, y: d1 - 0.55, w: 3.6, h: 1.1, valign: "middle", fontFace: FB, margin: 0 });
  // 回到盘点 (折线)
  s.addShape(pres.shapes.LINE, { x: 10.75, y: inv, w: 0, h: d1 - 0.55 - inv, line: { color: C.muted, width: 1.5 } });
  s.addShape(pres.shapes.LINE, { x: bx + bw, y: inv, w: 10.75 - bx - bw, h: 0, line: { color: C.muted, width: 1.5, beginArrowType: "triangle" } });
  s.addText("回到盘点", { x: 7.6, y: inv - 0.34, w: 1.3, h: 0.28, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });

  // 判定 "是" 分支标注
  s.addText("是", { x: bx + bw / 2 + 0.1, y: d1 + 0.12, w: 0.6, h: 0.26, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });
  s.addText("是", { x: bx + bw / 2 + 0.1, y: centerY(8) + 0.12, w: 0.6, h: 0.26, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });

  // n<m 回环: 未全确认 -> 继续盘点
  const d2 = centerY(8);
  s.addShape(pres.shapes.LINE, { x: 3.75, y: inv, w: 0, h: d2 - inv, line: { color: C.muted, width: 1.5 } });
  s.addShape(pres.shapes.LINE, { x: 3.75, y: d2, w: bx - 3.75, h: 0, line: { color: C.muted, width: 1.5, beginArrowType: "triangle" } });
  s.addText("否 · 继续盘点", { x: 2.42, y: (inv + d2) / 2 - 0.2, w: 2.4, h: 0.28, fontSize: 10, color: C.muted, fontFace: FB, margin: 0, rotate: 0, align: "right" });

  // 左侧异常出口
  const exits = [
    ["前置失败（忙 / 未回零）", "err = 1 / err = 7", centerY(0)],
    ["放标光电超时", "err = 11", centerY(3)],
    ["硬标窗 W 满 · 部分确认", "endReason = 2", (centerY(6) + d2) / 2],
    ["UHF 失联 5 s", "endReason = 4", (centerY(6) + d2) / 2 + 1.0],
    ["任意阶段 0x04 取消", "安全回降 · endReason = 6", centerY(11)],
  ];
  s.addText("异常出口", { x: M, y: 1.0, w: 3.0, h: 0.32, fontSize: 12.5, bold: true, color: C.green, fontFace: FB, margin: 0 });
  exits.forEach((e, i) => {
    const y = e[2] - 0.34;
    s.addShape(pres.shapes.RECTANGLE, { x: M, y, w: 2.85, h: 0.68, fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1 } });
    s.addText([
      { text: e[0] + "\n", options: { bold: true, fontSize: 11, color: C.ink } },
      { text: e[1], options: { fontSize: 10, color: C.muted } },
    ], { x: M + 0.1, y, w: 2.65, h: 0.68, valign: "middle", fontFace: FB, margin: 0 });
    s.addShape(pres.shapes.LINE, { x: M + 2.85, y: e[2], w: bx - M - 2.85, h: 0, line: { color: C.goldLine, width: 1, dashType: "dash", endArrowType: "triangle" } });
  });

  s.addText("阶段失败码：err=3/4 UHF 启动或链路 · err=10 AM 链路（softCnt>0 时）· err=8/9 电机（自动回退安全位）。解锁态内仅受理 0x04 / 0x05 / 0x09，其余命令一律 BUSY。",
    { x: 8.85, y: 6.15, w: 3.85, h: 0.95, fontSize: 10.5, color: C.muted, fontFace: FB, margin: 0, lineSpacingMultiple: 1.25 });
  s.addText("纯软标模式（epcCnt=0）：跳过等放标与 EPC 校对，受理即计时，直接进入升起 + 消磁 + 软解码；软标窗 5 min 满未完成按超时失败结账（endReason=7）。",
    { x: M, y: 7.05, w: 12.1, h: 0.35, fontSize: 11, color: C.muted, fontFace: FB, margin: 0 });
}

// ───────────────────────── 灯控映射表 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "灯控映射表（RGB 灯语）", "解锁 · 灯语");
  const rows = [
    ["FFFFFF", "白 · 慢闪", "等待放置标签"],
    ["4D7CC7", "蓝 · 慢闪", "盘点 / 校对进行中"],
    ["4D7CC7", "蓝 · 单闪", "读到一张标签（瞬时提示）"],
    ["3FA34D", "绿 · 单闪", "一张期望标签确认成功"],
    ["E04545", "红 · 快闪 3 s", "外来（非期望）标签"],
    ["3FA34D", "绿 · 常亮", "已开锁（磁块升起保持）"],
    ["FFFFFF", "白 · 常亮", "软标解码等待 / 进行中"],
    ["FFFFFF", "白 · 单闪", "软标消磁成功一次（瞬时提示）"],
    ["3FA34D", "绿 · 三连闪", "解锁流程完成"],
    ["E04545", "红 · 双闪", "操作失败"],
    ["E04545", "红 · 常亮", "设备故障"],
    ["E8C33C", "黄 · 慢闪", "回零 / 自检 · 放标提醒"],
  ];
  const y0 = 1.25, rh = 0.42;
  s.addText("颜色 / 节奏", { x: 8.45, y: y0 - 0.38, w: 2.2, h: 0.3, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  s.addText("含义", { x: 11.05, y: y0 - 0.38, w: 2.0, h: 0.3, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  rows.forEach((r, i) => {
    const y = y0 + i * (rh + 0.06);
    s.addShape(pres.shapes.OVAL, { x: 8.45, y: y + 0.05, w: 0.3, h: 0.3, fill: { color: r[0] }, line: { color: C.border, width: 0.75 } });
    s.addText(r[1], { x: 8.9, y, w: 2.05, h: rh, fontSize: 11.5, color: C.ink, fontFace: FB, valign: "middle", margin: 0 });
    s.addText(r[2], { x: 11.05, y, w: 2.0, h: rh, fontSize: 11, color: C.ink, fontFace: FB, valign: "middle", margin: 0 });
    s.addShape(pres.shapes.LINE, { x: 8.45, y: y + rh + 0.03, w: 4.6, h: 0, line: { color: "E4E2D8", width: 0.75 } });
  });

  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: M, y: 1.25, w: 7.35, h: 5.3, rectRadius: 0.08,
    fill: { color: "FFFFFF", transparency: 25 }, line: { color: C.goldLine, width: 1.25 },
  });
  s.addText("灯语说明", { x: M + 0.35, y: 1.55, w: 6.9, h: 0.4, fontSize: 16, bold: true, color: C.green, fontFace: FT, margin: 0 });
  const notes = [
    "灯语与解锁流程阶段一一对应，操作人员凭灯光颜色与节奏即可判断当前进度。",
    "确认（绿单闪）与失配（红快闪）为瞬时提示，闪毕回落当前阶段的稳态灯语。",
    "上位机可以 0x24 手动设置灯色（设备空闲时），10 s 后自动回收，业务灯语优先。",
    "任一时刻红灯常亮表示设备故障，请结合 0x23 05 状态查询定位原因。",
  ];
  s.addText(notes.map((t, i) => ({ text: t, options: { bullet: { code: "2013", indent: 12 }, breakLine: i < notes.length - 1 } })),
    { x: M + 0.35, y: 2.15, w: 6.9, h: 4.1, fontSize: 13.5, color: C.ink, fontFace: FB, paraSpaceAfter: 14, margin: 0, valign: "top", lineSpacingMultiple: 1.3 });
}

// ───────────────────────── 结尾 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  s.addImage({ path: `${A}/wash_right.png`, x: W - 4.5, y: H - 2.6, w: 4.5, h: 2.6, transparency: 15, flipH: true, flipV: true });
  s.addShape(pres.shapes.LINE, { x: M, y: 2.95, w: 2.4, h: 0, line: { color: C.goldLine, width: 1.5 } });
  s.addText("V1.0 · 初版定版", { x: M, y: 3.25, w: 8, h: 0.8, fontSize: 38, bold: true, color: C.green, fontFace: FT, margin: 0 });
  s.addText("本协议 V1.0 于 2026-08-24 初版定版；后续如有变更，将随版本号同步更新并记录于版本说明。",
    { x: M, y: 4.25, w: 9, h: 0.5, fontSize: 14, color: C.muted, fontFace: FB, margin: 0 });
}

pres.writeFile({ fileName: "/Users/swnw/Documents/Software/ZLR5401/Protocol/App_Protocol_V1.0.pptx" })
  .then(() => console.log("OK"));
