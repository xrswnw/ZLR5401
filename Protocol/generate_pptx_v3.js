// ZLR5401 应用层通信协议 — 客户版 PPT (V1.5 · 2026-09-29)
// V1.5: 解锁态门禁收紧为仅 0x04/0x09；外来缓存扩至 10；电机保持 2000 微步/s
// V1.4: 0x10 多标签名单 + 新状态机 (任意非空子集有效 / 外来介入报警 / 在场即保持)
//   · 0x11 恢复 AM 控制 (受理切消磁 → 达标/超窗挂起) · 0x09 进度 v2 (motorPh / flags)
//   · 电机全腿统一 2000 微步/s · 蜂鸣增补异常报警节奏
// V1.3: 解锁章 0x0A→0x10 EPC + 0x11 AM 双通道重做 · 双流程图 · 灯控映射更新
//   增补: AM 消磁器控制章移除 (固件零控制, 0x22 转厂商内部) · hex 单行化 + 动态测试帧防踩踏
// 风格: 晕染水彩 (白纸底 · 青绿晕染 · 金色点缀 · 宋体标题) — 参照水彩风模板 (17)
const pptxgen = require("pptxgenjs");

const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE";
pres.author = "ZLR5401";
pres.title = "ZLR5401 应用层通信协议 V1.5";

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

function paramTable(slide, rows, x, y, w, fieldW = 1.55) {
  const header = ["字段", "字节", "说明"].map(t => ({ text: t, options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5 } }));
  const body = rows.map((r, i) => r.map((c, j) => ({
    text: String(c),
    options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12, align: j === 1 ? "center" : "left" },
  })));
  slide.addTable([header, ...body], {
    x, y, w, colW: [fieldW, 0.85, w - fieldW - 0.85],
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
  return y - 0.1; // 栈底 (末框下缘, 供 cmdPage 溢出审计)
}

// desc 占行估算 (13.5pt 雅黑 · 列宽 3.5"=252pt): CJK/全角≈1em · ASCII≈0.55em · 空格≈0.33em
// 校准于 V1.3 PDF 实测: 行距 0.281"/行, p34 desc=4 行 / p35=6 行 逐页吻合
function estDescLines(desc) {
  if (!desc) return 0;
  const WIDE = new Set(["→", "—", "…", "×", "～"]);
  let lines = 1, w = 0;
  for (const ch of desc) {
    const cw = ch.codePointAt(0) >= 0x2E80 || WIDE.has(ch) ? 13.5 : ch === " " ? 4.4 : 7.4;
    if (w + cw > 250) { lines++; w = cw; } else w += cw;
  }
  return lines;
}

function cmdPage({ head, tag, hex, desc, reqRows, rspRows, note, test, testY, fieldW }) {
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, head, tag);
  // 主命令墨绿 / 子命令金色 双色区分 (单行拼接: 双行 54pt 第二行会下探 0.9" 压进 desc, V1.3 增补实测修正)
  const hexParts = hex.split("\n");
  s.addText(
    hexParts.map((t, i) => ({ text: (i ? " " : "") + t, options: { color: i === 0 ? C.giant : C.gold } })),
    { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 54, bold: true, fontFace: FT, margin: 0, valign: "top" }
  );
  s.addText(desc || "", { x: M, y: 2.55, w: 3.5, h: 3.3, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.25 });
  let y = 1.25;
  const tw = W - M - 4.55;
  if (reqRows) {
    s.addText("请求 data", { x: 4.55, y, w: tw, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
    paramTable(s, reqRows, 4.55, y + 0.36, tw, fieldW);
    y += 0.36 + 0.34 * (reqRows.length + 1) + 0.28;
  }
  if (rspRows) {
    s.addText("响应 data", { x: 4.55, y, w: tw, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
    paramTable(s, rspRows, 4.55, y + 0.36, tw, fieldW);
    y += 0.36 + 0.34 * (rspRows.length + 1) + 0.28;
  }
  if (note) s.addText(note, { x: 4.55, y: Math.min(y, 6.7), w: tw, h: 0.68, fontSize: 11.5, color: C.muted, fontFace: FB, margin: 0, valign: "top" });
  if (test && test.length) {
    // 防踩踏: 测试帧起点不低于 desc 估算底部 + 0.15" (V1.3 增补, 客户反馈 desc 压帧)
    const descBottom = 2.55 + estDescLines(desc) * 0.281;
    const ty = Math.max(testY !== undefined ? testY : 4.48, descBottom + 0.15);
    const bottom = testFrameBox(s, test, ty);
    if (bottom > 7.4) console.warn(`[layout] ${head}: 测试帧栈底 ${bottom.toFixed(2)}" 超出页底 7.4"`);
  }
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
    { text: "V1.5", options: { bold: true, color: C.gold } },
    { text: "（2026-09-29）", options: { color: C.muted } },
  ], { x: 4.3, y: 4.0, w: 4.75, h: 0.5, fontSize: 18, fontFace: FT, align: "center", margin: 0 });
  s.addText("帧格式 · 系统层命令 · UHF 控制 · 解锁命令 · 灯控映射", {
    x: 2.17, y: 6.65, w: 9.0, h: 0.4, fontSize: 12.5, color: C.muted, fontFace: FB, align: "center", margin: 0,
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
  const vrows = [
    ["V1.0", "2026-08-24", "初版定版"],
    ["V1.1", "2026-09-12", "增补 CRC 算法与 HID 传输层"],
    ["V1.2", "2026-09-19", "增补行程开关异常灯语"],
    ["V1.3", "2026-09-20", "解锁 0x10 / 0x11 双通道；移除 AM 控制章"],
    ["V1.4", "2026-09-29", "0x10 多标签新状态机；0x11 恢复 AM 控制；电机提速 2000"],
    ["V1.5", "2026-09-29", "电机保持 2000；解锁态仅 0x04/0x09；外来缓存扩至 10"],
  ];
  s.addTable([vh, ...vrows.map((r, i) => r.map((c, j) => ({
    text: c, options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12, align: j < 2 ? "center" : "left" },
  })))], { x: 7.0, y: 4.95, w: W - M - 7.0, colW: [0.9, 1.5, 3.73], border: TB, fontFace: FB, margin: 0.05, valign: "middle", rowH: 0.28 });
}

// ───────────────────────── CRC-32 校验算法 (V1.1 增补 · V1.3 收编) ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "CRC-32 校验算法", "第 1 章");
  const th = [
    { text: "参数", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5, align: "center" } },
    { text: "取值", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5 } },
  ];
  const rows = [
    ["算法名称", "CRC-32 / MPEG-2"],
    ["生成多项式", "0x04C11DB7（正序 · MSB first）"],
    ["初值 Init", "0xFFFFFFFF"],
    ["输入反射 RefIn", "无"],
    ["输出反射 RefOut", "无"],
    ["结果异或 XorOut", "无"],
    ["存放", "4 字节小端（低字节在前）追加帧尾"],
  ];
  s.addTable([th, ...rows.map((r, i) => [
    { text: r[0], options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12 } },
    { text: r[1], options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12 } },
  ])], { x: M, y: 1.15, w: 6.4, colW: [1.9, 4.5], border: TB, fontFace: FB, margin: 0.06, valign: "middle", rowH: 0.32 });
  s.addText([
    { text: "校验范围\n", options: { bold: true, fontSize: 13, color: C.green } },
    { text: "自帧头（53 77）起，至数据区末尾止 — 不含 CRC 字段本身；逐字节、按位 MSB first 计算。\n", options: { fontSize: 12.5, color: C.ink } },
    { text: "验证例\n", options: { bold: true, fontSize: 13, color: C.green } },
    { text: "握手请求 53 77 01 00 05 00 01 → CRC = 26 2F 69 C5，线上字节序 C5 69 2F 26。", options: { fontSize: 12.5, color: C.ink } },
  ], { x: 7.35, y: 1.15, w: 5.4, h: 2.2, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.25 });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: M, y: 3.85, w: W - 2 * M, h: 2.55, rectRadius: 0.06,
    fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1.25 },
  });
  s.addText("C 参考实现", { x: M + 0.25, y: 4.0, w: 3, h: 0.3, fontSize: 11, bold: true, color: C.gold, fontFace: FB, margin: 0 });
  s.addText(
`uint32_t crc32_mpeg2(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFF;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;          /* 低字节在前（小端）追加帧尾 */
}`,
    { x: M + 0.25, y: 4.35, w: W - 2 * M - 0.5, h: 1.9, fontSize: 11, fontFace: "Courier New", color: C.ink, margin: 0, valign: "top", lineSpacingMultiple: 1.15 });
}

