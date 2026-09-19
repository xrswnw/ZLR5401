// ZLR5401 App USB Protocol — 蓝白高端 Word 版生成器
// 依据: Protocol/App_Protocol.html (协议版本 3)
const {
  Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell,
  Header, Footer, PageNumber, NumberFormat, AlignmentType, HeadingLevel,
  WidthType, BorderStyle, ShadingType, TableLayoutType, SectionType,
  TableOfContents, VerticalAlign,
} = require("docx");
const fs = require("fs");

// ───────────────────────── 蓝白配色 ─────────────────────────
const P = {
  primary: "0B3D91",     // 深蓝 — 一级标题/表头
  h2: "1A4FA0", h3: "2A5DB0",
  body: "1F2937", secondary: "5B6B8C",
  headerBg: "0B3D91", headerText: "FFFFFF",
  accentLine: "0B3D91", innerLine: "D8E2F2", surface: "EEF3FB",
  groupRow: "DCE8F8",
  codeBg: "0B2046", codeText: "E6EDF7", codeBorder: "0B3D91",
  noteBg: "EEF3FB", noteTitle: "0B3D91", noteBorder: "0B3D91",
  warnBg: "FFF8E8", warnTitle: "8A6A08", warnBorder: "E8B400",
  tipBg: "EEF7EE", tipTitle: "2E7D32", tipBorder: "2E7D32",
  coverBg: "F7FAFD", coverTitle: "0B3D91", coverSub: "44608C",
  coverAccent: "0B3D91", coverMeta: "33507F", coverFooter: "8DA3C0", coverLabel: "2A5DB0",
};

const F_HEAD = { ascii: "Arial", eastAsia: "Microsoft YaHei" };
const F_BODY = { ascii: "Calibri", eastAsia: "Microsoft YaHei" };
const F_MONO = { ascii: "Courier New", eastAsia: "Microsoft YaHei" };

const NB = { style: BorderStyle.NONE, size: 0, color: "FFFFFF" };
const noBorders = { top: NB, bottom: NB, left: NB, right: NB };
const allNoBorders = { top: NB, bottom: NB, left: NB, right: NB, insideHorizontal: NB, insideVertical: NB };

// ───────────────────────── 通用构件 ─────────────────────────
let tblNo = 0, curCh = 0;
function setCh(n) { curCh = n; tblNo = 0; }

function h1(text) {
  return new Paragraph({
    heading: HeadingLevel.HEADING_1,
    pageBreakBefore: true,
    spacing: { before: 360, after: 200, line: 312 },
    children: [new TextRun({ text, bold: true, size: 32, color: P.primary, font: F_HEAD })],
  });
}
function h2(text) {
  return new Paragraph({
    heading: HeadingLevel.HEADING_2,
    spacing: { before: 280, after: 140, line: 312 },
    children: [new TextRun({ text, bold: true, size: 28, color: P.h2, font: F_HEAD })],
  });
}
function h3(text) {
  return new Paragraph({
    heading: HeadingLevel.HEADING_3,
    spacing: { before: 220, after: 110, line: 312 },
    children: [new TextRun({ text, bold: true, size: 24, color: P.h3, font: F_HEAD })],
  });
}
function h4(text) {
  return new Paragraph({
    heading: HeadingLevel.HEADING_4,
    spacing: { before: 180, after: 90, line: 312 },
    children: [new TextRun({ text, bold: true, size: 22, color: P.h3, font: F_HEAD })],
  });
}
// 正文段（可混排 mono 片段：字符串 或 {m:文本}）
function body(parts) {
  const arr = Array.isArray(parts) ? parts : [parts];
  return new Paragraph({
    alignment: AlignmentType.JUSTIFIED,
    indent: { firstLine: 480 },
    spacing: { line: 312, after: 80 },
    children: arr.map(p => typeof p === "string"
      ? new TextRun({ text: p, size: 24, color: P.body, font: F_BODY })
      : new TextRun({ text: p.m, size: 22, color: P.primary, font: F_MONO })),
  });
}
function bullet(parts, opts) {
  const arr = Array.isArray(parts) ? parts : [parts];
  return new Paragraph({
    bullet: { level: 0 },
    alignment: AlignmentType.JUSTIFIED,
    spacing: { line: 312, after: 60 },
    children: arr.map(p => typeof p === "string"
      ? new TextRun({ text: p, size: (opts && opts.size) || 24, color: P.body, font: F_BODY })
      : new TextRun({ text: p.m, size: 22, color: P.primary, font: F_MONO })),
  });
}
function numItem(no, parts) {
  const arr = Array.isArray(parts) ? parts : [parts];
  return new Paragraph({
    alignment: AlignmentType.JUSTIFIED,
    indent: { left: 540, hanging: 360 },
    spacing: { line: 312, after: 60 },
    children: [
      new TextRun({ text: no + " ", bold: true, size: 24, color: P.primary, font: F_BODY }),
      ...arr.map(p => typeof p === "string"
        ? new TextRun({ text: p, size: 24, color: P.body, font: F_BODY })
        : new TextRun({ text: p.m, size: 22, color: P.primary, font: F_MONO })),
    ],
  });
}
// 提示框（note / warn / tip）
function callout(kind, title, lines) {
  const conf = {
    note: { bg: P.noteBg, tc: P.noteTitle, bd: P.noteBorder },
    warn: { bg: P.warnBg, tc: P.warnTitle, bd: P.warnBorder },
    tip: { bg: P.tipBg, tc: P.tipTitle, bd: P.tipBorder },
  }[kind];
  const arr = Array.isArray(lines) ? lines : [lines];
  const paras = [];
  arr.forEach((ln, i) => {
    const parts = Array.isArray(ln) ? ln : [ln];
    paras.push(new Paragraph({
      alignment: AlignmentType.JUSTIFIED,
      shading: { type: ShadingType.CLEAR, fill: conf.bg },
      border: { left: { style: BorderStyle.SINGLE, size: 16, color: conf.bd, space: 10 } },
      indent: { left: 120, right: 60 },
      spacing: { line: 300, before: i === 0 ? 40 : 0, after: i === arr.length - 1 ? 40 : 20 },
      children: (i === 0 && title
        ? [new TextRun({ text: title + "  ", bold: true, size: 21, color: conf.tc, font: F_HEAD })]
        : []).concat(parts.map(p => typeof p === "string"
          ? new TextRun({ text: p, size: 21, color: P.body, font: F_BODY })
          : new TextRun({ text: p.m, size: 20, color: P.primary, font: F_MONO }))),
    }));
  });
  return paras;
}
// 代码块（深蓝底浅字）
function codeBlock(lines) {
  return lines.map((ln, i) => new Paragraph({
    shading: { type: ShadingType.CLEAR, fill: P.codeBg },
    border: { left: { style: BorderStyle.SINGLE, size: 12, color: P.codeBorder, space: 8 } },
    indent: { left: 160, right: 120 },
    spacing: { line: 252, before: i === 0 ? 120 : 0, after: i === lines.length - 1 ? 160 : 0 },
    children: [new TextRun({ text: ln.length ? ln : " ", size: 17, color: P.codeText, font: F_MONO })],
  }));
}
// 表标题
function caption(text) {
  tblNo += 1;
  return new Paragraph({
    keepNext: true,
    alignment: AlignmentType.CENTER,
    spacing: { before: 160, after: 80, line: 300 },
    children: [new TextRun({ text: "表 " + curCh + "-" + tblNo + "  " + text, bold: true, size: 20, color: P.primary, font: F_HEAD })],
  });
}
// 单元格内容: 字符串 | {t, m, b, i, c} | 上述对象的数组（多段混排）
function cellRuns(v, size, isHeader) {
  const items = Array.isArray(v)
    ? v
    : [(typeof v === "object" && v !== null) ? v : { t: String(v) }];
  return items.map(o => new TextRun({
    text: o.t,
    bold: isHeader || !!o.b,
    italics: !!o.i,
    size: o.m ? size - 1 : size,
    color: isHeader ? P.headerText : (o.c || P.body),
    font: o.m ? F_MONO : F_BODY,
  }));
}
// 通用蓝白表格。rows 项: 数组=数据行; {group:文本}=整行合并组标题
function tbl(opts) {
  const { headers, rows, widths, size = 18, headSize } = opts;
  const fs_ = size, hs = headSize || size;
  const zebra = opts.zebra !== false;
  const trs = [];
  trs.push(new TableRow({
    tableHeader: true, cantSplit: true,
    children: headers.map((htext, i) => new TableCell({
      children: [new Paragraph({
        alignment: AlignmentType.CENTER,
        spacing: { line: 276 },
        children: cellRuns(htext, hs, true),
      })],
      shading: { type: ShadingType.CLEAR, fill: P.headerBg },
      margins: { top: 70, bottom: 70, left: 100, right: 100 },
      width: { size: widths[i], type: WidthType.PERCENTAGE },
      verticalAlign: VerticalAlign.CENTER,
    })),
  }));
  let di = 0;
  for (const r of rows) {
    if (r && r.group) {
      trs.push(new TableRow({
        cantSplit: true,
        children: [new TableCell({
          columnSpan: headers.length,
          children: [new Paragraph({
            spacing: { line: 276 },
            children: [new TextRun({ text: r.group, bold: true, italics: true, size: fs_, color: P.primary, font: F_BODY })],
          })],
          shading: { type: ShadingType.CLEAR, fill: P.groupRow },
          margins: { top: 50, bottom: 50, left: 100, right: 100 },
        })],
      }));
      continue;
    }
    const fill = zebra && (di % 2 === 1) ? P.surface : "FFFFFF";
    trs.push(new TableRow({
      cantSplit: true,
      children: r.map((v, i) => new TableCell({
        children: [new Paragraph({
          alignment: (opts.centerCols && opts.centerCols.includes(i)) ? AlignmentType.CENTER : AlignmentType.LEFT,
          spacing: { line: 276 },
          children: cellRuns(v, fs_, false),
        })],
        shading: { type: ShadingType.CLEAR, fill },
        margins: { top: 55, bottom: 55, left: 100, right: 100 },
        width: { size: widths[i], type: WidthType.PERCENTAGE },
        verticalAlign: VerticalAlign.CENTER,
      })),
    }));
    di += 1;
  }
  return new Table({
    width: { size: 100, type: WidthType.PERCENTAGE },
    layout: TableLayoutType.FIXED,
    borders: {
      top: { style: BorderStyle.SINGLE, size: 8, color: P.accentLine },
      bottom: { style: BorderStyle.SINGLE, size: 8, color: P.accentLine },
      left: NB, right: NB,
      insideHorizontal: { style: BorderStyle.SINGLE, size: 2, color: P.innerLine },
      insideVertical: NB,
    },
    rows: trs,
  });
}
// 逐字节注释表
function byteTable(rows) {
  return tbl({
    headers: ["偏移", "字节", "字段", "详细含义"],
    rows,
    widths: [8, 11, 21, 60],
    size: 18,
    centerCols: [0, 1],
  });
}
const B = (t) => ({ t, m: true });   // mono 单元格

// ───────────────────────── 封面（R2 双线框 · 蓝白） ─────────────────────────
function splitTitleLines(title, charsPerLine) {
  if (title.length <= charsPerLine) return [title];
  const breakAfter = new Set([..."，。、；：！？", ..."的与和及之在于为", ..."-_—–·/", ..." \t"]);
  const lines = []; let remaining = title;
  while (remaining.length > charsPerLine) {
    let breakAt = -1;
    for (let i = charsPerLine; i >= Math.floor(charsPerLine * 0.6); i--) {
      if (i < remaining.length && breakAfter.has(remaining[i - 1])) { breakAt = i; break; }
    }
    if (breakAt === -1) {
      const limit = Math.min(remaining.length, Math.ceil(charsPerLine * 1.3));
      for (let i = charsPerLine + 1; i < limit; i++) {
        if (breakAfter.has(remaining[i - 1])) { breakAt = i; break; }
      }
    }
    if (breakAt === -1) {
      breakAt = charsPerLine;
      const prevChar = remaining[breakAt - 1], nextChar = remaining[breakAt];
      if (prevChar && nextChar && !breakAfter.has(prevChar) && !breakAfter.has(nextChar)
        && /[\u4e00-\u9fff]/.test(prevChar) && /[\u4e00-\u9fff]/.test(nextChar)) breakAt -= 1;
    }
    lines.push(remaining.slice(0, breakAt).trim());
    remaining = remaining.slice(breakAt).trim();
  }
  if (remaining) lines.push(remaining);
  if (lines.length > 1 && lines[lines.length - 1].length <= 2) {
    const last = lines.pop(); lines[lines.length - 1] += last;
  }
  return lines;
}
function calcTitleLayout(title, maxWidthTwips, preferredPt = 40, minPt = 24) {
  const charWidth = (pt) => pt * 20;
  const charsPerLine = (pt) => Math.floor(maxWidthTwips / charWidth(pt));
  let titlePt = preferredPt, lines;
  while (titlePt >= minPt) {
    const cpl = charsPerLine(titlePt);
    if (cpl < 2) { titlePt -= 2; continue; }
    lines = splitTitleLines(title, cpl);
    if (lines.length <= 3) break;
    titlePt -= 2;
  }
  if (!lines || lines.length > 3) {
    lines = splitTitleLines(title, charsPerLine(minPt)); titlePt = minPt;
  }
  return { titlePt, titleLines: lines };
}
function buildCoverR2(config) {
  const padL = 1400, padR = 1400;
  const { titlePt, titleLines } = calcTitleLayout(config.title, 11906 - padL - padR, 40, 24);
  const titleSize = titlePt * 2;
  const thickBorder = { style: BorderStyle.SINGLE, size: 18, color: P.coverAccent, space: 20 };
  const children = [];
  children.push(new Paragraph({
    indent: { left: padL - 400, right: padR - 400 }, spacing: { before: 1200, after: 200 },
    border: { top: thickBorder }, children: [],
  }));
  children.push(new Paragraph({ spacing: { before: 1600 } }));
  children.push(new Paragraph({
    alignment: AlignmentType.CENTER, spacing: { after: 520 },
    children: [new TextRun({ text: config.englishLabel.split("").join("  "), size: 18, color: P.coverLabel, font: { ascii: "Calibri" }, characterSpacing: 40 })],
  }));
  for (let i = 0; i < titleLines.length; i++) {
    children.push(new Paragraph({
      alignment: AlignmentType.CENTER,
      spacing: { after: i < titleLines.length - 1 ? 80 : 320, line: Math.ceil(titlePt * 23), lineRule: "atLeast" },
      children: [new TextRun({ text: titleLines[i], size: titleSize, bold: true, color: P.coverTitle, font: { eastAsia: "Microsoft YaHei", ascii: "Arial" } })],
    }));
  }
  children.push(new Paragraph({
    alignment: AlignmentType.CENTER, spacing: { after: 420 },
    children: [new TextRun({ text: config.subtitle, size: 24, color: P.coverSub, font: { eastAsia: "Microsoft YaHei", ascii: "Arial" } })],
  }));
  children.push(new Paragraph({ spacing: { before: 1100 } }));
  for (const line of config.metaLines) {
    children.push(new Paragraph({
      alignment: AlignmentType.CENTER,
      spacing: { after: 110, line: Math.ceil(18 * 23), lineRule: "atLeast" },
      children: [new TextRun({ text: line, size: 36, color: P.coverMeta, font: { eastAsia: "Microsoft YaHei", ascii: "Arial" } })],
    }));
  }
  children.push(new Paragraph({ spacing: { before: 1900 } }));
  children.push(new Paragraph({
    alignment: AlignmentType.CENTER,
    indent: { left: padL - 400, right: padR - 400 }, spacing: { before: 200 },
    border: { bottom: thickBorder },
    children: [new TextRun({ text: config.footerRight, size: 18, color: P.coverFooter, font: { ascii: "Arial", eastAsia: "Microsoft YaHei" } })],
  }));
  return [new Table({
    width: { size: 100, type: WidthType.PERCENTAGE },
    layout: TableLayoutType.FIXED,
    borders: allNoBorders,
    rows: [new TableRow({
      height: { value: 16838, rule: "exact" },
      children: [new TableCell({
        shading: { type: ShadingType.CLEAR, fill: P.coverBg }, borders: noBorders,
        verticalAlign: VerticalAlign.TOP,
        children,
      })],
    })],
  })];
}

