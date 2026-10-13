// 校验 generate_pptx_v3.js (客户版协议文档) 中全部测试帧:
//   1) CRC-32/MPEG-2 (低字节在前追加帧尾)  2) 长度域 = 帧长-6  3) 预留位=0 / 帧头
// 用法: node verify_frames.js   (Protocol/ 目录下)
const fs = require("fs");
const SRC = __dirname + "/generate_pptx_v3.js";
const src = fs.readFileSync(SRC, "utf8");

function crc32mpeg2(buf) {
  let crc = 0xFFFFFFFF;
  for (const b of buf) {
    crc = ((crc ^ (b << 24)) >>> 0);
    for (let i = 0; i < 8; i++)
      crc = (crc & 0x80000000) ? (((crc << 1) >>> 0) ^ 0x04C11DB7) >>> 0 : (crc << 1) >>> 0;
  }
  return crc >>> 0;
}
const le32 = (b, o) => ((b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0);

const frames = [];
{ const re = /hex:\s*"([0-9A-Fa-f ]+)"/g; let m;
  while ((m = re.exec(src))) frames.push(["hex", m[1]]); }
{ const hid = src.match(/OUT: (00 [0-9A-Fa-f ]+?)\s*←/);
  if (hid) frames.push(["HID-OUT", hid[1].trim()]); }
{ const hin = src.match(/IN\s*:\s*(02 [0-9A-Fa-f ]+?)(?:\s*←|\s*")/s);
  if (hin) frames.push(["HID-IN", hin[1].replace(/\s+/g, " ").trim()]); }

let ok = 0, fail = 0;
for (const [where, hx0] of frames) {
  const hx = hx0.replace(/\s+/g, " ").trim();
  let by = Buffer.from(hx.replace(/ /g, ""), "hex");
  let pre = "";
  if ((by[0] === 0x00 || by[0] === 0x02) && by[1] === 0x53) { pre = `HID前缀${by[0].toString(16)} `; by = by.subarray(1); }
  const n = by.length, iss = [];
  if (n < 11) { console.log(`SKIP ${where} 非完整帧(${n}B): ${hx}`); continue; }
  if (by[0] !== 0x53 || by[1] !== 0x77) iss.push("帧头错");
  if (by[3] !== 0) iss.push(`预留=${by[3].toString(16)}`);
  const lf = by[4] | (by[5] << 8);
  if (lf !== n - 6) iss.push(`长度域${lf}≠${n - 6}`);
  const calc = crc32mpeg2(by.subarray(0, n - 4));
  if (calc !== le32(by, n - 4)) iss.push(`CRC不符 calc=${calc.toString(16).toUpperCase()} recv=${le32(by, n-4).toString(16).toUpperCase()}`);
  if (iss.length) { fail++; console.log(`FAIL ${where} ${pre}${hx}\n     -> ${iss.join("; ")}`); }
  else { ok++; console.log(`OK   ${where} ${pre}${n}B FC=${by[6].toString(16).toUpperCase().padStart(2, "0")} data=${n - 11}B`); }
}
console.log(`\n合计 ${ok + fail} 帧: OK=${ok} FAIL=${fail}`);