// ───────────────────────── USB HID 传输层 (V1.1 增补 · V1.3 收编) ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "USB HID 传输层", "第 1 章");
  const th = [
    { text: "项目", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5, align: "center" } },
    { text: "内容", options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 12.5 } },
  ];
  const rows = [
    ["设备标识", "VID 0x5377 · PID 0x5378 · 产品名 SwCcrd · bcdDevice 0x0200 · 自供电 100 mA"],
    ["接口", "接口 0 · HID 类（class 03 / subclass 00 / protocol 00）· 报告描述符 43 字节 · 无报告 ID"],
    ["端点", "EP1 IN（地址 0x81）· 中断传输 · 64 字节 · 轮询 16 ms　｜　EP2 OUT（地址 0x02）· 中断传输 · 64 字节 · 轮询 16 ms"],
    ["输出报告（上位机→设备）", "[0x00] + 帧流（每报告 64 字节，有效载荷 ≤ 63 字节）"],
    ["输入报告（设备→上位机）", "[0x02] + 帧流（每个报告均带此头字节，含续包）"],
    ["分包规则", "帧长 > 63 字节时跨多报告续传，帧流按序拼接；帧与帧首尾相接，按 53 77 同步头与长度域切分"],
  ];
  s.addTable([th, ...rows.map((r, i) => [
    { text: r[0], options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12 } },
    { text: r[1], options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 12 } },
  ])], { x: M, y: 1.15, w: W - 2 * M, colW: [2.5, 9.63], border: TB, fontFace: FB, margin: 0.06, valign: "middle", rowH: 0.32 });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: M, y: 3.55, w: W - 2 * M, h: 1.95, rectRadius: 0.06,
    fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1.25 },
  });
  s.addText("完整交互示例 — 握手 0x01（单报告即可承载，剩余字节补零）", { x: M + 0.25, y: 3.7, w: 8, h: 0.3, fontSize: 11, bold: true, color: C.gold, fontFace: FB, margin: 0 });
  s.addText(
`OUT: 00 53 77 01 00 05 00 01 C5 69 2F 26                  ← [0x00] + 请求帧（11 B）
IN : 02 53 77 01 00 21 00 FE 00 03 01 1C D9 37 30 38 33 0B 00 39 35 37 39
     9D 43 A2 4D 01 00 00 00 00 00 C2 01 00 25 56 0B 38  ← [0x02] + 响应帧（39 B）`,
    { x: M + 0.25, y: 4.05, w: W - 2 * M - 0.5, h: 1.3, fontSize: 10.5, fontFace: "Courier New", color: C.ink, margin: 0, valign: "top", lineSpacingMultiple: 1.2 });
  s.addText([
    { text: "时序注意\n", options: { bold: true, fontSize: 13, color: C.green } },
    { text: "① 软复位（0x08）后设备 USB 重新枚举，上位机须重连再握手；② 复位后 UHF 模块约 3–5 s 才就绪，期间盘点命令超时，建议先查链路状态（0x21 07）。", options: { fontSize: 12.5, color: C.ink } },
  ], { x: M, y: 5.75, w: W - 2 * M, h: 1.15, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.25 });
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
    ["0x23", "解锁命令", "开锁业务", "EPC 解锁 0x10 / AM 解锁 0x11、取消、状态与进度查询、事件推送"],
  ];
  const header = ["功能码", "类别", "名称", "用途"].map(t => ({ text: t, options: { bold: true, fill: { color: C.headFill }, color: C.green, fontSize: 13 } }));
  const body = rows.map((r, i) => r.map((c, j) => ({
    text: c, options: { fill: { color: i % 2 ? C.rowFill : "FFFFFF" }, color: C.ink, fontSize: 13, align: j < 2 ? "center" : "left" },
  })));
  s.addTable([header, ...body], {
    x: M, y: 1.3, w: W - 2 * M, colW: [1.3, 1.7, 2.2, 6.93],
    border: TB, fontFace: FB, margin: 0.07, valign: "middle", rowH: 0.52,
  });
  s.addText("本文件仅公开系统层、UHF 与解锁命令；其余功能码（电机、灯、自检、IO、消磁器 0x22 等）为厂商内部接口。消磁器按默认参数自主运行，全程不受设备控制。",
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

// ───────────────────────── 解锁命令 ─────────────────────────
divider("叁", "解锁命令", "功能码 0x23 · 含解锁流程图与灯控映射");

cmdPage({
  head: "取消", tag: "解锁 · 0x23 04",
  hex: "0x23\n·04",
  test: [{ tag: "测试帧（实测）", hex: "53 77 01 00 06 00 23 04 C9 52 EE 5E",
    exp: "取消当前解锁流程；磁块安全回降后回帧。",
    rsp: { hex: "53 77 01 00 07 00 DC 04 00 FD BC 6A 54",
      exp: "err=0（实测，空闲态取消）" } }],
  desc: "取消当前操作，回到空闲。解锁流程进行中仅允许本命令、0x05 状态查询与 0x09 进度查询；取消立即回 OK，流程由各自终帧（0x10 / 0x11）以 endReason=6 报告（0x10 升起段取消先安全回降再结账；0x11 无机械段，窗内即回）。",
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
    ["state", "1", "0 空闲 / 1 等放标 / 2 校对 / 3 升起 / 4 回降 / 5 软标计数 / 7 保持 / 8 完成"],
    ["hm / sc / su", "6", "硬标期望 / 软标期望 / 软标已解（各 2 字节）"],
    ["faultReason", "1", "最近故障原因诊断码，0 = 无"],
  ],
});