// ───────────────────────── 正文内容 ─────────────────────────
const C = [];

// ============ 1 概述 ============
setCh(1);
C.push(h1("1  概述"));
C.push(h2("1.1  适用范围"));
C.push(body(["本文档定义 ZLR5401 应用层固件（App）与上位机之间经由 USB HID 链路传输的通信协议（App USB Protocol），完整描述帧格式、命令集、子命令、结果码、设备主动推送事件与 CRC32 校验算法，并给出经实机验证的示例帧逐字节解析，供上位机软件开发、固件开发与测试人员作为唯一的接口依据使用。"]));
C.push(body(["协议共分两层：", { m: "系统级命令 0x01~0x09" }, " 负责发现、复位、升级引导与设备信息查询；", { m: "应用级命令 0x20~0x26" }, " 覆盖步进电机、UHF 超高频读写模块、AM 消磁器、开锁器业务编排、RGB 指示灯、设备自检与 IO 诊断七类外设。Bootloader 侧协议另见 Boot 协议文档。"]));
C.push(h2("1.2  协议总体特性"));
C.push(caption("协议关键参数"));
C.push(tbl({
  headers: ["项目", "取值 / 规则"], widths: [30, 70], size: 20,
  rows: [
    ["协议版本", { t: "3", m: true }],
    ["物理传输", "USB HID Interrupt 端点，64B 包，16ms 轮询间隔"],
    ["HID 报告 ID", { t: "0x02（设备发出的 64B 报告首字节，接收时先剥除）", m: true }],
    ["帧同步字", { t: "0x53 0x77（ASCII 字符 S、w）", m: true }],
    ["字节序", "多字节字段一律小端 LE（低字节在前）"],
    ["校验", "CRC32（MPEG-2 变体），4 字节小端尾附，覆盖帧头至数据域末尾"],
    ["响应规则", [{ t: "响应帧 func = 请求帧 func ^ 0xFF", m: true }]],
    ["异常处理", "CRC 不匹配、length 越界或 reserved≠0x00 时设备静默丢弃整帧，不回任何响应"],
  ],
}));
C.push(...callout("note", "响应规则速查：", [
  [{ m: "0x01→0xFE" }, "（握手）、", { m: "0x20→0xDF" }, "（电机）、", { m: "0x21→0xDE" }, "（UHF）、", { m: "0x22→0xDD" }, "（AM）、", { m: "0x23→0xDC" }, "（开锁器/事件推送）、", { m: "0x25→0xDA" }, "（自检）。"],
]));
C.push(h2("1.3  术语与缩略语"));
C.push(caption("术语与缩略语"));
C.push(tbl({
  headers: ["术语", "含义"], widths: [24, 76], size: 19,
  rows: [
    [{ t: "EPC", m: true }, "UHF 标签的电子产品码（Electronic Product Code），Gen2 标签的唯一标识"],
    [{ t: "UHF", m: true }, "超高频 RFID 读写模块（EPC Class 1 Gen2 / ISO 18000-6C）"],
    [{ t: "AM", m: true }, "声磁（Acoustic Magnetic）EAS 消磁器，检测标签并消磁/报警，不读标签唯一 ID"],
    ["硬标签 / 硬标", "可重复使用的 UHF 开锁标签，经 EPC 比对确认后机械开锁"],
    ["软标签 / 软标", "一次性 AM 消磁标签，结账时经消磁器解码失效"],
    ["光电门控", "开锁器上的红外光电开关（PC4），检测客户是否把标签放到开锁器上"],
    ["回零", "上电时升降机构向上、下行程开关寻触，建立绝对位置基准的过程"],
    ["行程开关", { t: "KEY_UP / KEY_DOWN 上/下限位开关，低有效", m: true }],
    [{ t: "W（解锁窗）", m: true }, "解锁流程中放标等待与校对的时间预算窗，自光电触发时刻起算"],
    [{ t: "LE", m: true }, "小端字节序（Little-Endian），低字节在前"],
    [{ t: "POST", m: true }, "上电自检（Power-On Self Test）"],
    [{ t: "DRV", m: true }, "步进电机驱动器件"],
  ],
}));
C.push(h2("1.4  修订历史"));
C.push(caption("修订历史"));
C.push(tbl({
  headers: ["版本", "日期", "说明"], widths: [14, 22, 64], size: 19,
  rows: [
    [{ t: "V3", m: true }, "2026-09-03", "当前版本：单标与多标解锁统一为 0x0A UNLOCK_MULTI 唯一开锁通道；GET_PROGRESS 唯一布局（holdMs 3 字节）；RGB 灯语五色叙事方案"],
  ],
}));

// ============ 2 传输层 ============
setCh(2);
C.push(h1("2  传输层规范（USB HID）"));
C.push(h2("2.1  端点定义"));
C.push(caption("USB 端点"));
C.push(tbl({
  headers: ["端点", "方向", "类型", "包大小", "轮询间隔"], widths: [14, 26, 18, 18, 24], size: 20, centerCols: [0, 2, 3, 4],
  rows: [
    [{ t: "EP1", m: true }, "IN（设备 → 主机）", "Interrupt", "64B", "16 ms"],
    [{ t: "EP2", m: true }, "OUT（主机 → 设备）", "Interrupt", "64B", "16 ms"],
  ],
}));
C.push(h2("2.2  HID 报告 ID 与描述符"));
C.push(body(["设备发送的 64B Interrupt Report 的", { m: "第一个字节" }, "固定为报告 ID ", { m: "0x02" }, "。主机接收时应先剥除该字节，剩余 63B 才是协议层帧。写方向同样以 64B 报告为单位（报告 ID 占首字节）。"]));
C.push(...codeBlock([
  "报告描述符: Usage Page 0x8C (Vendor Defined), Collection Application",
  "              Input + Output + Feature (各 64B 或 2B)",
  "接口类    : HID (0x03)",
  "子类/协议  : 0 / 0",
]));
C.push(...callout("warn", "单包帧大小限制：", [
  ["剥掉报告 ID 后帧体 ≤ 63B，即数据域最多 ", { m: "data_len ≤ 63 - 7 = 56" }, " 字节可单帧承载；更长的业务（如多张 EPC 清单）由协议在应用层拆分（见第 10 章 ADD 逐条追加 / 0x0A 单帧 m≤4 张）。"],
]));

// ============ 3 帧格式 ============
setCh(3);
C.push(h1("3  通用帧格式（所有命令共用）"));
C.push(body(["所有请求、响应与设备主动推送帧共用同一帧结构：", { m: "帧头(2B) + 设备地址(1B) + 保留(1B) + 长度(2B LE) + 命令码(1B) + 数据域(N) + CRC32(4B LE)" }, "。"]));
C.push(caption("帧结构总览"));
C.push(tbl({
  headers: ["帧头1", "帧头2", "设备地址", "保留", "长度", "命令码", "数据域", "CRC32"],
  widths: [11, 11, 12, 10, 12, 12, 16, 16], size: 19, zebra: false, centerCols: [0, 1, 2, 3, 4, 5, 6, 7],
  rows: [
    [{ t: "0x53", m: true }, { t: "0x77", m: true }, { t: "1B", m: true }, { t: "1B", m: true }, { t: "2B LE", m: true }, { t: "1B", m: true }, { t: "N 字节", m: true }, { t: "4B LE", m: true }],
    [{ t: "0", m: true }, { t: "1", m: true }, { t: "2", m: true }, { t: "3", m: true }, { t: "4..5", m: true }, { t: "6", m: true }, { t: "7..7+N-1", m: true }, { t: "7+N..7+N+3", m: true }],
  ],
}));
C.push(h2("3.1  逐字节定义"));
C.push(caption("通用帧逐字节定义"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1，固定 0x53（ASCII 字符 S）"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2，固定 0x77（ASCII 字符 w），二者合成同步字 Sw"],
  ["2", B("xx"), B("devAddr"), "目标设备地址：本机默认 0x01；0xFF 为广播地址"],
  ["3", B("0x00"), B("reserved"), "保留字节，固定 0x00；非 0 时设备静默丢弃整帧且不响应"],
  ["4", B("xx"), B("length L"), "payload 长度低字节（小端）：payload = 命令码(1) + 数据域(N) + CRC32(4)"],
  ["5", B("xx"), B("length H"), "payload 长度高字节；length = 1 + N + 4，合法范围 5..1029，越界静默丢弃"],
  ["6", B("xx"), B("func"), "命令码；响应帧 func = 请求帧 func ^ 0xFF"],
  [{ t: "7..7+N-1", m: true }, B("xx"), B("data[N]"), "数据域：命令参数（请求）或响应数据，字节数 N = length - 5；子命令类命令的 data[0] 为子命令码"],
  [{ t: "7+N..7+N+3", m: true }, B("xx"), B("crc32"), "CRC32 校验（MPEG-2），小端 4 字节尾附；覆盖偏移 0 至 7+N-1（即 CRC 之前的全部 N+7 字节），算法见第 15 章"],
]));
C.push(h2("3.2  字节序与校验约束"));
C.push(bullet(["多字节字段一律小端（LE）：", { m: "length[低,高]" }, "、", { m: "crc32[b0,b1,b2,b3]" }, "、", { m: "holdMs(3)" }, " 等均低字节在前。"]));
C.push(bullet(["CRC 不匹配、length 越界或 reserved≠0x00，设备静默丢弃整帧、不回任何响应——上位机不得将超时一概归因为设备故障，应先自校验帧合法性。"]));

// ============ 4 命令总览 ============
setCh(4);
C.push(h1("4  命令总览"));
C.push(caption("命令总览（协议版本 3）"));
C.push(tbl({
  headers: ["FC", "名称", "主机 → 设备", "设备 → 主机", "用途"], widths: [8, 17, 14, 35, 26], size: 17,
  rows: [
    [B("0x01"), B("FC_HANDSHAKE"), "—", "28B：result/protoVer/status/UID/uidHash/layer/upgradeCount/baudRate", "发现与存活探测"],
    [B("0x02"), B("FC_ENTER_BOOT"), "—", "1B：result", "预升级：置升级标志后自动软复位进 Boot"],
    [B("0x07"), B("FC_DEVICE_INFO"), "—", "34B：result/addr/hwVersion/version(App 层=swVersion)", "查询版本与设备地址"],
    [B("0x08"), B("FC_RESET"), "—", "1B：result", "软件复位（空参数，回 OK 后自动复位）"],
    [B("0x09"), B("FC_EXIT_BOOT"), "—", "1B：result", "退出升级（Boot 处理；App 忽略）"],
    [B("0x20"), B("FC_MOTOR_CTRL"), "data 首字节为子命令码", "同步响应，子命令 0x01~0x09（见第 7 章）", "步进电机升降机构控制与健康监控"],
    [B("0x21"), B("FC_UHF_CTRL"), "data 首字节为子命令码", "同步响应，子命令 0x01~0x0E、0x10（见第 8 章）", "UHF 超高频读写模块控制"],
    [B("0x22"), B("FC_AM_CTRL"), "data 首字节为子命令码", "同步响应，子命令 0x01~0x09（见第 9 章）", "AM 消磁器控制"],
    [B("0x23"), B("FC_LOCKER_CTRL"), "data 首字节为子命令码", "同步响应，子命令 0x01~0x0A（见第 10 章）", "开锁器业务编排，UNLOCK_MULTI 唯一开锁通道"],
    [B("0x24"), B("FC_RGB_CTRL"), "data 首字节为子命令码", "同步响应，子命令 0x01（见第 11 章）", "RGB 三色指示灯手动设色（G/R/B 位掩码）"],
    [B("0x25"), B("FC_SELFTEST_CTRL"), "data 首字节为子命令码", "同步响应，子命令 0x01~0x03（见第 12 章）", "设备级自检锁存错误位（App 专属）"],
    [B("0x26"), B("FC_IO_DIAG"), "—（空参数）", "10B：见第 13 章", "IO/外设状态一帧总览"],
  ],
}));
C.push(body(["Boot 域命令 0x03~0x06 的结果码在 App 端不出现。除上表外，", { m: "0x23" }, " 命令通道还承载五类设备主动推送事件帧（func=0xDC，见 10.5）。"]));

// ============ 5 系统级命令 ============
setCh(5);
C.push(h1("5  系统级命令（0x01~0x09）"));

C.push(h2("5.1  FC_HANDSHAKE（0x01）— 握手 / 存活探测"));
C.push(body(["请求帧数据域为空（N=0，length=5）。响应 data 固定 28 字节，App 与 Boot 格式一致，用于设备发现、协议版本核对与身份识别。"]));
C.push(caption("握手请求帧逐字节注释（总长 11B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1（ASCII S）"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2（ASCII w）"],
  ["2", B("0x01"), B("devAddr"), "目标设备地址 0x01（本机默认）"],
  ["3", B("0x00"), B("reserved"), "保留字节，必须 0x00"],
  ["4", B("0x05"), B("length L"), "payload 长度低字节 = 5（func 1B + data 0B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "payload 长度高字节 = 0，length=5"],
  ["6", B("0x01"), B("func"), "命令码 0x01 = FC_HANDSHAKE"],
  ["7", B("0xC5"), B("crc32[0]"), "CRC32 低字节（小端第 1 字节），覆盖偏移 0..6 共 7 字节"],
  ["8", B("0x69"), B("crc32[1]"), "CRC32 小端第 2 字节"],
  ["9", B("0x2F"), B("crc32[2]"), "CRC32 小端第 3 字节"],
  ["10", B("0x26"), B("crc32[3]"), "CRC32 高字节（小端第 4 字节）"],
]));
C.push(caption("FC_HANDSHAKE 响应 data 字段（28B，App/Boot 一致）"));
C.push(tbl({
  headers: ["偏移", "字段", "类型", "说明"], widths: [10, 22, 14, 54], size: 18, centerCols: [0, 2],
  rows: [
    ["0", B("result"), "u8", "0 = RESULT_OK；其他 = 错误"],
    ["1", B("protoVer"), "u8", "协议版本 = 3"],
    ["2", B("status"), "u8", "设备状态：0=IDLE 空闲 / 1=RUN 运行 / 2=UPG 升级 / 3=FAULT 故障"],
    ["3", B("UID"), "12B", "STM32 唯一 ID（96 位），原样回读"],
    ["15", B("uidHash"), "u32 LE", "CRC32(UID)，设备身份指纹"],
    ["19", B("layer"), "u8", "1 = App 层（Boot 层返回 0）"],
    ["20", B("upgradeCount"), "u32 LE", "累计升级次数"],
    ["24", B("baudRate"), "u32 LE", "UART 当前波特率"],
  ],
}));
C.push(body(["响应帧帧头为 ", { m: "53 77 01 00 21 00 FE" }, "：length=0x21=33（func 1B + data 28B + CRC32 4B），func=0xFE=0x01^0xFF。实机响应的逐字节解析见 16.2。"]));

C.push(h2("5.2  FC_ENTER_BOOT（0x02）— 预升级"));
C.push(body(["请求帧与握手同构，仅 func 字节不同（", { m: "53 77 01 00 05 00 02 + CRC32" }, "）。响应 data 1 字节："]));
C.push(caption("FC_ENTER_BOOT 响应值"));
C.push(tbl({
  headers: ["值", "含义"], widths: [16, 84], size: 20, centerCols: [0],
  rows: [
    [B("0"), "RESULT_OK：已置升级标志，设备将自动软复位进入 Boot 升级模式"],
    [B("2"), "ENTER_BOOT_PARAM_ERR：置升级标志失败"],
  ],
}));
C.push(...callout("note", "预升级语义：", [
  ["设备置升级标志后自动软复位进 Boot（Boot 停留等待升级，不超时）。进 Boot 后不升级时，由主机发 ", { m: "FC_EXIT_BOOT (0x09)" }, " 退出。"],
]));

C.push(h2("5.3  FC_DEVICE_INFO（0x07）— 版本 / 地址查询"));
C.push(body(["请求帧：", { m: "53 77 01 00 05 00 07 + CRC32" }, "（data 为空）。App 层响应 data 34 字节："]));
C.push(caption("FC_DEVICE_INFO 响应 data 字段（34B）"));
C.push(tbl({
  headers: ["偏移", "字段", "长度", "说明"], widths: [10, 24, 12, 54], size: 18, centerCols: [0, 2],
  rows: [
    ["0", B("result"), "1B", "0 = OK"],
    ["1", B("addr"), "1B", "设备地址（默认 0x01）"],
    ["2", B("hwVersion"), "16B", "硬件版本字符串"],
    ["18", B("version"), "16B", "App 层固件版本（swVersion）"],
  ],
}));
C.push(body(["Boot 层响应格式相同，但 ", { m: "version" }, " 字段内容为 bootVersion。"]));

C.push(h2("5.4  FC_RESET（0x08）— 软件复位"));
C.push(body(["请求帧：", { m: "53 77 01 00 05 00 08 + CRC32" }, "（data 必须为空）。响应 data 1 字节："]));
C.push(caption("FC_RESET 响应值"));
C.push(tbl({
  headers: ["值", "含义"], widths: [16, 84], size: 20, centerCols: [0],
  rows: [
    [B("0"), "RESULT_OK：已调度复位，主机收到 OK 后应等待设备重新枚举"],
    [B("1"), "RESET_PARAM_ERR：data 非空（必须为空参数）"],
  ],
}));

C.push(h2("5.5  FC_EXIT_BOOT（0x09）— 退出升级"));
C.push(body(["由 Bootloader 处理，App 端忽略。用于设备停留在 Boot 升级模式时由主机主动唤回 App（清升级标志并复位）。"]));

// ============ 6 错误码 ============
setCh(6);
C.push(h1("6  结果码与错误码总表"));
C.push(body(["各命令响应 data[0]（系统级）或 data[1]（子命令级）为结果码。子命令类命令的通用约定：", { m: "data[0]=cmd 回显" }, "，", { m: "data[1]=err" }, "。"]));
C.push(caption("结果码与错误码总表"));
C.push(tbl({
  headers: ["FC", "码", "宏", "含义"], widths: [10, 10, 32, 48], size: 18, centerCols: [0, 1],
  rows: [
    ["(通用)", B("0"), B("RESULT_OK"), "成功"],
    [B("0x02"), B("2"), B("ENTER_BOOT_PARAM_ERR"), "置升级标志失败"],
    [B("0x08"), B("1"), B("RESET_PARAM_ERR"), "RESET data 非空（必须为空参数）"],
    { group: "FC=0x20 步进电机结果码" },
    [B("0x20"), B("0"), B("MOTOR_ERR_OK"), "成功"],
    [B("0x20"), B("1"), B("MOTOR_ERR_PARAM"), "参数错误"],
    [B("0x20"), B("2"), B("MOTOR_ERR_FAULT"), "器件处于故障状态"],
    [B("0x20"), B("3"), B("MOTOR_ERR_BUSY"), "忙（在途运动/行程测试/后台回零中）"],
    { group: "FC=0x21 UHF 结果码" },
    [B("0x21"), B("0"), B("UHF_ERR_OK"), "成功"],
    [B("0x21"), B("1"), B("UHF_ERR_PARAM"), "参数错误"],
    [B("0x21"), B("2"), B("UHF_ERR_BUSY"), "忙（已有操作进行中）"],
    [B("0x21"), B("3"), B("UHF_ERR_NOT_READY"), "未就绪（未 OPEN / 已下电）"],
    [B("0x21"), B("4"), B("UHF_ERR_LINK"), "链路错误"],
    [B("0x21"), B("5"), B("UHF_ERR_NO_TAG"), "未读到标签"],
    [B("0x21"), B("6"), B("UHF_ERR_TIMEOUT"), "超时"],
    { group: "FC=0x22 AM 结果码" },
    [B("0x22"), B("0"), B("AM_ERR_OK"), "成功"],
    [B("0x22"), B("1"), B("AM_ERR_PARAM"), "参数错误"],
    [B("0x22"), B("4"), B("AM_ERR_LINK"), "链路错误"],
    [B("0x22"), B("6"), B("AM_ERR_TIMEOUT"), "超时"],
    { group: "FC=0x23 开锁器结果码" },
    [B("0x23"), B("0"), B("LOCKER_ERR_OK"), "成功"],
    [B("0x23"), B("1"), B("LOCKER_ERR_BUSY"), "忙（非空闲需先取消）"],
    [B("0x23"), B("2"), B("LOCKER_ERR_PARAM"), "参数非法 / 超上限"],
    [B("0x23"), B("3"), B("LOCKER_ERR_NO_EVENT"), "无待取事件"],
    { group: "FC=0x23 子命令 0x0A（UNLOCK_MULTI）终帧 err 为独立结果码 UNLK_ERR_*，完整定义见 10.6" },
    [B("0x23"), B("7"), B("UNLK_ERR_HOMING"), "未回零 / 行程开关错误，禁止运动"],
    [B("0x23"), B("8"), B("UNLK_ERR_MOTOR_FAULT"), "升降中电机驱动器件故障"],
    [B("0x23"), B("9"), B("UNLK_ERR_MOTOR_TIMEOUT"), "寻触超时 / 超步 / 停滞"],
    [B("0x23"), B("10"), B("UNLK_ERR_AM_LINK"), "softCnt>0：AM 链路断 / 切消磁模式失败"],
    [B("0x23"), B("11"), B("UNLK_ERR_NO_IR"), "光电门控窗内未检测到放标"],
    { group: "FC=0x25 设备自检结果码" },
    [B("0x25"), B("0"), B("SELFTEST_ERR_OK"), "成功"],
    [B("0x25"), B("1"), B("SELFTEST_ERR_PARAM"), "参数错误（长度不足 / 未知子命令）"],
    [B("0x25"), B("2"), B("SELFTEST_ERR_BUSY"), "忙（RERUN 时开锁流程非空闲）"],
  ],
}));

// ============ 7 电机 ============
setCh(7);
C.push(h1("7  步进电机控制 FC_MOTOR_CTRL（0x20）"));
C.push(body(["本命令控制步进电机升降机构。data 区首字节为子命令码 ", { m: "cmd" }, "（0x01~0x09），其后为参数。所有响应同步返回：", { m: "data[0]=cmd" }, "，", { m: "data[1]=err" }, "（0=OK），其余字段随命令。"]));
C.push(h2("7.1  子命令总览"));
C.push(caption("FC_MOTOR_CTRL 子命令"));
C.push(tbl({
  headers: ["cmd", "名称", "主机 → 设备 (data)", "设备 → 主机 (data)", "说明"], widths: [8, 11, 20, 24, 37], size: 17,
  rows: [
    [B("0x01"), "MOVE", B("[cmd,dir,stepsL,stepsM,stepsH]"), B("[cmd,err]"), "启动运动：dir=0(CW)/1(CCW)，steps 为 24bit 微步数（0=持续运行），走满自动停脉冲回 IDLE。MOVE 不受行程自检门控（switchErr 仅作报警诊断）。互斥：在途（MOVE 运行中/行程测试中/后台回零中）再下发一律回 err=3 BUSY，不替换在途运动，改目标先 STOP；复位后约 12s 后台回零窗口内回 BUSY"],
    [B("0x02"), "STOP", B("[cmd]"), B("[cmd,err]"), "停止并关断输出"],
    [B("0x03"), "SPEED", B("[cmd,hzL,hzH]"), B("[cmd,err]"), "设定速度（微步/秒，1~2000）"],
    [B("0x04"), "TORQUE", B("[cmd,pct]"), B("[cmd,err]"), "转矩百分比（6~100）"],
    [B("0x05"), "QUERY", B("[cmd]"), B("[cmd,err,state,fault,diag1,diag2,stepsDoneL,stepsDoneH,switchErr,testState]"), "查询状态/故障/已走微步数/行程开关错误位/行程测试态，字段见 7.2"],
    [B("0x06"), "CLEAR", B("[cmd]"), B("[cmd,err]"), "清除故障并回到 IDLE"],
    [B("0x07"), "TEST", B("[cmd,passes]"), B("[cmd,err]"), "行程测试：正转触碰行程开关→立即反转→触碰为 1 次往返，共 passes 次后自动停转，单程超时 10s。进行中再下发（含 MOVE/TEST）回 MOTOR_ERR_BUSY；回零未成功回 MOTOR_ERR_FAULT；后台回零中回 BUSY。结果经 QUERY 的 testState（2=DONE/3=FAULT）+ diag1 取走，保持至下次 TEST/STOP/CLEAR"],
    [B("0x08"), "HEALTH", B("[cmd]"), B("[cmd,err,olovState,threshL,threshH,trqL,trqH,reason]"), "读健康/堵转监测：高负载状态/阈值/实时转矩计数/最近停机原因，字段见 7.3"],
    [B("0x09"), "STATS", B("[cmd]"), B("[cmd,err,runSeconds(3B),startCount(2B),lastReason]"), "读运行统计：累计运行秒数/启动次数/最近停止原因，字段见 7.3"],
  ],
}));
C.push(h2("7.2  QUERY 响应字段"));
C.push(caption("FC_MOTOR_CTRL QUERY 响应字段"));
C.push(tbl({
  headers: ["字段", "说明"], widths: [18, 82], size: 18,
  rows: [
    [B("err"), "0=OK，其余见第 6 章 0x20 组"],
    [B("state"), "步进电机态：0=IDLE / 1=RUN / 2=FAULT。行程测试进行中（testState=1）时 state 恒为 1(RUN)；测试结束（DONE/FAULT）后 state 回落为步进态（通常 IDLE=0），结果由 testState+diag1 表达，不再占用 state，上位机等 IDLE 不会因 DONE 卡住"],
    [B("fault"), "电机驱动故障寄存器原始值：bit7=FAULT、bit6=SPI_ERROR、bit5=UVLO、bit4=CPUV、bit3=OCP、bit2=STALL、bit1=过温、bit0=开路。state=FAULT 时为停机瞬间快照（DRV 状态寄存器读后自清，固件在 FAULT 态冻结轮询以防证据被抹掉，归因靠此快照）；IDLE/RUN 时为实时轮询值"],
    [B("diag1"), "行程测试故障原因（非测试时为 0）：1=步进故障 / 2=挣脱超时 / 3=方向极性错触 / 4=期望触点超时 / 5=RUN 中电机停转（外部 STOP/被劫持，300ms 宽限后判）"],
    [B("diag2"), "诊断寄存器 2（OTW/OTS/STALL/OL）。state=FAULT 时同为停机瞬间快照"],
    [B("stepsDone"), "本次运动已完成的微步数（16bit，溢出截断；持续运行意会）"],
    [B("switchErr"), "行程开关错误位：bit0=上行程错（缺失/超腿长未触发）、bit1=下行程错。由上电回零/运行中开关监控置位，CLEAR 清 0。置位时 TEST 被禁（冲挡块保护）；MOVE 不受门控（上位机自行决策），但运行中开关监控仍会在超腿长未触发时置位并停机"],
    [B("testState"), "行程测试状态机当前态：0=非测试 / 1=RUN / 2=DONE / 3=测试 FAULT。测试结束后保持到下次 TEST/STOP/CLEAR，供上位机可靠取走结果；state 已回落步进态，二者不再歧义"],
  ],
}));
C.push(h2("7.3  HEALTH / STATS 响应字段"));
C.push(caption("HEALTH (0x08) / STATS (0x09) 响应字段"));
C.push(tbl({
  headers: ["命令", "字段", "说明"], widths: [16, 20, 64], size: 18,
  rows: [
    [{ t: "HEALTH (0x08)", m: true }, B("olovState"), "高负载监测状态：0=正常 / 1=持续高负载（待降速）/ 2=已自动降速 / 3=高负载停机"],
    [{ t: "HEALTH", m: true }, B("thresh"), "高负载 TRQ_COUNT 阈值（16bit，默认 400；越低越接近失速）"],
    [{ t: "HEALTH", m: true }, B("trq"), "最近一次转矩计数采样（12bit，满刻度 4095=无负载，越低负载越高）"],
    [{ t: "HEALTH", m: true }, B("reason"), "最近停机原因：0=无 / 1=正常停止 / 2=运行时限 / 3=高负载停机 / 4=DRV 故障"],
    [{ t: "STATS (0x09)", m: true }, B("runSeconds"), "累计运行秒数（24bit LE，仅 RAM 存储，断电清零）"],
    [{ t: "STATS", m: true }, B("startCount"), "累计启动次数（16bit LE，仅 RAM 存储）"],
    [{ t: "STATS", m: true }, B("lastReason"), "最近停止原因（同 HEALTH.reason）"],
  ],
}));
C.push(h2("7.4  上电行程自检与回零"));
C.push(body(["上电时电机位置未知，开全局中断后无论电机停在何处（含已压行程开关）均执行两阶段自检："]));
C.push(numItem("1.", ["阶段一向上找 ", { m: "KEY_UP" }, "：验证上行程开关在位、可达（静态已触发则免驱动）。"]));
C.push(numItem("2.", ["阶段二向下回归 ", { m: "KEY_DOWN" }, "：验证下行程开关并建立下行程绝对基准。"]));
C.push(body(["两阶段通过即回零完成。任一失败（超腿长未触发/堵转停机/超步/超时）置 ", { m: "switchErr" }, " 对应位并立即返回，此时 TEST 回 ", { m: "MOTOR_ERR_FAULT" }, "（防冲挡块）；MOVE 不受此门控，但运行中开关监控超腿长未触发仍会置位并停机；CLEAR 清错误位。"]));
C.push(...callout("note", "自检参数与蜂鸣判据：", [
  ["自检以保护性参数运行（低转速低转矩 + 每段步数上限 + 30s 超时），堵转由转矩监测兜底。蜂鸣判据在 POST 末尾：", { m: "电机自检与 UHF 通信全部通过才蜂鸣 1s" }, "；任一失败静默，由错误灯语表达（AM 通信/参数 CRC 仅记错误位，不参与蜂鸣判据）。"],
]));
C.push(...callout("note", "运行中开关监控与灯色：", [
  ["正向（奔上）超过上腿限制 LEG_UP_LIMIT 未触上开关 → 置 switchErr.bit0；反向（奔下）超下腿限制未触下开关 → 置 bit1。灯色（500ms 闪烁）：上行程错=黄，下行程错=粉红。"],
]));