cmdPage({
  head: "进度查询", tag: "解锁 · 0x23 09",
  hex: "0x23\n·09",
  test: [{ tag: "测试帧（实测 · v2 布局）", hex: "53 77 01 00 06 00 23 09 1A D4 23 6F",
    exp: "拉取实时进度：phase / 计时 / 位图 / 电机子状态 / 标志位。",
    rsp: { hex: "53 77 01 00 13 00 DC 09 00 07 2C 01 00 02 02 03 01 00 01 02 09 49 CC 18 30",
      exp: "err=0 · phase=7 保持 · 已过 300ms · 2/2 确认 · 在顶 · 位 09 = 稳定在场 + 红外遮挡" } }],
  desc: "解锁流程中拉取实时进度（V1.4 起 0x10 / 0x11 统一 v2 布局，14 字节）；无流程时 phase = 0、计数全零。",
  reqRows: [["cmd", "1", "0x09"]],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["phase", "1", "0 无 / 1 等放标 / 3 升起 / 4 回降 / 5 计数(0x11) / 7 顶部保持"],
    ["holdMs", "3", "自受理已过 ms（3 字节小端）"],
    ["total / confirmed / bitmap", "3", "名单张数 / 当前稳定在场数 / 位图（bit n = 第 n 张在场）"],
    ["softCnt / softDone", "2", "0x10：升至顶次数 / 报警次数 · 0x11：目标数 / 已解锁数"],
    ["lastCycle", "1", "0x10 最近电机事件：0 无 / 1 升至顶 / 2 全失回降 / 3 介入回降"],
    ["motorPh", "1", "0 静止 / 1 上行 / 2 在顶 / 3 下行（0x11 恒 0）"],
    ["flags", "1", "位图：稳定在场 / 介入 / 报警 / 红外遮挡 / 消磁中(0x11)"],
  ],
});