// ============ 8 UHF ============
setCh(8);
C.push(h1("8  超高频读写模块控制 FC_UHF_CTRL（0x21）"));
C.push(body(["本命令控制 UHF RFID 读写模块（EPC Class 1 Gen2 / ISO 18000-6C）。data 区首字节为子命令码 ", { m: "cmd" }, "（0x01~0x0E，另 0x10）。所有响应同步返回：", { m: "data[0]=cmd" }, "，", { m: "data[1]=err" }, "（0=OK）。"]));
C.push(h2("8.1  子命令总览"));
C.push(caption("FC_UHF_CTRL 子命令"));
C.push(tbl({
  headers: ["cmd", "名称", "主机 → 设备 (data)", "设备 → 主机 (data)", "说明"], widths: [7, 12, 21, 25, 35], size: 17,
  rows: [
    [B("0x01"), "OPEN", B("[cmd]"), B("[cmd,err]"), "上电 + 下发配置 + 回波检测，进入 READY（天线不良仅告警不阻断）"],
    [B("0x02"), "CLOSE", B("[cmd]"), B("[cmd,err]"), "停止并下电"],
    [B("0x03"), "INVENTORY", B("[cmd,tmoL,tmoH]"), B("[cmd,err,countL,countH,(rssi,epcLen,epc..)..]"), "发起一次盘点：设备内部同步完成盘存与取回，标签内联在响应里带回；tmo 为盘存超时（ms，缺省 1000）"],
    [B("0x04"), "READ_TAG", B("[cmd,epcLen,epc..,bank,addr,cnt]"), B("[cmd,err]"), "读标签：按 EPC 定位，读 bank/addr 起 cnt 个字"],
    [B("0x05"), "WRITE_TAG", B("[cmd,epcLen,epc..,bank,addr,len,data..]"), B("[cmd,err]"), "写标签：按 EPC 定位，写 len 字节到 bank/addr"],
    [B("0x06"), "STOP", B("[cmd]"), B("[cmd,err]"), "停止当前操作"],
    [B("0x07"), "QUERY", B("[cmd]"), B("[cmd,err,state,link,totalTags]"), "查询状态/链路/累计标签数。未上电（CLOSE 后）直接回 err=3 NOT_READY + 当前 state，不再做必然超时的链路探测，与链路坏（err=4 LINK）语义区分"],
    [B("0x08"), "GET_CONFIG", B("[cmd]"), B("[cmd,err,power,ant,checksum,session,target,q,band]"), "读取当前配置（字段见 8.3）"],
    [B("0x09"), "SET_CONFIG", B("[cmd,power,ant,checksum,session,target,q,(band)]"), B("[cmd,err]"), "设置配置（参数合法即生效；模块忙时配置仍立即生效于设备侧，下轮盘点前/下次 OPEN 时下发给模块）"],
    [B("0x0A"), "GET_TAGS", B("[cmd,count]"), B("[cmd,err,totalL,totalH,(epcLen,rssi,epc..)..]"), "取标签缓冲：count=0 取全部，>0 取前 count 条"],
    [B("0x0B"), "GET_STATUS", B("[cmd]"), B("[cmd,err,state,link,totalTags,powered,antennaOk,lastErr,antRlH,antRlL,antVswrH,antVswrL]"), "读取状态/错误监控（含回波结果），字段见 8.4"],
    [B("0x0C"), "CHECK_ANT", B("[cmd]"), B("[cmd,err,antennaOk,antRlH,antRlL,antVswrH,antVswrL]"), "主动触发一次回波检测"],
    [B("0x0D"), "SCAN_START", B("[cmd,cycleL,cycleH]"), B("[cmd,err]"), "启动自动扫描：连续盘点持续入缓冲（不主动上报），主机用 GET_TAGS 拉取；cycle 为每轮盘存超时（ms，缺省 1000）"],
    [B("0x0E"), "SCAN_STOP", B("[cmd]"), B("[cmd,err]"), "停止自动扫描并停止当前盘点轮次"],
    [B("0x10"), "GET_DUMP", B("[cmd]"), B("[cmd,err,cnt,reqCmd,reqLen,reqData..,rawLenL,rawLenH,rspCmd,rspStatusL,rspStatusH,rspLenL,rspLenH,rspData..]"), "读取最近一次盘存的原始请求/响应字节（诊断用）"],
  ],
}));
C.push(h2("8.2  INVENTORY 同步语义与错误码"));
C.push(body([{ m: "INVENTORY" }, " 为同步阻塞：设备在单请求内完成多标签盘存（超时 tmo，缺省 1000ms）→ 取回缓冲 → 解码 → 把全部标签内联在响应里带回，无主动上报。err 取值："]));
C.push(caption("INVENTORY 错误码"));
C.push(tbl({
  headers: ["err", "含义"], widths: [26, 74], size: 19, centerCols: [0],
  rows: [
    [B("0 OK"), "紧随其后 count L,H + 每条 [rssi, epcLen, epc..]（rssi 0~255，epcLen 6~16）"],
    [B("1 PARAM"), "参数非法"],
    [B("2 BUSY"), "设备忙（其它操作进行中），需稍后重试"],
    [B("3 NOT_READY"), "未 OPEN / 已下电"],
    [B("4 LINK"), "UHF 模块链路异常"],
    [B("5 NO_TAG"), "场内无标签"],
    [B("6 TIMEOUT"), "盘存超时"],
  ],
}));
C.push(h2("8.3  配置字段（SET_CONFIG / GET_CONFIG）"));
C.push(caption("UHF 配置字段"));
C.push(tbl({
  headers: ["字段", "范围", "说明"], widths: [18, 18, 64], size: 19, centerCols: [1],
  rows: [
    [B("power"), "5~30", "发射功率（dBm）"],
    [B("antenna"), "0~1", "天线选择：0=ANT1 / 1=ANT2"],
    [B("checksumEn"), "0~1", "1=启用模块帧 CRC16 校验 / 0=关闭"],
    [B("session"), "0~3", "Gen2 session S0~S3"],
    [B("target"), "0~1", "Gen2 target：0=A / 1=B"],
    [B("q"), "0~15", "Gen2 Q 值：0=动态 Q（模块默认），1~15=静态 Q"],
    [B("band"), "0x01/06/08/FF", "工作频段 Region 码：0x01=北美（默认）/ 0x06=中国 1 / 0x08=CE_LOW / 0xFF=全频段"],
  ],
}));
C.push(...callout("tip", "开机自动恢复：", [
  ["复位后设备在上电阶段即自动完成 UHF 上电与配置下发——上位机发首条命令时 UHF 已配置完毕、state=READY，", { m: "无需先发 OPEN/SET_CONFIG" }, "；GET_CONFIG 直接读到生效值。"],
]));
C.push(h2("8.4  QUERY / GET_STATUS / CHECK_ANT 字段"));
C.push(caption("UHF 状态字段"));
C.push(tbl({
  headers: ["字段", "说明"], widths: [20, 80], size: 19,
  rows: [
    [B("state"), "0=IDLE / 1=READY / 2=INVENTORY / 3=READ / 4=WRITE / 5=ERROR / 6=ANT_CHECK / 7=GETBUF（盘存后取缓冲）"],
    [B("link"), "链路状态：0=正常 / 1=超时 / 2=CRC 错误"],
    [B("totalTags"), "累计盘点到标签总数（8bit 截断）"],
    [B("powered"), "1=已上电"],
    [B("antennaOk"), "1=天线连接正常（RL≥0.5dB 且 VSWR≤7.00）"],
    [B("lastErr"), "最近一次操作错误（0=OK）"],
    [B("antRl"), "回波反射损耗 RL（0.1dB 单位），0=未检测"],
    [B("antVswr"), "回波驻波比 VSWR（×100，如 1.67→167），0=未检测"],
  ],
}));
C.push(...callout("note", "天线告警语义：", [
  ["OPEN 时若检测到未接天线（天线不良）仅上报告警，模块仍可用（进入 READY 不阻断）；antRl/antVswr 保留上次检测值，可用 CHECK_ANT 随时复测。"],
]));
C.push(...callout("note", "错误码（data[1]）：", [
  [{ m: "0=OK, 1=参数错误, 2=忙（已有操作进行中）, 3=未就绪, 4=链路错误, 5=未读到标签, 6=超时" }, "。"],
]));

// ============ 9 AM ============
setCh(9);
C.push(h1("9  消磁器控制 FC_AM_CTRL（0x22）"));
C.push(body(["本命令控制 AM EAS 消磁器（Deactivator）：检测标签 → 消磁/报警，不读标签唯一 ID。data 区首字节为子命令码 ", { m: "cmd" }, "（0x01~0x09）。注意：", { m: "16 位参数值高字节在后" }, "，即组装时 ", { m: "(H<<8)|L" }, "。"]));
C.push(h2("9.1  子命令总览"));
C.push(caption("FC_AM_CTRL 子命令"));
C.push(tbl({
  headers: ["cmd", "名称", "主机 → 设备 (data)", "设备 → 主机 (data)", "说明"], widths: [7, 14, 30, 27, 22], size: 17,
  rows: [
    [B("0x01"), "GET_CONFIG", B("[cmd]"), B("[cmd,err,thrH,thrL,hitH,hitL,freq,delayH,delayL,len,invert,syncH,syncL,volt,mode,mains]"), "读取当前配置（字段见 9.2）"],
    [B("0x02"), "SET_CONFIG", B("[cmd,thrH,thrL,hitH,hitL,freq,delayH,delayL,len,invert,syncH,syncL,volt,mode,(mains)]"), B("[cmd,err]"), "设置配置"],
    [B("0x03"), "GET_PARAM", B("[cmd,amCmd]"), B("[cmd,err,amCmd,valH,valL]"), "读单个参数"],
    [B("0x04"), "SET_PARAM", B("[cmd,amCmd,valH,valL]"), B("[cmd,err]"), "写单个参数"],
    [B("0x05"), "QUERY", B("[cmd]"), B("[cmd,err,link]"), "探测链路（link=0 正常）"],
    [B("0x06"), "GET_STATUS", B("[cmd]"), B("[cmd,link,evt(4B),last(4B),deact]"), "监控：链路 + 消磁事件累计（4B）+ 最近事件相对上电毫秒（4B）+ 消磁结果分类（0=空闲/1=成功/2=失败）"],
    [B("0x07"), "SET_MODE", B("[cmd,mode]"), B("[cmd,err]"), "仅切换工作模式"],
    [B("0x08"), "GET_WAVE", B("[cmd]"), B("[cmd,err,pointsH,pointsL]"), "触发一次同步波形采集（阻塞约 1s），points=有效点数（400 或部分）"],
    [B("0x09"), "GET_WAVE_PAGE", B("[cmd,page]"), B("[cmd,page,err,pointsH,pointsL,w(≤48)]"), "取一页波形（48 点/页）：400 点共 9 页（末页 16 点）；points 为该次采集总有效点数，以响应为准"],
  ],
}));
C.push(h2("9.2  配置字段（SET_CONFIG / GET_CONFIG）"));
C.push(caption("AM 配置字段"));
C.push(tbl({
  headers: ["字段", "范围", "说明"], widths: [18, 16, 66], size: 19, centerCols: [1],
  rows: [
    [B("thr"), "0~30", "接收阈值（16bit）"],
    [B("hit"), "3~8", "命中次数（16bit）"],
    [B("freq"), "0~2", "频率范围：0=宽 / 1=中 / 2=窄"],
    [B("delay"), "0~50", "接收延迟（16bit，掩藏参数）"],
    [B("len"), "0~1", "接收长短：0=长 / 1=短"],
    [B("invert"), "0~1", "零火翻转：0=否 / 1=是"],
    [B("sync"), "0~2000", "相位同步（16bit）"],
    [B("volt"), "0~2", "解码电压：0=低 / 1=中 / 2=高"],
    [B("mode"), "0~2", "工作模式：0=检测/消磁 / 1=仅检测 / 2=待机（SET_MODE 亦可用）"],
    [B("mains"), "0~1", "市电频率：0=50Hz / 1=60Hz（SET_CONFIG 可省略，省略保持当前值）"],
  ],
}));
C.push(...callout("note", "错误码（data[1]）与特例：", [
  [{ m: "0=OK, 1=参数错误, 4=链路错误, 6=超时" }, "。", { m: "GET_STATUS 无 err 字段" }, "：cmd 后首字节即 link。"],
]));

// ============ 10 LOCKER ============
setCh(10);
C.push(h1("10  开锁器业务编排 FC_LOCKER_CTRL（0x23）"));
C.push(body(["本命令把开锁器（硬标签 UHF 开锁 + 软标计数）业务编排状态机暴露给上位机。data 区首字节为子命令码 ", { m: "cmd" }, "（0x01~0x0A；0x0B~0x0F 为设备主动推送事件码，见 10.5）。所有响应同步返回：", { m: "data[0]=cmd" }, "，", { m: "data[1]=err" }, "（0=OK）。"]));
C.push(body(["一帧装不下的多枚硬标签可先 ADD 逐条追加再 START；", { m: "解锁任务（单标/多标）一律用 0x0A UNLOCK_MULTI 单帧下发" }, "（≤4 张 EPC，单标即 epcCnt=1），这是唯一开锁通道。"]));
C.push(h2("10.1  子命令总览"));
C.push(caption("FC_LOCKER_CTRL 子命令"));
C.push(tbl({
  headers: ["cmd", "名称", "主机 → 设备 (data)", "设备 → 主机 (data)", "说明"], widths: [7, 13, 24, 26, 30], size: 17,
  rows: [
    [B("0x01"), "CONFIGURE", B("[cmd,hardCntL,hardCntH,softCntL,softCntH]"), B("[cmd,err]"), "清空清单、置软标数 N（hardCnt 仅用于预检上限，不写入；softCnt=0 为合法的纯硬标任务，硬标经 ADD 累积）"],
    [B("0x02"), "ADD", B("[cmd,epcLen,epc..]"), B("[cmd,err]"), "追加一条硬标签 EPC（≤12B）"],
    [B("0x03"), "START", B("[cmd]"), B("[cmd,err]"), "上电 UHF + 开扫，进入可开锁"],
    [B("0x04"), "CANCEL", B("[cmd]"), B("[cmd,err]"), "取消当前任务，磁块回降、回 IDLE；0x0A 流程中则打断请求（立即回 OK，流程安全回降后回终帧）"],
    [B("0x05"), "QUERY", B("[cmd]"), B("[cmd,err,state,hmL,hmH,scL,scH,suL,suH,faultReason]"), "查询状态/计数，字段见 10.2"],
    [B("0x06"), "CONSUME_SOFT", B("[cmd]"), B("[cmd,err]"), "软标：上位机上报已解码一次"],
    [B("0x07"), "GET_EVENT", B("[cmd]"), B("[cmd,err,code,epcLen,epc..,hmL,hmH,suL,suH,scL,scH,hcL,hcH]"), "取一条上报事件"],
    [B("0x09"), "GET_PROGRESS", B("[cmd]"), B("[cmd,err,phase,holdMs(3B),total,confirmed,bitmap,softCnt,softDone]"), "解锁流程进度快照（唯一布局，非流程时 phase=0 全零）。holdMs 3 字节（W≤240000）；流程中仅 CANCEL/QUERY/GET_PROGRESS 放行，其余回 BUSY"],
    [B("0x0A"), "UNLOCK_MULTI", B("[cmd,tmoL,tmoH,holdL,holdH,softCnt,epcCnt,epcLen,EPC 字节流]"), "见 10.6（随 err 变体）", "同步解锁（唯一开锁通道，单标/多标同一流程）：单帧下发 m≤4 张期望 EPC + 软标数，阻塞自治执行，期间推 0x0B~0x0F 事件帧，流程结束回 0x0A 终帧"],
  ],
}));
C.push(h2("10.2  QUERY 字段与状态机"));
C.push(caption("QUERY 响应字段"));
C.push(tbl({
  headers: ["字段", "说明"], widths: [20, 80], size: 18,
  rows: [
    [B("state"), "0=IDLE 空闲 / 1=CONFIGURED 已配清单等放硬标 / 2=UNLOCK_HOLD 硬标匹配磁块升起 / 3=SOFT_DECODE 硬标全解进软标 / 4=DONE 完成 / 5=FAULT 故障 / 6=LOWERING 回降中"],
    [B("hm"), "已解锁硬标数 n"],
    [B("sc"), "软标总数 N"],
    [B("su"), "已消耗软标数"],
    [B("faultReason"), "故障归因：1=UHF 链路 / 2=未回零 / 3=寻触启动失败 / 4·5·6=升寻触失败 / 7=回降寻触失败 / 8=回降启动失败"],
  ],
}));
C.push(caption("GET_EVENT 事件码"));
C.push(tbl({
  headers: ["code", "事件"], widths: [18, 82], size: 19, centerCols: [0],
  rows: [
    [B("0"), "MATCH_OK：硬标匹配（亮绿升锁）"],
    [B("1"), "MISMATCH：非清单 EPC（红灯闪不升）"],
    [B("2"), "HARD_DONE：硬标全解进软标"],
    [B("3"), "SOFT_USED：消耗一次软标"],
    [B("4"), "TIMEOUT：开锁保持超时"],
    [B("5"), "DONE：结账完成"],
    [B("6"), "FAULT：故障"],
  ],
}));
C.push(body(["无待取事件时回 ", { m: "err=3 (NO_EVENT)" }, "。错误码：", { m: "0=OK, 1=忙（非空闲需先取消）, 2=参数非法/超上限, 3=无待取事件" }, "。"]));

C.push(h2("10.3  UNLOCK_MULTI（0x0A）同步解锁流程"));
C.push(h3("10.3.1  业务闭环（上位机视角，单标/多标同一流程）"));
C.push(numItem("1.", ["客户付款。"]));
C.push(numItem("2.", ["上位机把结账清单（m 张硬标 EPC + 软标数 softCnt）单帧下发给开锁器。"]));
C.push(numItem("3.", ["客户把标签放到开锁器上，光电（PC4）触发。"]));
C.push(numItem("4.", ["持续比对：清单内 EPC 稳定确认 → 逐张推 0x0B 回传；清单外 EPC 稳定确认 → 红闪并逐张推 0x0C 回传，不终止流程。"]));
C.push(numItem("5.", [{ m: "全确认门控" }, "：所有 EPC 均校验通过（n==m）才绿灯升起开锁（W 是预留上限，非最短等待）；任一未确认（窗满 PARTIAL）磁块全程不动（不升不消磁，不进软标段）。全部确认磁块升起时，有软标即同时开启 AM 消磁（软标可与硬标校对并行解码）。epcCnt=0 且 softCnt>0 为纯软标结账：跳过光电门控与 EPC 校验，受理即升起+消磁直接进入软标段（窗即软标窗 5min）。"]));
C.push(numItem("6.", ["软标段：每个软标对应一次解码（AM），每次推 0x0E；次数用完即结账完成；软标窗 5min 满未校验完成按超时失败结账（endReason=7）。整个流程结束磁块才回降 + AM 关消磁，回 0x0A 终帧。"]));
C.push(h3("10.3.2  请求参数逐字节定义"));
C.push(caption("UNLOCK_MULTI 请求 data 逐字节定义"));
C.push(tbl({
  headers: ["data 偏移", "字段", "详细含义"], widths: [14, 20, 66], size: 18, centerCols: [0],
  rows: [
    ["data[0]", B("cmd"), "固定 0x0A = UNLOCK_MULTI"],
    ["data[1..2]", B("tmoMs LE"), "每轮盘点时限（ms），0 → 取缺省 500，上限 10000"],
    ["data[3..4]", B("holdMs LE"), "解锁窗 W 上报值：0 → 由公式计算；非零取 min(上位机值, 240000)"],
    ["data[5]", B("softCnt"), "软标消磁数（0 = 跳过软标段）"],
    ["data[6]", B("epcCnt"), "期望硬标签数 m：0~4；1 即单标流程；0 且 softCnt>0 = 纯软标结账（跳过光电门控与 EPC 校验，受理即计时直通软标段，此时 holdMs 忽略、窗即软标窗 5min）；0 且 softCnt=0 → err=2 PARAM"],
    ["data[7]", B("epcLen"), "每张 EPC 字节数，1~12（epcCnt=0 时忽略）"],
    ["data[8..]", B("EPC 字节流"), "epcCnt × epcLen 字节，按清单顺序紧密排列"],
  ],
}));
C.push(body(["解锁窗 W 公式：", { m: "holdMs=0 → W = 120000 + (m-1)*30000 ms" }, "（首标签 2 分钟，每多一标签加 30 秒），绝对上限 240000ms；非零时取 min(上位机值, 上限)。W 自光电触发时刻起算，兼任光电等待窗与校对预算。epcCnt=0 纯软标：holdMs 忽略，窗即软标窗 300000ms，自受理时刻起算。"]));
C.push(h3("10.3.3  设备内部流程"));
C.push(caption("UNLOCK_MULTI 设备内部流程（含全部出口）"));
C.push(tbl({
  headers: ["阶段", "处理内容", "出口 / 终帧 err"], widths: [16, 50, 34], size: 17,
  rows: [
    ["(1) 受理", "前置检查：Locker/UHF/电机空闲 + 已回零 +（有软标时）AM 链路；通过后推 0x0F 受理帧（含实际 W；纯软标 phase=5 SOFT，受理即计时）", "失败 → 终帧 err=1 BUSY / 3 UHF_OPEN / 7 HOMING / 10 AM_LINK（见 10.6）"],
    ["(2) WAIT_TAG", "光电门控等放标（PC4，200ms 去抖，窗=W）；纯软标跳过本段及 (4)(5)，直接升起+软解码", "窗满未放标 → 终帧 err=11 NO_IR（磁块未动）"],
    ["(3) 消磁准备", "softCnt>0：AM 探链 + 强制检测模式——校验期仅检测不消磁；=0 跳过", "AM 链路失败 → 终帧 err=10 AM_LINK"],
    ["(4) 盘点上电", "UHF 上电 / 配置 + 连续盘点", "失败 → 终帧 err=3 UHF_OPEN / 4 UHF_LINK"],
    ["(5) VERIFY 校对", "每标签独立状态机 PENDING→TRACKING→CONFIRMED；确认判据双门限：在场连续 1.5s 且累计读到 ≥2 次（单轮约 1/4 漏读，禁连续 N 轮式判据）；离场超 3s 回 PENDING 重累计；CONFIRMED 单向掩码（后续读到静默忽略）。每确认一张推 0x0B + 蜂鸣 200ms + RGB 绿单闪；外来 EPC 独立缓存（≤4 张，判据同期望标签）稳定确认一张推 0x0C + 红闪 3s（单向掩码不重报），不终止，窗 W 兜底", "n==m → ALL_OK；窗 W 满 → PARTIAL；UHF 失联 5s → UHF_LOST（均推 0x0D 后进入收尾）"],
    ["(6) 升起", "n==m 全确认才升 KEY_UP（保持至整个流程结束）；softCnt>0 同时切 AM 消磁模式（升起与消磁同时；软标可与硬标校对并行解码）；PARTIAL/UHF_LOST 未动过磁块（不升不降不消磁，不进软标段）；推 0x0D 硬标完成帧", "—"],
    ["(7) SOFT 软标", "softCnt>0 且 ALL_OK：升起时已开消磁（失败入口再试一次，再败 err=10 AM_LINK 结账）；消磁计数每 +1 推 0x0E；达标即收；5min 软标窗满未校验完成 → 按超时失败结账（endReason=7）", "AM 再败 → 终帧 err=10 AM_LINK"],
    ["(8) LOWER 回降", "整个流程结束统一回降 KEY_DOWN（免疫打断）；结账完成时长鸣 300ms + 绿三连闪；AM 切回仅检测模式", "电机故障 → 终帧 err=8 MOTOR_FAULT / 9 MOTOR_TIMEOUT（诊断见 10.6）"],
    ["(9) DONE", "回 0x0A 终帧（随 err 变体，见 10.6）", "—"],
    ["任意阶段", "CANCEL → 立即回 OK → 安全回降 → 终帧 endReason=6 ABORTED（CANCEL 后回降完成前仍处解锁态）", "—"],
  ],
}));
C.push(...callout("note", "解锁态响应矩阵：", [
  ["受理帧（0x0F）起至终帧发完，仅 ", { m: "CANCEL / QUERY / GET_PROGRESS" }, "（及各 FC 只读命令）被放行，其余 LOCKER 子命令与 MOTOR/UHF/AM 控制类一律回 BUSY。"],
]));
C.push(h2("10.4  GET_PROGRESS（0x09）进度布局"));
C.push(caption("GET_PROGRESS 响应 data 逐字节定义（唯一布局）"));
C.push(tbl({
  headers: ["data 偏移", "字段", "详细含义"], widths: [14, 20, 66], size: 18, centerCols: [0],
  rows: [
    ["data[0]", B("cmd"), "0x09 回显"],
    ["data[1]", B("err"), "0=OK"],
    ["data[2]", B("phase"), "1=WAIT_TAG 光电门控 / 2=VERIFY 持续校对 / 3=RISE 升起 / 4=LOWER 回降 / 5=SOFT 软标解码 / 6=DONE 终帧组包（瞬时）；非流程时 phase=0 且全字段为 0"],
    ["data[3..5]", B("holdMs LE"), "3 字节（W≤240000，超 16bit 回填范围）：自光电触发已过 ms（未触发=0；纯软标=自受理时刻）"],
    ["data[6]", B("total"), "清单 m（期望标签总数）"],
    ["data[7]", B("confirmed"), "已确认 n"],
    ["data[8]", B("bitmap"), "确认位图（bit i = 清单第 i 张已确认）"],
    ["data[9]", B("softCnt"), "软标目标数"],
    ["data[10]", B("softDone"), "软标已解码数"],
  ],
}));
C.push(h2("10.5  设备主动推送事件帧"));
C.push(body(["推送事件帧经命令来向通道原路回传，", { m: "func = 0x23 ^ 0xFF = 0xDC" }, "，", { m: "data[0]" }, " 为事件码。事件帧不是对某请求的响应，主机接收线程应按 data[0] 分发。"]));
C.push(caption("UNLOCK_MULTI 流程推送事件帧"));
C.push(tbl({
  headers: ["码", "名称", "Device → Host (data)", "说明"], widths: [7, 13, 32, 48], size: 17,
  rows: [
    [B("0x0F"), "EVT_START 受理", B("[0x0F, 0, phase, winMs(3B LE)]"), "命令已受理，含实际生效的解锁窗 W（区别于终帧的流程开始标志）。phase：1=WAIT_TAG；纯软标=5（直通软标段），winMs=软标窗 300000"],
    [B("0x0B"), "EVT_TAG 确认", B("[0x0B, seq, epcLen, epc.., confirmed, total, 判据耗时(2B LE), 流程耗时(2B LE)]"), "一张期望 EPC 稳定确认，比对结果回传上位机（每确认一张一帧）。seq=确认序号 1..m；confirmed/total=已确认 n / 总数 m；判据耗时=自该标签首次读到（进 TRACKING）至确认；流程耗时=自光电触发至本帧"],
    [B("0x0C"), "EVT_MISMATCH 失配", B("[0x0C, epcLen, epc.., hits]"), "外来标签稳定确认（判据同期望标签：在场连续 1.5s 且累计读到 ≥2 次），比对结果回传，一张一帧（独立缓存 ≤4 张，单向掩码不重报）。epc=该外来 EPC；hits=稳定确认期间累计读到次数。推帧不终止流程"],
    [B("0x0D"), "EVT_HARD_DONE 硬标完成", B("[0x0D, endReason, bitmap, confirmed, total, elapsed(2B LE)]"), "硬标段结束：n==m 升起的磁块保持至整个流程结束才回降；PARTIAL/UHF_LOST 未动过磁块。endReason 见 10.6；bitmap=确认位图（bit i=清单第 i 张）；elapsed=自光电触发起 ms（纯软标：自受理时刻，total/confirmed=0）"],
    [B("0x0E"), "EVT_SOFT 软标解码", B("[0x0E, done, softCnt]"), "软标每解码一次推一帧：done=累计已解码数（1..softCnt），softCnt=目标数"],
  ],
}));
C.push(h2("10.6  UNLOCK_MULTI 终帧格式（随 err 变体）"));
C.push(caption("终帧 data 布局（按 err 变体）"));
C.push(tbl({
  headers: ["err", "Device → Host (data)", "说明"], widths: [16, 42, 42], size: 17,
  rows: [
    [B("0 OK"), B("[cmd,0,endReason,bitmap,confirmed,total,riseL,riseH,lowerL,lowerH,softDone,softCnt,elapsedL,elapsedH]"), "流程结束：endReason/bitmap/confirmed/total 见下表；rise/lower=升/降实际微步数（全确认门控：未全部确认时恒 0）；softDone=软标已解码数（softCnt=0 恒 0）；elapsed=自光电触发至终帧 ms（纯软标：自受理时刻）"],
    [B("1 BUSY"), B("[cmd,1,lockerState,uhfState,stepperState]"), "三模块占用状态快照"],
    [B("2 PARAM"), B("[cmd,2]"), "帧长/epcCnt/epcLen 非法（含 epcCnt=0 且 softCnt=0）"],
    [B("3 UHF_OPEN"), B("[cmd,3,uhfRawErr]"), "UHF 上电/配置失败"],
    [B("4 UHF_LINK"), B("[cmd,4,uhfRawErr]"), "UHF 通信失败"],
    [B("7 HOMING"), B("[cmd,7,switchErr]"), "未回零/行程开关错误，禁止运动（bit0=上 bit1=下）"],
    [B("8 MOTOR_FAULT"), B("[cmd,8,fault,diag1,diag2,steps(3B),phase,retreat]"), "电机驱动器件故障：phase 1=升段 / 2=降段；retreat 0=已回退下端 / 1=回退失败 / 2=未尝试（失败发生在运动前）"],
    [B("9 MOTOR_TIMEOUT"), B("[cmd,9,steps(3B),phase,retreat]"), "寻触超时 / 超步 / 停滞"],
    [B("10 AM_LINK"), B("[cmd,10]"), "softCnt>0：AM 链路断 / 升起时与软标入口两次切消磁模式均失败"],
    [B("11 NO_IR"), B("[cmd,11]"), "光电门控窗内未检测到放标（客户未放标 / 光电传感器故障）"],
  ],
}));
C.push(caption("endReason 定义（err=0 时的流程结束原因）"));
C.push(tbl({
  headers: ["值", "宏", "含义"], widths: [10, 28, 62], size: 18, centerCols: [0],
  rows: [
    [B("1"), B("ALL_OK"), "全部 m 张确认（n==m，随后升起+消磁）"],
    [B("2"), B("PARTIAL_TIMEOUT"), "硬标窗 W 满部分确认（bitmap 明示哪些已确认；全确认门控：磁块全程未动、未消磁、不进软标段）"],
    [B("4"), B("UHF_LOST"), "链路失联确认窗（5s）仍坏（磁块未动）"],
    [B("6"), B("ABORTED"), "上位机 CANCEL 打断（安全回降后以 ABORTED 结账，跳过软标段）"],
    [B("7"), B("SOFT_TIMEOUT"), "软标窗 5min 满未校验完成（按超时失败结账，softDone 明示完成数）"],
  ],
}));
C.push(h2("10.7  命令交互序列（泳道）"));
C.push(caption("解锁命令交互序列（主机/设备双泳道）"));
C.push(tbl({
  headers: ["序", "方向", "帧 / 事件", "说明"], widths: [7, 13, 32, 48], size: 17, centerCols: [0, 1],
  rows: [
    ["1", "主机 →", "UNLOCK_MULTI [tmo, hold, softCnt, m 张 EPC]", "单帧下发结账清单"],
    ["2", "← 设备", "推帧 0x0F [phase=1 WAIT_TAG, W(3B)]", "受理（纯软标：[phase=5 SOFT]，其后直接进软标段）"],
    ["3", "设备内", "光电门控 — 客户放标触发（窗=W）", "—"],
    ["4", "设备内", "持续校对（UHF 连续盘点）", "—"],
    ["5", "← 设备", "推帧 0x0B 确认 ×n", "每张一帧 + 蜂鸣 200ms"],
    ["6", "← 设备", "推帧 0x0C 失配（按需）", "外来标签稳定确认，不终止"],
    ["7", "设备内", "全部确认 n==m → 磁块升起（保持至流程结束；softCnt>0 同时开 AM 消磁）", "任一未确认则不升"],
    ["8", "← 设备", "推帧 0x0D 硬标完成 [endReason, bitmap, n/m]", "—"],
    ["9", "← 设备", "推帧 0x0E 软标解码 ×softCnt [done, softCnt]（有软标时）", "—"],
    ["10", "设备内", "整个流程结束：回降 KEY_DOWN + AM 关消磁", "—"],
    ["11", "← 设备", "终帧 0x0A（流程结束标志，随 err 变体）", "结账完成：蜂鸣 300ms + 绿三连闪"],
  ],
}));