cmdPage({
  head: "EPC 解锁 · 参数", tag: "解锁 · 0x23 10",
  hex: "0x23\n·10",
  testY: 3.42,
  test: [
    { tag: "测试帧 · 2 标签名单（tmo=30 s）", hex: "53 77 01 00 17 00 23 10 1E 00 02 06 AA AA AA AA AA AA 06 55 55 55 55 55 55 E1 9A EF FD",
      exp: "流程窗 30 s · 名单 AA×6 + 55×6。",
      rsp: { hex: "53 77 01 00 12 00 DC 10 00 01 03 21 10 21 10 1E 00 01 00 01 C6 58 BA 66",
        exp: "err=0 · 在顶结账 · 升 1 次 · 报警 1 次。" } },
    { tag: "推送帧 · 0x0B 标签稳定确认", hex: "53 77 01 00 14 00 DC 0B 01 06 AA AA AA AA AA AA 01 02 51 00 03 00 95 EC 03 D5",
      exp: "第 1 张稳定在场 · 81 ms 判据。" },
  ],
  desc: "EPC 解锁（V1.4 重设计）：下发超时窗 + 名单 1~10 张变长 EPC；任意非空子集稳定在场 + 红外遮挡 → 升起 → 在场即保持，异常报警快速下行、恢复再升，循环至超窗（详见流程图页）。",
  fieldW: 1.95,
  reqRows: [
    ["cmd", "1", "0x10"],
    ["tmo", "2", "流程超时窗秒（u16 小端，>0，上限 600 钳位）"],
    ["epcCnt", "1", "名单张数 1 ~ 10（内部 10 槽缓冲）"],
    ["epcLen + epc…", "1 + n", "每张：长度 1 ~ 12 + EPC 字节，变长重复 × epcCnt"],
    ["rsv…", "n", "尾随保留字节忽略（不校验）"],
  ],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["endReason / bitmap", "2", "结束原因（见错误码页）/ 最终确认位 bit n = 第 n 张在场"],
    ["rise / lower", "4", "升 / 降实际微步数（各 2 字节小端）"],
    ["elapsed", "2", "自受理至终帧整秒（0xFFFF 封顶）"],
    ["riseCnt / alarmCnt / fgnSeen", "3", "升至顶次数 / 异常报警次数 / 是否出现过外来介入（0/1）"],
  ],
  note: "名单内 EPC 不得重复（违者 err=2）；外来标签 = 不在名单内的 EPC 累计读到 2 次即介入。蜂鸣仅三处 — 异常报警期间每 500ms 鸣 50ms、流程结束 ALL_OK 鸣 1 s，其余静默；CANCEL 随时安全回降 + 全状态复原。解锁态内仅响应 0x04 取消 / 0x09 进度查询，其余一律 BUSY。",
});

cmdPage({
  head: "AM 解锁 · 参数", tag: "解锁 · 0x23 11",
  hex: "0x23\n·11",
  testY: 3.42,
  test: [
    { tag: "测试帧 · 3 张（tmo=120 s）", hex: "53 77 01 00 0B 00 23 11 03 78 00 A5 5A 7B BB 5B D5",
      exp: "期望消磁 3 张 · 计数窗 120 s。",
      rsp: { hex: "53 77 01 00 13 00 DC 11 00 01 00 00 00 00 78 00 03 03 00 A5 5A B2 09 15 5D",
        exp: "err=0 · 3/3 达标 · 挂起成功。" } },
    { tag: "推送帧 · 0x0E 每解锁一张", hex: "53 77 01 00 08 00 DC 0E 01 03 40 B3 99 4B",
      exp: "done=1 · 一张一帧 · 白单闪。" },
  ],
  desc: "AM 解锁（V1.4 恢复消磁控制）：受理即切消磁模式 → 逐帧即时计数（一条 = 一张，无等待无稳定判定）→ 达标 / 超窗即挂起消磁 → 蜂鸣 1 s + 绿三闪结账，全程不动电机（详见流程图页）。",
  reqRows: [
    ["cmd", "1", "0x11"],
    ["amCnt", "1", "期望张数 0 ~ 255；0 = 无计数门受理即结账 ALL_OK"],
    ["tmo", "2", "计数窗秒（u16 小端，>0，上限 16000）"],
    ["rsv0 / rsv1", "2", "保留位（设备不解释，终帧原样回显）"],
  ],
  rspRows: [
    ["cmd / err", "2", "回显 + 错误码"],
    ["endReason", "1", "1 ALL_OK 达标 / 3 TIMEOUT 窗满 / 6 ABORTED 取消"],
    ["rise / lower", "4", "恒 0（全程不动电机）"],
    ["elapsed", "2", "自受理至终帧整秒（u16）"],
    ["amCnt / deactDone", "2", "目标数回显 / 实际解锁数（窗满如实上报部分完成数）"],
    ["deactFail / rsv0 / rsv1", "3", "deactFail：1 = 挂起确认失败（解码器可能仍在消磁），其余恒 0；rsv 原样回显"],
  ],
  note: "达标检查优先于窗满（同拍边界）；逐帧即时入账，无窗末宽限。挂起确认失败的残余消磁风险无法根除（解码器自行恢复行为，客户已知）。CANCEL：立即尽力挂起 + ABORTED（无机械段）。蜂鸣仅达标 / 结束一处 — 1 s；每张消磁成功无蜂鸣，仅绿单闪。",
});

{ // 解锁错误码 + 结束原因 (双通道)
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "解锁 · 错误码与结束原因", "解锁 · 0x23");
  s.addText("ERR", { x: M, y: 1.25, w: 3.5, h: 1.15, fontSize: 54, bold: true, color: C.giant, fontFace: FT, margin: 0 });
  s.addText("0x10 / 0x11 终帧 err 字段（左）与 err=0 时的结束原因 endReason（右）。",
    { x: M, y: 2.55, w: 3.5, h: 2.5, fontSize: 13.5, color: C.ink, fontFace: FB, margin: 0, lineSpacingMultiple: 1.25 });
  s.addText("错误码（终帧 data[1]）", { x: 4.55, y: 1.25, w: 3.9, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  kvTable(s, [
    ["0", "成功"], ["1", "忙 BUSY（须先取消）"], ["2", "参数非法"],
    ["3 / 4", "UHF 启动 / 链路失败"], ["7", "未回零 / 行程开关错误"],
    ["8 / 9", "电机故障 / 电机超时"], ["10", "AM 链路 / 切模式失败（0x11）"],
    ["11", "光电等待窗满（0x10 · 已废除通道保留）"],
  ], 4.55, 1.62, 3.9);
  s.addText("结束原因 endReason（err=0）", { x: 8.85, y: 1.25, w: 3.9, h: 0.32, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  kvTable(s, [
    ["1", "ALL_OK：0x10 超窗时在顶（曾升起保持）/ 0x11 计数达标"],
    ["2", "（V1.4 起 0x10 不再触发，保留码位）"],
    ["3", "0x10 超窗未在顶（未升起 / 异常回降后未恢复）/ 0x11 计数窗满未达标"],
    ["4", "UHF 链路失联（0x10）"],
    ["6", "被 0x04 取消（0x11 无机械段，窗内即回）"],
  ], 8.85, 1.62, 3.9);
  s.addText("0x11 全程不动电机：err 7 / 8 / 9 对 0x11 不可达（编码保留兼容）；受理切消磁失败 3 次重试后按 err=10 结账，挂起失败仅终帧 deactFail=1 如实上报。0x0A 多标签解锁已停用（任意请求回 err=2）。",
    { x: 4.55, y: 5.35, w: 8.2, h: 0.9, fontSize: 11.5, color: C.muted, fontFace: FB, margin: 0, valign: "top", lineSpacingMultiple: 1.25 });
}

// ───────────────────────── EPC 解锁流程图 (0x10) ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "EPC 解锁流程图（0x10 下发至完成）", "解锁 · 0x23 10");

  const bx = 4.42, bw = 3.05, bh = 0.38, pitch = 0.5, y0 = 0.95;
  const NLAST = 9;
  const steps = [
    ["前置检查（空闲 · 已回零）", 0],
    ["下发 0x10（tmo · epcCnt · len+EPC×n）", 0],
    ["受理 · 推 0x0F（白慢闪 · phase=1）", 0],
    ["连续盘点 0x22 多标签（蓝慢闪 · 10 槽）", 0],
    ["非空子集稳定 + 红外遮挡 且无介入？", 1],
    ["升起至顶（2000 微步/s · 绿常亮）", 0],
    ["在顶保持：在场即保持 · 盘点监守不停", 0],
    ["外来介入 / 名单全失？", 1],
    ["超窗回降 KEY_DOWN → 终帧（在顶 ALL_OK）", 0],
  ];
  const centerY = i => y0 + i * pitch + bh / 2;
  steps.forEach((t, i) => {
    const y = y0 + i * pitch;
    if (t[1]) {
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

  // 右分支: 判定②是 -> 报警 + 快速下行
  const d1 = centerY(7), inv = centerY(3);
  s.addShape(pres.shapes.LINE, { x: bx + bw + 0.35, y: d1, w: 1.35, h: 0, line: { color: C.muted, width: 1.5, endArrowType: "triangle" } });
  s.addText("是", { x: bx + bw + 0.38, y: d1 - 0.32, w: 0.6, h: 0.28, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 8.85, y: d1 - 0.75, w: 3.85, h: 1.55, rectRadius: 0.06,
    fill: { color: C.rowFill }, line: { color: C.border, width: 1 },
  });
  s.addText([
    { text: "异常报警 + 快速下行\n", options: { bold: true, fontSize: 12, color: C.ink } },
    { text: "红灯 50 ms 亮 / 50 ms 灭闪（持续）\n", options: { fontSize: 10.5, color: C.ink } },
    { text: "蜂鸣 50 ms · 每 500 ms 一拍（同步）\n", options: { fontSize: 10.5, color: C.ink } },
    { text: "外来稳定消失（1.5 s）且子集恢复 → 经红外门重新升起，循环至超窗", options: { fontSize: 10, color: C.muted } },
  ], { x: 8.98, y: d1 - 0.75, w: 3.6, h: 1.55, valign: "middle", fontFace: FB, margin: 0 });

  s.addText("否", { x: bx + bw / 2 + 0.1, y: centerY(4) + 0.12, w: 0.6, h: 0.26, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });
  s.addText("否 · 继续保持", { x: bx + bw / 2 + 0.12, y: d1 + 0.12, w: 1.5, h: 0.26, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });

  // 判定①否: 未达标 -> 继续盘点 (左回环)
  s.addShape(pres.shapes.LINE, { x: 3.75, y: inv, w: 0, h: centerY(4) - inv, line: { color: C.muted, width: 1.5 } });
  s.addShape(pres.shapes.LINE, { x: 3.75, y: centerY(4), w: bx - 3.75, h: 0, line: { color: C.muted, width: 1.5, beginArrowType: "triangle" } });
  s.addText("否 · 继续盘点", { x: 2.42, y: (inv + centerY(4)) / 2 - 0.2, w: 2.4, h: 0.28, fontSize: 10, color: C.muted, fontFace: FB, align: "right", margin: 0 });

  // 左侧异常出口
  const exits = [
    ["前置失败（忙 / 未回零）", "err = 1 / err = 7", centerY(0)],
    ["帧非法（tmo=0 / 张数 / 重复 EPC）", "err = 2", centerY(1)],
    ["超窗未在顶（未升 / 未恢复）", "endReason = 3 · 黄慢闪", centerY(4)],
    ["UHF 失联 5 s", "endReason = 4", (centerY(4) + centerY(6)) / 2],
    ["任意阶段 0x04 取消", "安全回降 · endReason = 6", centerY(8)],
  ];
  s.addText("异常出口", { x: M, y: 1.0, w: 3.0, h: 0.32, fontSize: 12.5, bold: true, color: C.green, fontFace: FB, margin: 0 });
  exits.forEach((e) => {
    const y = e[2] - 0.34;
    s.addShape(pres.shapes.RECTANGLE, { x: M, y, w: 2.85, h: 0.68, fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1 } });
    s.addText([
      { text: e[0] + "\n", options: { bold: true, fontSize: 11, color: C.ink } },
      { text: e[1], options: { fontSize: 10, color: C.muted } },
    ], { x: M + 0.1, y, w: 2.65, h: 0.68, valign: "middle", fontFace: FB, margin: 0 });
    s.addShape(pres.shapes.LINE, { x: M + 2.85, y: e[2], w: bx - M - 2.85, h: 0, line: { color: C.goldLine, width: 1, dashType: "dash", endArrowType: "triangle" } });
  });

  s.addText("阶段失败码：err=3/4 UHF 启动或链路 · err=8/9 电机（自动回退安全位）。解锁态内仅受理 0x04 取消 / 0x09 查询，其余命令一律 BUSY。",
    { x: 8.85, y: 6.15, w: 3.85, h: 0.95, fontSize: 10.5, color: C.muted, fontFace: FB, margin: 0, lineSpacingMultiple: 1.25 });
  s.addText("判据：稳定在场 = 首读起 1.5 s 内无 1.5 s 级断读；外来介入 = 非名单 EPC 累计读到 2 次，1.5 s 未再读即消失。任意非空子集在场均有效。",
    { x: M, y: 6.92, w: 12.1, h: 0.45, fontSize: 10.5, color: C.muted, fontFace: FB, margin: 0, valign: "top" });
}

// ───────────────────────── AM 解锁流程图 (0x11) ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "AM 解锁流程图（0x11 下发至完成）", "解锁 · 0x23 11");

  const bx = 4.42, bw = 3.35, bh = 0.38, pitch = 0.5, y0 = 1.05;
  const steps = [
    ["下发 0x11（amCnt · tmo · rsv）", 0],
    ["受理 · 推 0x0F（phase=5 · winMs=tmo×1000）", 0],
    ["AM 切消磁模式（回显确认 · 重试 3 次）", 0],
    ["确认成功？", 1],
    ["计数窗 tmo 秒：逐帧即时入账", 0],
    ["每解锁一张 · 推 0x0E（白单闪 · 无蜂鸣）", 0],
    ["已解锁数 ≥ amCnt？", 1],
    ["AM 挂起切仅检测（失败 amFail=1 上报）", 0],
    ["蜂鸣 1 s + 绿三闪 · 终帧（rise/lower 恒 0）", 0],
  ];
  const NLAST = 8;
  const centerY = i => y0 + i * pitch + bh / 2;
  steps.forEach((t, i) => {
    const y = y0 + i * pitch;
    if (t[1]) {
      s.addShape(pres.shapes.DIAMOND, {
        x: bx + 0.45, y: y - 0.09, w: bw - 0.9, h: bh + 0.18,
        fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1.25 },
      });
    } else {
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
        x: bx, y, w: bw, h: bh, rectRadius: 0.05,
        fill: { color: i === NLAST ? C.headFill : "FFFFFF" },
        line: { color: i === NLAST ? C.green : C.border, width: i === NLAST ? 1.5 : 1 },
      });
    }
    s.addText(t[0], { x: bx - 0.35, y: y - (t[1] ? 0.09 : 0), w: bw + 0.7, h: bh + (t[1] ? 0.18 : 0), align: "center", valign: "middle", fontSize: 10.5, bold: t[1] === 1 || i === 0 || i === NLAST, color: C.ink, fontFace: FB, margin: 0 });
    if (i < steps.length - 1) s.addShape(pres.shapes.LINE, {
      x: bx + bw / 2, y: y + bh + (t[1] ? 0.09 : 0), w: 0, h: pitch - bh - (t[1] ? 0.18 : 0),
      line: { color: C.muted, width: 1.5, endArrowType: "triangle" },
    });
  });
  s.addText("是", { x: bx + bw / 2 + 0.1, y: centerY(6) + 0.12, w: 0.6, h: 0.26, fontSize: 10, color: C.muted, fontFace: FB, margin: 0 });
  s.addText("否 · 继续等（回计数窗）", { x: bx - 3.3, y: centerY(6) - 0.14, w: 3.05, h: 0.28, fontSize: 10, color: C.muted, fontFace: FB, align: "right", margin: 0 });
  s.addShape(pres.shapes.LINE, { x: bx - 0.25, y: centerY(6), w: 0, h: centerY(4) - centerY(6), line: { color: C.muted, width: 1.5 } });
  s.addShape(pres.shapes.LINE, { x: bx - 0.25, y: centerY(4), w: 0.25, h: 0, line: { color: C.muted, width: 1.5, endArrowType: "triangle" } });

  // 左侧异常出口
  const exits = [
    ["计数窗满未达标", "endReason = 3 · 黄慢闪 2 s", 3.0],
    ["窗内 0x04 取消", "endReason = 6（尽力挂起 · 无机械段）", 4.6],
    ["帧长 / tmo=0 非法", "err = 2", 6.2],
    ["切消磁模式失败（3 次重试）", "err = 10 · 红双闪", 7.6],
  ];
  s.addText("异常出口", { x: M, y: 1.15, w: 3.0, h: 0.32, fontSize: 12.5, bold: true, color: C.green, fontFace: FB, margin: 0 });
  exits.forEach((e) => {
    const y = e[2] - 0.34;
    s.addShape(pres.shapes.RECTANGLE, { x: M, y, w: 2.85, h: 0.68, fill: { color: "FFFFFF" }, line: { color: C.goldLine, width: 1 } });
    s.addText([
      { text: e[0] + "\n", options: { bold: true, fontSize: 11, color: C.ink } },
      { text: e[1], options: { fontSize: 10, color: C.muted } },
    ], { x: M + 0.1, y, w: 2.65, h: 0.68, valign: "middle", fontFace: FB, margin: 0 });
  });

  // 右侧要点框
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 8.85, y: 1.5, w: 3.85, h: 2.6, rectRadius: 0.06,
    fill: { color: C.rowFill }, line: { color: C.border, width: 1 },
  });
  s.addText([
    { text: "AM 消磁控制（V1.4 恢复）\n", options: { bold: true, fontSize: 13, color: C.green } },
    { text: "受理即切消磁模式，达标 / 超窗 / 取消即挂起（切仅检测）\n", options: { fontSize: 11, color: C.ink, breakLine: true } },
    { text: "模式切换经回显确认，失败重试 3 次\n", options: { fontSize: 11, color: C.ink, breakLine: true } },
    { text: "每收到一条消磁成功帧立即入账一计（无等待无稳定判定）\n", options: { fontSize: 11, color: C.ink, breakLine: true } },
    { text: "挂起确认失败不阻塞结账，终帧 deactFail=1 如实上报（解码器自行恢复风险，客户已知）", options: { fontSize: 11, color: C.ink } },
  ], { x: 8.98, y: 1.62, w: 3.6, h: 2.4, valign: "top", fontFace: FB, margin: 0, lineSpacingMultiple: 1.3 });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 8.85, y: 4.4, w: 3.85, h: 1.7, rectRadius: 0.06,
    fill: { color: C.rowFill }, line: { color: C.border, width: 1 },
  });
  s.addText([
    { text: "全程不动电机\n", options: { bold: true, fontSize: 13, color: C.green } },
    { text: "达标仅蜂鸣 1 s + 绿三连闪结账\n", options: { fontSize: 11, color: C.ink, breakLine: true } },
    { text: "rise / lower 恒 0 · 无升起 / 保持 / 回降\n", options: { fontSize: 11, color: C.ink, breakLine: true } },
    { text: "amCnt=0：受理即达标即时结账 ALL_OK", options: { fontSize: 11, color: C.ink } },
  ], { x: 8.98, y: 4.52, w: 3.6, h: 1.5, valign: "top", fontFace: FB, margin: 0, lineSpacingMultiple: 1.3 });
  s.addText("每张消磁成功由 0x0E 推送帧实时上报（done 累计计数）；整体进度经 0x09 进度查询拉取（phase=5 计数窗 · flags 位4=消磁中）。",
    { x: M, y: 7.05, w: 12.1, h: 0.35, fontSize: 11, color: C.muted, fontFace: FB, margin: 0 });
}