// ============ 11 RGB ============
setCh(11);
C.push(h1("11  RGB 指示灯控制 FC_RGB_CTRL（0x24）"));
C.push(body(["控制板载 RGB 三色指示灯（绿/红/蓝三通道）。data 区首字节为子命令码 ", { m: "cmd" }, "（0x01）。响应同步返回：", { m: "data[0]=cmd" }, "，", { m: "data[1]=err" }, "，并回显 ", { m: "mask" }, "。"]));
C.push(caption("FC_RGB_CTRL 子命令"));
C.push(tbl({
  headers: ["cmd", "名称", "主机 → 设备 (data)", "设备 → 主机 (data)", "说明"], widths: [8, 10, 22, 18, 42], size: 17,
  rows: [
    [B("0x01"), "SET", B("[cmd, mask, reserved]"), B("[cmd, err, mask]"), "手动设色：mask bit0=G 绿 / bit1=R 红 / bit2=B 蓝，其余位与 reserved 预留（忽略）。仅设备空闲（Locker/Unlock/行程测试均未运行）时接受，作为手动灯语保持 10s 自动回收（业务灯语优先，到期自动灭）；mask=0 立即撤销。运行中回 err=2 BUSY"],
  ],
}));
C.push(caption("颜色掩码示例"));
C.push(tbl({
  headers: ["mask", "颜色", "mask", "颜色"], widths: [20, 30, 20, 30], size: 19, centerCols: [0, 2],
  rows: [
    [B("0x00"), "全灭", B("0x04"), "蓝"],
    [B("0x01"), "绿", B("0x03"), "黄（G+R）"],
    [B("0x02"), "红", B("0x05"), "青（G+B）"],
    [B("0x06"), "紫（R+B）", B("0x07"), "白（G+R+B）"],
  ],
}));
C.push(...callout("note", "错误码（data[1]）：", [
  [{ m: "0=OK, 1=参数非法/长度不足, 2=BUSY" }, "（结账/开锁/行程测试流程运行中，拒绝手动设色）。"],
]));

// ============ 12 SELFTEST ============
setCh(12);
C.push(h1("12  设备级自检 FC_SELFTEST_CTRL（0x25）"));
C.push(body(["设备级健康监控：上电 POST 逐个探测外设（步进电机 / UHF / AM 消磁器），结果沉淀为 16bit 锁存错误位图 ", { m: "errBits" }, "；运行期故障（开锁器 UHF 链路故障、未回零）与参数区 CRC 失败亦联动置位。data 区首字节为子命令码，响应 ", { m: "data[0]=cmd" }, "，", { m: "data[1]=err" }, "。本命令为 App 专属（Bootloader 忽略）。"]));
C.push(h2("12.1  锁存错误位定义（errBits，16bit LE）"));
C.push(caption("errBits 位定义"));
C.push(tbl({
  headers: ["bit", "宏", "含义", "置位来源"], widths: [8, 28, 38, 26], size: 17, centerCols: [0],
  rows: [
    ["bit0", B("SELF_ERR_MOTOR_SPI"), "电机驱动配置回读不一致（通信/落配置失败）", "POST / RERUN 探测"],
    ["bit1", B("SELF_ERR_MOTOR_FAULT"), "电机驱动器件故障（FAULT 位）", "POST / RERUN 探测"],
    ["bit2", B("SELF_ERR_UHF_COMM"), "UHF Open/Query 通信失败", "POST / RERUN；运行期开锁器 UHF 链路故障"],
    ["bit3", B("SELF_ERR_AM_COMM"), "AM 消磁器 Query 通信失败", "POST / RERUN 探测"],
    ["bit4", B("SELF_ERR_PARAM_CRC"), "参数区 CRC 失败回落默认值", "仅上电置位（不随探测刷新，只能 CLEAR 手动清）"],
    ["bit5", B("SELF_ERR_TRAVEL_SW"), "行程开关缺失/回零失败（明细见 QUERY switchErr：bit0=上 bit1=下）", "POST / RERUN；运行期开锁器未回零故障"],
    ["bit6~15", "—", "预留（恒 0）", "—"],
  ],
}));
C.push(body(["刷新语义：bit0~3、bit5 随 ", { m: "RERUN" }, " 探测结果整体刷新（故障已消除则自动清位）；bit4 为引导期事实，仅 ", { m: "CLEAR" }, " 可清。"]));
C.push(h2("12.2  子命令"));
C.push(caption("FC_SELFTEST_CTRL 子命令"));
C.push(tbl({
  headers: ["cmd", "名称", "主机 → 设备 (data)", "设备 → 主机 (data)", "说明"], widths: [8, 11, 16, 30, 35], size: 17,
  rows: [
    [B("0x01"), "QUERY", B("[cmd]"), B("[cmd,err,errBitsL,errBitsH,motorCommOk,drvFault,uhfLink,amLink,paramCrc,switchErr]"), "读锁存错误位 + 实时诊断快照（无阻塞），字段见 12.3"],
    [B("0x02"), "RERUN", B("[cmd]"), B("[cmd,err,errBitsL,errBitsH]"), "重探外设并刷新锁存位：阻塞约 3s（UHF 上电+回帧、AM 回帧超时），期间喂狗不复位。仅 Locker/Unlock 空闲时接受，否则回 err=2 BUSY"],
    [B("0x03"), "CLEAR", B("[cmd,maskL,maskH]"), B("[cmd,err,errBitsL,errBitsH]"), "按掩码清指定位（mask 与 12.1 位定义一致，可多选），响应回清位后的剩余位图"],
  ],
}));
C.push(h2("12.3  QUERY 响应字段"));
C.push(caption("FC_SELFTEST_CTRL QUERY 响应字段"));
C.push(tbl({
  headers: ["字段", "含义"], widths: [22, 78], size: 18,
  rows: [
    [B("errBitsL / errBitsH"), "锁存错误位图（低/高字节，位定义见 12.1）"],
    [B("motorCommOk"), "1 = 电机驱动配置回读一致（查询时实时复核寄存器）"],
    [B("drvFault"), "电机驱动实时故障寄存器（0=无故障）"],
    [B("uhfLink"), "UHF 实时链路：0=正常 / 1=超时 / 2=CRC 错误"],
    [B("amLink"), "AM 实时链路：0=正常 / 非 0=掉线"],
    [B("paramCrc"), "1 = 本次上电参数区曾 CRC 失败回落默认值（errBits bit4 的镜像）"],
    [B("switchErr"), "行程开关实时错误位：bit0=上行程错 / bit1=下行程错（同 FC_MOTOR_CTRL QUERY 的 switchErr）"],
  ],
}));
C.push(...callout("note", "错误码（data[1]）：", [
  [{ m: "0=OK, 1=参数错误（长度不足/未知子命令）, 2=BUSY（RERUN 时开锁流程运行中）" }, "。"],
]));

// ============ 13 IO_DIAG ============
setCh(13);
C.push(h1("13  IO 状态总览 FC_IO_DIAG（0x26）"));
C.push(body(["一帧读取全部 IO 与外设状态，复位后无需逐个外设轮询。请求帧 data 为空：", { m: "53 77 01 00 05 00 26 + CRC32" }, "。响应 data 10 字节："]));
C.push(caption("FC_IO_DIAG 响应 data 逐字节定义（10B）"));
C.push(tbl({
  headers: ["data 偏移", "字段", "详细含义"], widths: [14, 22, 64], size: 18, centerCols: [0],
  rows: [
    ["data[0]", B("result"), "0=OK"],
    ["data[1]", B("ir"), "红外光电门原始读数：1=触发（检测到放标）"],
    ["data[2]", B("keyUp"), "上行程开关：1=释放 / 0=按下（低有效）"],
    ["data[3]", B("keyDown"), "下行程开关：1=释放 / 0=按下"],
    ["data[4]", B("uhfPowered"), "UHF 模块电源：1=上电"],
    ["data[5]", B("antennaOk"), "UHF 天线回波检测 OK（最近一次 OPEN/CHECK_ANT 结果）"],
    ["data[6]", B("amLink"), "AM 解码器链路：0=正常 / 非 0=掉线（自动探测恢复中）"],
    ["data[7]", B("homing"), "后台回零状态：0=未运行 / 1=RUNNING / 2=READY / 3=FAILED"],
    ["data[8]", B("locker"), "Locker 状态机态（同 10.2 QUERY state）"],
    ["data[9]", B("testState"), "行程测试态（同 7.2 testState）"],
  ],
}));
C.push(...callout("tip", "典型用法：", [
  ["上位机复位后一帧即可判断：(a) 后台回零是否完成（", { m: "homing=2" }, " 才可 MOVE/TEST/开锁业务）；(b) UHF 是否上电且天线正常；(c) 行程开关实际电平（现场装调）；(d) Locker/测试是否占用。复位后约 12s 内 homing=1 属正常（后台回零窗口）。"],
]));