// ───────────────────────── 灯控映射表 ─────────────────────────
{
  const s = pres.addSlide();
  s.background = { color: C.paper };
  ornament(s);
  pageHead(s, "灯控映射表（RGB 灯语）", "解锁 · 灯语");
  const rows = [
    ["FFFFFF", "白 · 慢闪", "0x10 等待名单标签 + 红外遮挡"],
    ["4D7CC7", "蓝 · 慢闪", "0x10 连续盘点进行中"],
    ["3FA34D", "绿 · 单闪", "0x10 名单标签稳定确认（无蜂鸣）"],
    ["E04545", "红 · 快闪 3 s", "0x10 外来（非名单）标签失配（瞬时）"],
    ["E04545", "红 · 快闪（50/50）", "0x10 异常报警：外来介入 / 名单全失（持续至解除）"],
    ["3FA34D", "绿 · 常亮", "0x10 已开锁（升起 / 在顶保持）"],
    ["FFFFFF", "白 · 常亮", "0x11 AM 计数窗（消磁运行中）"],
    ["FFFFFF", "白 · 单闪", "0x11 一张消磁成功（无蜂鸣）"],
    ["E8C33C", "黄 · 慢闪 2 s", "超窗未达成（0x10 未在顶 / 0x11 窗满）"],
    ["3FA34D", "绿 · 三连闪", "解锁流程完成（ALL_OK 鸣 1 s）"],
    ["E04545", "红 · 双闪", "链路失败"],
    ["E04545", "红 · 常亮", "设备故障"],
    ["E8C33C", "黄 · 快闪", "行程开关异常（上行程）"],
    ["C355B5", "紫 · 快闪", "行程开关异常（下行程）"],
    ["E8C33C", "黄 · 慢闪", "回零 / 自检进行中"],
  ];
  const y0 = 1.25, rh = 0.32;
  s.addText("颜色 / 节奏", { x: 8.45, y: y0 - 0.38, w: 2.2, h: 0.3, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  s.addText("含义", { x: 11.05, y: y0 - 0.38, w: 2.0, h: 0.3, fontSize: 13, bold: true, color: C.green, fontFace: FB, margin: 0 });
  rows.forEach((r, i) => {
    const y = y0 + i * (rh + 0.06);
    s.addShape(pres.shapes.OVAL, { x: 8.45, y: y + 0.01, w: 0.3, h: 0.3, fill: { color: r[0] }, line: { color: C.border, width: 0.75 } });
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
    "灯语与流程阶段一一对应，操作人员凭灯光颜色与节奏即可判断当前进度；单闪类（绿 / 白）为瞬时提示，闪毕回落当前阶段稳态灯语。",
    "蜂鸣三处：0x10 异常报警期间红灯 50/50 闪同步每 500 ms 鸣 50 ms（持续至异常解除）、流程结束 ALL_OK 鸣 1 s（伴随绿三连闪）、0x11 达标鸣 1 s；标签确认与每张消磁成功均无蜂鸣，超时 / 取消无鸣。",
    "上位机可以 0x24 手动设置灯色（设备空闲时），10 s 后自动回收，业务灯语优先。",
    "紫 / 黄快闪为行程开关异常诊断灯语（优先级最高）：上行程异常 = 黄快闪、下行程异常 = 紫快闪、两路同错亦显黄快闪；应停机检修并联系厂商。",
    "红灯常亮表示设备故障（行程开关异常除外，见紫 / 黄快闪），请结合 0x23 05 状态查询定位原因。",
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
  s.addText("V1.5", { x: M, y: 3.25, w: 8, h: 0.8, fontSize: 38, bold: true, color: C.green, fontFace: FT, margin: 0 });
  s.addText("V1.5 于 2026-09-29 发布：解锁流程进行中收紧为仅响应 0x04 停止 / 0x09 进度两条指令，其余一律 BUSY；外来标签追踪缓存扩至 10 张；电机全腿保持 2000 微步/s。0x10 多标签名单状态机与 0x11 AM 消磁控制沿用 V1.4 设计。历史变更见版本说明。",
    { x: M, y: 4.25, w: 9, h: 0.5, fontSize: 14, color: C.muted, fontFace: FB, margin: 0 });
}

pres.writeFile({ fileName: "/Users/swnw/Documents/Software/ZLR5401/Protocol/App_Protocol_V1.5.pptx" })
  .then(() => console.log("OK"));