// ============ 14 灯语 ============
setCh(14);
C.push(h1("14  RGB 灯语状态表"));
C.push(body(["RGB 灯带由固件按业务状态自动驱动：上位机不可控、亦无需控制，语义仅映射流程状态（与硬件无关）。", { m: "稳态灯语" }, "表当前所处流程阶段；", { m: "闪烁灯语" }, "为瞬时事件，叠加在稳态上短时表达后回落。手动设色（第 11 章）仅空闲时叠加显示，10s 自动回收。"]));
C.push(h2("14.1  稳态灯语（按流程阶段）"));
C.push(caption("稳态灯语"));
C.push(tbl({
  headers: ["灯语", "场景"], widths: [30, 70], size: 19,
  rows: [
    ["灭", "待机 / 回降中 / 无需关注"],
    ["白慢闪", "等待放标（解锁流程光电门控窗，提示客户放标签）"],
    ["蓝慢闪", "盘点 / 校对进行中（设备工作）"],
    ["绿常亮", "磁块升起 / 保持（开锁中）"],
    ["白常亮", "软标等待（消磁窗）"],
    ["黄慢闪", "后台回零"],
    ["黄快闪", "行程测试 / 安全回退"],
    ["红常亮", "故障"],
  ],
}));
C.push(h2("14.2  闪烁灯语（瞬时事件）"));
C.push(caption("闪烁灯语"));
C.push(tbl({
  headers: ["灯语", "事件"], widths: [32, 68], size: 19,
  rows: [
    ["绿单闪 200ms", "硬标签匹配（期望 EPC 稳定确认）"],
    ["蓝单闪 200ms", "解码到一张标签（任意 EPC）"],
    ["白单闪", "软标消耗 / 消磁成功"],
    ["红快闪 3s", "EPC 失配（不升起）"],
    ["红双闪 1s", "消磁失败（含软标窗 5min 超时未校验完成）/ UHF、AM 链路断"],
    ["黄慢闪 2s", "未放标 / 无标签收尾（NO_IR / NO_TAG）"],
    ["绿三连闪 1.8s", "结账完成（伴随蜂鸣 300ms）"],
  ],
}));
C.push(h2("14.3  自检类灯语（黄系）"));
C.push(caption("自检类灯语"));
C.push(tbl({
  headers: ["灯语", "场景"], widths: [32, 68], size: 19,
  rows: [
    ["黄慢闪", "回零中（同稳态回零）"],
    ["黄快闪", "自检 / 测试进行中"],
    ["黄 500ms 闪烁", "上行程开关错误"],
    ["粉红 500ms 闪烁", "下行程开关错误"],
  ],
}));

// ============ 15 CRC ============
setCh(15);
C.push(h1("15  CRC32 校验算法"));
C.push(caption("CRC32 参数（MPEG-2 变体）"));
C.push(tbl({
  headers: ["参数", "值"], widths: [34, 66], size: 19,
  rows: [
    ["多项式 poly", B("0x04C11DB7")],
    ["初值 init", B("0xFFFFFFFF")],
    ["输入反射 / 输出反射", "无 / 无"],
    ["最终 XOR", "无"],
    ["覆盖范围", "帧头 0x53 起至数据域末尾（即 CRC 之前的全部 N+7 字节）"],
    ["附加方式", ["4 字节", { t: "小端", b: true }, "尾附（crc & 0xFF 在最前）"]],
  ],
}));
C.push(body(["参考实现（C 语言）："]));
C.push(...codeBlock([
  "uint32_t crc32_mpeg2(const uint8_t *p, uint32_t len) {",
  "    uint32_t crc = 0xFFFFFFFFu;",
  "    for (uint32_t i = 0; i < len; i++) {",
  "        crc ^= (uint32_t)p[i] << 24;",
  "        for (int b = 0; b < 8; b++)",
  "            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u",
  "                                      : (crc << 1);",
  "    }",
  "    return crc;                        /* 无反射, 无最终 XOR */",
  "}",
]));

// ============ 16 示例帧逐字节解析 ============
setCh(16);
C.push(h1("16  示例帧逐字节解析"));
C.push(body(["本章所有字节序列均来自实机验证帧。每张表逐字节给出偏移、字节值、所属字段与详细含义，可直接作为上位机解析器的对拍样例。"]));

C.push(h2("16.1  握手请求"));
C.push(body(["FC_HANDSHAKE（0x01），data 为空，length=5，总帧长 11 字节。16 进制原文："]));
C.push(...codeBlock(["53 77 01 00 05 00 01 C5 69 2F 26"]));
C.push(caption("握手请求帧逐字节注释（11B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1（ASCII S）"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2（ASCII w）"],
  ["2", B("0x01"), B("devAddr"), "目标设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留，必须 0x00"],
  ["4", B("0x05"), B("length L"), "payload=5（func 1B + data 0B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=5（N=0，无数据域）"],
  ["6", B("0x01"), B("func"), "命令码 0x01 FC_HANDSHAKE"],
  ["7", B("0xC5"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..6"],
  ["8", B("0x69"), B("crc32[1]"), "CRC32 第 2 字节（小端）"],
  ["9", B("0x2F"), B("crc32[2]"), "CRC32 第 3 字节（小端）"],
  ["10", B("0x26"), B("crc32[3]"), "CRC32 高字节（小端）"],
]));

C.push(h2("16.2  握手响应（实机）"));
C.push(body(["实机响应（protoVer=3、status=RUN、layer=1 App、baudRate=115200、升级次数 0），总帧长 39 字节："]));
C.push(...codeBlock([
  "53 77 01 00 21 00 FE 00 03 01 1C D9 37 30 38 33 0B 00 39 35 37 39",
  "9D 43 A2 4D 01 00 00 00 00 00 C2 01 00 25 56 0B 38",
]));
C.push(caption("握手响应帧逐字节注释（39B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x21"), B("length L"), "payload=0x21=33（func 1B + data 28B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=33，数据域 N=28"],
  ["6", B("0xFE"), B("func"), "响应命令码 = 0x01 ^ 0xFF = 0xFE"],
  ["7", B("0x00"), B("result"), "data[0]：result=0 OK"],
  ["8", B("0x03"), B("protoVer"), "data[1]：协议版本 3"],
  ["9", B("0x01"), B("status"), "data[2]：设备状态 1=RUN"],
  ["10", B("0x1C"), B("UID[0]"), "data[3]：STM32 96 位唯一 ID 第 1 字节"],
  ["11", B("0xD9"), B("UID[1]"), "data[4]：唯一 ID 第 2 字节"],
  ["12", B("0x37"), B("UID[2]"), "data[5]：唯一 ID 第 3 字节"],
  ["13", B("0x30"), B("UID[3]"), "data[6]：唯一 ID 第 4 字节"],
  ["14", B("0x38"), B("UID[4]"), "data[7]：唯一 ID 第 5 字节"],
  ["15", B("0x33"), B("UID[5]"), "data[8]：唯一 ID 第 6 字节"],
  ["16", B("0x0B"), B("UID[6]"), "data[9]：唯一 ID 第 7 字节"],
  ["17", B("0x00"), B("UID[7]"), "data[10]：唯一 ID 第 8 字节"],
  ["18", B("0x39"), B("UID[8]"), "data[11]：唯一 ID 第 9 字节"],
  ["19", B("0x35"), B("UID[9]"), "data[12]：唯一 ID 第 10 字节"],
  ["20", B("0x37"), B("UID[10]"), "data[13]：唯一 ID 第 11 字节"],
  ["21", B("0x39"), B("UID[11]"), "data[14]：唯一 ID 第 12 字节（共 12B 结束）"],
  ["22", B("0x9D"), B("uidHash[0]"), "data[15]：CRC32(UID) 低字节（小端 4B）"],
  ["23", B("0x43"), B("uidHash[1]"), "data[16]：设备身份指纹第 2 字节"],
  ["24", B("0xA2"), B("uidHash[2]"), "data[17]：设备身份指纹第 3 字节"],
  ["25", B("0x4D"), B("uidHash[3]"), "data[18]：设备身份指纹高字节"],
  ["26", B("0x01"), B("layer"), "data[19]：1 = App 层（Boot 返回 0）"],
  ["27", B("0x00"), B("upgradeCount[0]"), "data[20]：累计升级次数低字节（LE）"],
  ["28", B("0x00"), B("upgradeCount[1]"), "data[21]：升级次数第 2 字节"],
  ["29", B("0x00"), B("upgradeCount[2]"), "data[22]：升级次数第 3 字节"],
  ["30", B("0x00"), B("upgradeCount[3]"), "data[23]：升级次数高字节 = 0 次"],
  ["31", B("0xC2"), B("baudRate[0]"), "data[24]：UART 波特率低字节（LE）"],
  ["32", B("0x01"), B("baudRate[1]"), "data[25]：波特率第 2 字节"],
  ["33", B("0x00"), B("baudRate[2]"), "data[26]：波特率第 3 字节"],
  ["34", B("0x00"), B("baudRate[3]"), "data[27]：波特率高字节；0x0001C200 = 115200"],
  ["35", B("0x25"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..34 共 35 字节"],
  ["36", B("0x56"), B("crc32[1]"), "CRC32 第 2 字节（小端）"],
  ["37", B("0x0B"), B("crc32[2]"), "CRC32 第 3 字节（小端）"],
  ["38", B("0x38"), B("crc32[3]"), "CRC32 高字节（小端）"],
]));

C.push(h2("16.3  UHF 开放（OPEN）请求"));
C.push(...codeBlock(["53 77 01 00 06 00 21 01 AD A7 D9 E9"]));
C.push(caption("UHF OPEN 请求帧逐字节注释（12B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x06"), B("length L"), "payload=6（func 1B + data 1B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=6，数据域 N=1"],
  ["6", B("0x21"), B("func"), "命令码 0x21 FC_UHF_CTRL"],
  ["7", B("0x01"), B("data[0]"), "子命令码 0x01 OPEN：上电+配置+回波检测"],
  ["8", B("0xAD"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..7"],
  ["9", B("0xA7"), B("crc32[1]"), "CRC32 第 2 字节"],
  ["10", B("0xD9"), B("crc32[2]"), "CRC32 第 3 字节"],
  ["11", B("0xE9"), B("crc32[3]"), "CRC32 高字节"],
]));

C.push(h2("16.4  UHF 盘点（INVENTORY）请求与响应"));
C.push(body(["盘点请求（超时 1000ms）："]));
C.push(...codeBlock(["53 77 01 00 08 00 21 03 E8 03 3D 41 A4 CB"]));
C.push(caption("UHF 盘点请求帧逐字节注释（14B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x08"), B("length L"), "payload=8（func 1B + data 3B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=8，数据域 N=3"],
  ["6", B("0x21"), B("func"), "命令码 0x21 FC_UHF_CTRL"],
  ["7", B("0x03"), B("data[0]"), "子命令码 0x03 INVENTORY"],
  ["8", B("0xE8"), B("tmo L"), "data[1]：盘存超时低字节"],
  ["9", B("0x03"), B("tmo H"), "data[2]：超时高字节；0x03E8 = 1000ms"],
  ["10", B("0x3D"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..9"],
  ["11", B("0x41"), B("crc32[1]"), "CRC32 第 2 字节"],
  ["12", B("0xA4"), B("crc32[2]"), "CRC32 第 3 字节"],
  ["13", B("0xCB"), B("crc32[3]"), "CRC32 高字节"],
]));
C.push(body(["实机响应（场内 1 张标签，EPC 12 字节），响应 func=0x21^0xFF=0xDE，总帧长 29 字节："]));
C.push(...codeBlock([
  "53 77 01 00 17 00 DE 03 00 01 00 FF 0C 33 55 34 63 A4 00 01 58 EB",
  "7E 75 07 7D 44 AA D8",
]));
C.push(caption("UHF 盘点响应帧逐字节注释（29B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x17"), B("length L"), "payload=0x17=23（func 1B + data 18B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=23，数据域 N=18"],
  ["6", B("0xDE"), B("func"), "响应命令码 = 0x21 ^ 0xFF = 0xDE"],
  ["7", B("0x03"), B("cmd"), "data[0]：子命令 0x03 回显"],
  ["8", B("0x00"), B("err"), "data[1]：err=0 OK，标签数据紧随其后"],
  ["9", B("0x01"), B("count L"), "data[2]：标签计数低字节"],
  ["10", B("0x00"), B("count H"), "data[3]：计数高字节，共 1 张"],
  ["11", B("0xFF"), B("rssi"), "data[4]：第 1 张标签场强 RSSI（0~255）"],
  ["12", B("0x0C"), B("epcLen"), "data[5]：EPC 长度 12 字节"],
  ["13", B("0x33"), B("EPC[0]"), "data[6]：EPC 第 1 字节"],
  ["14", B("0x55"), B("EPC[1]"), "data[7]：EPC 第 2 字节"],
  ["15", B("0x34"), B("EPC[2]"), "data[8]：EPC 第 3 字节"],
  ["16", B("0x63"), B("EPC[3]"), "data[9]：EPC 第 4 字节"],
  ["17", B("0xA4"), B("EPC[4]"), "data[10]：EPC 第 5 字节"],
  ["18", B("0x00"), B("EPC[5]"), "data[11]：EPC 第 6 字节"],
  ["19", B("0x01"), B("EPC[6]"), "data[12]：EPC 第 7 字节"],
  ["20", B("0x58"), B("EPC[7]"), "data[13]：EPC 第 8 字节"],
  ["21", B("0xEB"), B("EPC[8]"), "data[14]：EPC 第 9 字节"],
  ["22", B("0x7E"), B("EPC[9]"), "data[15]：EPC 第 10 字节"],
  ["23", B("0x75"), B("EPC[10]"), "data[16]：EPC 第 11 字节"],
  ["24", B("0x07"), B("EPC[11]"), "data[17]：EPC 第 12 字节（该标签数据结束）"],
  ["25", B("0x7D"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..24 共 25 字节"],
  ["26", B("0x44"), B("crc32[1]"), "CRC32 第 2 字节"],
  ["27", B("0xAA"), B("crc32[2]"), "CRC32 第 3 字节"],
  ["28", B("0xD8"), B("crc32[3]"), "CRC32 高字节"],
]));

C.push(h2("16.5  解锁请求 — 单标签（epcCnt=1）"));
C.push(body(["hold=0 → 解锁窗 W 由公式自动取 120s；示例 EPC 为 6 字节 0xAA 填充。总帧长 25 字节："]));
C.push(...codeBlock(["53 77 01 00 13 00 23 0A E8 03 00 00 00 01 06 AA AA AA AA AA AA 8F 6B 4E 46"]));
C.push(caption("单标签解锁请求帧逐字节注释（25B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x13"), B("length L"), "payload=0x13=19（func 1B + data 14B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=19，数据域 N=14"],
  ["6", B("0x23"), B("func"), "命令码 0x23 FC_LOCKER_CTRL"],
  ["7", B("0x0A"), B("cmd"), "data[0]：子命令 0x0A UNLOCK_MULTI（唯一开锁通道）"],
  ["8", B("0xE8"), B("tmo L"), "data[1]：每轮盘点时限低字节"],
  ["9", B("0x03"), B("tmo H"), "data[2]：时限高字节；0x03E8 = 1000ms"],
  ["10", B("0x00"), B("hold L"), "data[3]：解锁窗上报值低字节"],
  ["11", B("0x00"), B("hold H"), "data[4]：上报值高字节；hold=0 → W=120000+(1-1)×30000=120s"],
  ["12", B("0x00"), B("softCnt"), "data[5]：软标消磁数 0（跳过软标段，纯硬标任务）"],
  ["13", B("0x01"), B("epcCnt"), "data[6]：期望硬标签数 m=1（单标流程）"],
  ["14", B("0x06"), B("epcLen"), "data[7]：每张 EPC 6 字节"],
  ["15", B("0xAA"), B("EPC[0]"), "data[8]：期望 EPC 第 1 字节（示例填充 0xAA）"],
  ["16", B("0xAA"), B("EPC[1]"), "data[9]：期望 EPC 第 2 字节"],
  ["17", B("0xAA"), B("EPC[2]"), "data[10]：期望 EPC 第 3 字节"],
  ["18", B("0xAA"), B("EPC[3]"), "data[11]：期望 EPC 第 4 字节"],
  ["19", B("0xAA"), B("EPC[4]"), "data[12]：期望 EPC 第 5 字节"],
  ["20", B("0xAA"), B("EPC[5]"), "data[13]：期望 EPC 第 6 字节（epcCnt×epcLen=6 字节结束）"],
  ["21", B("0x8F"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..20 共 21 字节"],
  ["22", B("0x6B"), B("crc32[1]"), "CRC32 第 2 字节"],
  ["23", B("0x4E"), B("crc32[2]"), "CRC32 第 3 字节"],
  ["24", B("0x46"), B("crc32[3]"), "CRC32 高字节"],
]));

C.push(h2("16.6  解锁请求 — 双标签 / 三标签"));
C.push(body(["双标签（epcCnt=2 → W=150s，EPC 为 0xAA×6 + 0xBB×6），总帧长 31 字节："]));
C.push(...codeBlock(["53 77 01 00 19 00 23 0A E8 03 00 00 00 02 06 AA AA AA AA AA AA BB BB BB BB BB BB AF DC C0 6C"]));
C.push(body(["三标签（epcCnt=3 → W=180s，EPC 为 0xAA×6 + 0xBB×6 + 0x55×6；data=26B 仍单帧可载），总帧长 37 字节："]));
C.push(...codeBlock(["53 77 01 00 1F 00 23 0A E8 03 00 00 00 03 06 AA AA AA AA AA AA BB BB BB BB BB BB 55 55 55 55 55 55 3B 23 1D 6D"]));
C.push(caption("多标签请求与单标签请求的字节差异"));
C.push(tbl({
  headers: ["字段", "单标签", "双标签", "三标签"], widths: [28, 24, 24, 24], size: 19, centerCols: [1, 2, 3],
  rows: [
    [B("length (payload)"), B("0x13 (19)"), B("0x19 (25)"), B("0x1F (31)")],
    [B("epcCnt"), B("0x01"), B("0x02"), B("0x03")],
    [B("自动解锁窗 W"), "120s", "150s", "180s"],
    [B("EPC 字节流"), "AA×6", "AA×6 + BB×6", "AA×6 + BB×6 + 55×6"],
    [B("总帧长"), "25B", "31B", "37B"],
  ],
}));

C.push(h2("16.7  受理推帧 0x0F（EVT_START）"));
C.push(body(["短窗演示 W=3000ms。func=0x23^0xFF=0xDC，data[0]=0x0F，总帧长 17 字节："]));
C.push(...codeBlock(["53 77 01 00 0B 00 DC 0F 00 01 B8 0B 00 4B 18 26 80"]));
C.push(caption("受理推帧 0x0F 逐字节注释（17B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x0B"), B("length L"), "payload=0x0B=11（func 1B + data 6B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=11，数据域 N=6"],
  ["6", B("0xDC"), B("func"), "推送事件帧 func = 0x23 ^ 0xFF = 0xDC（非请求响应）"],
  ["7", B("0x0F"), B("evtCode"), "data[0]：事件码 0x0F EVT_START 受理"],
  ["8", B("0x00"), B("err"), "data[1]：0=受理成功"],
  ["9", B("0x01"), B("phase"), "data[2]：流程阶段 1=WAIT_TAG 光电门控（纯软标为 5=SOFT）"],
  ["10", B("0xB8"), B("winMs[0]"), "data[3]：实际解锁窗低字节（3 字节小端）"],
  ["11", B("0x0B"), B("winMs[1]"), "data[4]：窗口第 2 字节"],
  ["12", B("0x00"), B("winMs[2]"), "data[5]：窗口高字节；0x000BB8 = 3000ms"],
  ["13", B("0x4B"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..12 共 13 字节"],
  ["14", B("0x18"), B("crc32[1]"), "CRC32 第 2 字节"],
  ["15", B("0x26"), B("crc32[2]"), "CRC32 第 3 字节"],
  ["16", B("0x80"), B("crc32[3]"), "CRC32 高字节"],
]));

C.push(h2("16.8  终帧 0x0A（短窗到期收尾）"));
C.push(body(["短窗到期收尾：endReason=2 PARTIAL_TIMEOUT、0 张确认、磁块未动（rise=lower=0），总帧长 25 字节："]));
C.push(...codeBlock(["53 77 01 00 13 00 DC 0A 00 02 00 00 01 00 00 00 00 00 00 00 F4 0B 11 1F 8D B8"]));
C.push(caption("终帧 0x0A 逐字节注释（25B）"));
C.push(byteTable([
  ["0", B("0x53"), B("header1"), "帧同步字节 1"],
  ["1", B("0x77"), B("header2"), "帧同步字节 2"],
  ["2", B("0x01"), B("devAddr"), "设备地址 0x01"],
  ["3", B("0x00"), B("reserved"), "保留 0x00"],
  ["4", B("0x13"), B("length L"), "payload=0x13=19（func 1B + data 14B + CRC32 4B）"],
  ["5", B("0x00"), B("length H"), "length=19，数据域 N=14"],
  ["6", B("0xDC"), B("func"), "推送通道 func = 0x23 ^ 0xFF = 0xDC"],
  ["7", B("0x0A"), B("cmd"), "data[0]：终帧标志 0x0A（UNLOCK_MULTI 流程结束）"],
  ["8", B("0x00"), B("err"), "data[1]：err=0 OK（流程正常结束，非异常出口）"],
  ["9", B("0x02"), B("endReason"), "data[2]：2=PARTIAL_TIMEOUT 硬标窗满部分确认"],
  ["10", B("0x00"), B("bitmap"), "data[3]：确认位图 0x00，无任何一张确认"],
  ["11", B("0x00"), B("confirmed"), "data[4]：已确认 n=0"],
  ["12", B("0x01"), B("total"), "data[5]：清单总数 m=1"],
  ["13", B("0x00"), B("rise L"), "data[6]：升起微步数低字节"],
  ["14", B("0x00"), B("rise H"), "data[7]：升起微步数高字节；未全部确认 → 恒 0（全确认门控）"],
  ["15", B("0x00"), B("lower L"), "data[8]：回降微步数低字节"],
  ["16", B("0x00"), B("lower H"), "data[9]：回降微步数高字节；磁块全程未动"],
  ["17", B("0x00"), B("softDone"), "data[10]：软标已解码数 0"],
  ["18", B("0x00"), B("softCnt"), "data[11]：软标目标数 0"],
  ["19", B("0xF4"), B("elapsed L"), "data[12]：流程耗时低字节（自光电触发）"],
  ["20", B("0x0B"), B("elapsed H"), "data[13]：耗时高字节；0x0BF4 = 3060ms"],
  ["21", B("0x11"), B("crc32[0]"), "CRC32 低字节，覆盖偏移 0..20 共 21 字节"],
  ["22", B("0x1F"), B("crc32[1]"), "CRC32 第 2 字节"],
  ["23", B("0x8D"), B("crc32[2]"), "CRC32 第 3 字节"],
  ["24", B("0xB8"), B("crc32[3]"), "CRC32 高字节"],
]));

// ============ 17 附录 A ============
setCh(17);
C.push(h1("17  附录 A：主机接入样板（hidapi / C）"));
C.push(h2("A.1  打开设备"));
C.push(...codeBlock([
  "// hidapi",
  "handle = hid_open(0x5377, 0x5378, NULL);",
  "// 报告 ID = 0x02, 包长 64",
  "unsigned char report[64];             // 读写都先放 64B 缓冲",
]));
C.push(h2("A.2  发送请求（以握手为例）"));
C.push(...codeBlock([
  "// 例: FC_HANDSHAKE",
  "uint8_t frame[12];",
  "frame[0] = 0x53; frame[1] = 0x77;            // header \"Sw\"",
  "frame[2] = 0x01;                              // devAddr",
  "frame[3] = 0x00;                              // reserved (必须 0x00)",
  "frame[4] = 5;  frame[5] = 0;                  // length LE",
  "frame[6] = 0x01;                              // FC_HANDSHAKE",
  "// frame[7..10] = CRC32 覆盖前 7B",
  "uint32_t crc = crc32_mpeg2(frame, 7);         // MPEG-2, 无反射, 无最终 XOR (见第 15 章)",
  "memcpy(frame + 7, &crc, 4);                   // CRC32 LE",
  "hid_write(handle, frame, 12);",
]));
C.push(h2("A.3  接收响应 / 事件"));
C.push(...codeBlock([
  "int n = hid_read(handle, report, 64);",
  "if (n >= 64 && report[0] == 0x02) {",
  "    // 报告 ID 剥除后, 剩余 63B 是协议帧",
  "    parse_frame(report + 1, n - 1, handle);",
  "    // response func = request func ^ 0xFF",
  "    // func = 0xDC 且 data[0] = 0x0B..0x0F 时为解锁流程事件帧",
  "}",
]));
C.push(...callout("note", "解析要点：", [
  ["先校验帧头与 CRC32，再按 length 截取数据域；func=0xDC 的事件帧（0x0B~0x0F）与 0x0A 终帧是 UNLOCK_MULTI 流程的三类关键帧，须在独立接收线程中异步处理。"],
]));

// ───────────────────────── 页眉页脚 ─────────────────────────
function makeFooter() {
  return new Footer({
    children: [new Paragraph({
      alignment: AlignmentType.CENTER,
      children: [new TextRun({ children: [PageNumber.CURRENT], size: 18, color: P.secondary, font: F_BODY })],
    })],
  });
}
function makeHeader() {
  return new Header({
    children: [new Paragraph({
      alignment: AlignmentType.CENTER,
      border: { bottom: { style: BorderStyle.SINGLE, size: 4, color: P.innerLine, space: 4 } },
      children: [new TextRun({ text: "ZLR5401 应用层通信协议 · V3", size: 16, color: P.secondary, font: F_BODY })],
    })],
  });
}

// ───────────────────────── 组装文档 ─────────────────────────
const pgSize = { width: 11906, height: 16838 };
const pgMargin = { top: 1440, bottom: 1440, left: 1701, right: 1417 };

const doc = new Document({
  creator: "ZLR5401",
  title: "ZLR5401 应用层通信协议 V3",
  description: "App USB Protocol Host API Reference",
  styles: {
    default: {
      document: {
        run: { font: F_BODY, size: 24, color: P.body },
        paragraph: { spacing: { line: 312 } },
      },
      heading1: {
        run: { font: F_HEAD, size: 32, bold: true, color: P.primary },
        paragraph: { spacing: { before: 360, after: 200, line: 312 }, outlineLevel: 0 },
      },
      heading2: {
        run: { font: F_HEAD, size: 28, bold: true, color: P.h2 },
        paragraph: { spacing: { before: 280, after: 140, line: 312 }, outlineLevel: 1 },
      },
      heading3: {
        run: { font: F_HEAD, size: 24, bold: true, color: P.h3 },
        paragraph: { spacing: { before: 220, after: 110, line: 312 }, outlineLevel: 2 },
      },
      heading4: {
        run: { font: F_HEAD, size: 22, bold: true, color: P.h3 },
        paragraph: { spacing: { before: 180, after: 90, line: 312 }, outlineLevel: 3 },
      },
    },
  },
  sections: [
    // 封面
    {
      properties: {
        page: { size: pgSize, margin: { top: 0, bottom: 0, left: 0, right: 0 } },
      },
      children: buildCoverR2({
        englishLabel: "COMMUNICATION PROTOCOL SPECIFICATION",
        title: "ZLR5401 应用层通信协议",
        subtitle: "App USB Protocol — 主机端 API 参考手册",
        metaLines: [
          "协议版本：V3",
          "传输层：USB HID Interrupt · 64B · 报告 ID 0x02",
          "命令分层：系统级 0x01~0x09 · 应用级 0x20~0x26",
          "文档日期：2026 年 9 月 4 日",
        ],
        footerRight: "ZLR5401 系列技术文档",
      }),
    },
    // 目录（罗马页码）
    {
      properties: {
        type: SectionType.NEXT_PAGE,
        page: { size: pgSize, margin: pgMargin, pageNumbers: { start: 1, formatType: NumberFormat.UPPER_ROMAN } },
      },
      footers: { default: makeFooter() },
      children: [
        new Paragraph({
          alignment: AlignmentType.CENTER,
          spacing: { before: 480, after: 360 },
          children: [new TextRun({ text: "目  录", bold: true, size: 32, color: P.primary, font: F_HEAD })],
        }),
        new TableOfContents("目录", { hyperlink: true, headingStyleRange: "1-3" }),
        new Paragraph({
          spacing: { before: 200 },
          children: [new TextRun({
            text: "注：本目录由域代码生成。编辑文档后请在目录上右键选择更新域，以刷新页码。",
            italics: true, size: 18, color: "888888", font: F_BODY,
          })],
        }),
      ],
    },
    // 正文（阿拉伯页码，从 1 起）
    {
      properties: {
        type: SectionType.NEXT_PAGE,
        page: { size: pgSize, margin: pgMargin, pageNumbers: { start: 1, formatType: NumberFormat.DECIMAL } },
      },
      headers: { default: makeHeader() },
      footers: { default: makeFooter() },
      children: C,
    },
  ],
});

Packer.toBuffer(doc).then(buf => {
  fs.writeFileSync("App_Protocol_V3.0.docx", buf);
  console.log("OK: App_Protocol_V3.0.docx (" + buf.length + " bytes)");
});
