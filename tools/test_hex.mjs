/**
 * docs/js/hex.js 的单元测试（Node 直接跑，不需要浏览器）
 *
 *   node tools/test_hex.mjs
 *
 * 两类验证，缺一不可：
 *   ① 手工向量：hex 字符串由**独立的 Python 实现**生成（与 hex.js 无共享代码），
 *      期望的镜像字节也来自那份实现 —— 这才是真正的交叉验证。
 *   ② 往返：本文件里的编码器生成 hex、再喂给解析器比对，覆盖大数据与跨 64K 边界。
 *
 * 只测解析器本身；「起始地址必须是 APP 区起点」这类策略由 app.js 传入参数决定，
 * 但默认值也在这里测一次（避免默认值被改错）。
 */

import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseHex, describeHex, HEX_DEFAULT_BASE } from '../docs/js/hex.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

let pass = 0, fail = 0;

function ok(cond, what) {
  if (cond) { pass++; return; }
  fail++;
  console.log('    FAIL: ' + what);
}

function eq(got, want, what) {
  if (got === want) { pass++; return; }
  fail++;
  console.log('    FAIL: ' + what + '  (got ' + got + ', want ' + want + ')');
}

/** "AABBCC" → Uint8Array */
function bytes(h) {
  return Uint8Array.from(h.match(/../g) || [], (x) => parseInt(x, 16));
}

function bytesEq(a, b, what) {
  if (a.length === b.length && a.every((v, i) => v === b[i])) { pass++; return; }
  fail++;
  const n = Math.min(a.length, b.length);
  let at = -1;
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) { at = i; break; }
  console.log('    FAIL: ' + what + `  (len ${a.length} vs ${b.length}`
    + (at < 0 ? '' : `, 首个不同 @${at}: 0x${a[at].toString(16)} vs 0x${b[at].toString(16)}`) + ')');
}

function throws(fn, needle, what) {
  try {
    fn();
    fail++;
    console.log('    FAIL: ' + what + '（没有抛错）');
  } catch (e) {
    if (e.message.includes(needle)) { pass++; return; }
    fail++;
    console.log('    FAIL: ' + what + `  (报的是「${e.message}」，期望含「${needle}」)`);
  }
}

// ------------------------------------------------------------------ 11 组向量

const APP = 0x08004000;

// ① 单段
const V1 = [
  ':020000040800F2',
  ':10400000000102030405060708090A0B0C0D0E0F38',
  ':00000001FF',
].join('\n');

// ② 两段带 12 字节空洞
const V2 = [
  ':020000040800F2',
  ':04400000AABBCCDDAE',
  ':044010001122334402',
  ':00000001FF',
].join('\n');

// ③ 跨 64K 边界（0x0800FFFC → 0x08010000），中间有一次 ELA 切换
const V3 = [
  ':020000040800F2',
  ':04FFFC0000010203FB',
  ':020000040801F1',
  ':0400000004050607E6',
  ':00000001FF',
].join('\n');

// ④ 用扩展段地址记录（type 02），只能表示 20 位地址空间 → 配合自定义 base
const V4 = [
  ':020000020100FB',
  ':0400000001020304F2',
  ':00000001FF',
].join('\n');

// ⑤ EOF 之后还有非记录内容（应被忽略）
const V5 = V1 + '\n这是 EOF 之后的内容，不是合法记录\n:::\nFFFF';

// ⑥ 小写 + 空行 + 行首尾空白（真实文件常见）
const V6 = '  \n'
  + ':020000040800f2\n\n:0840000000010203040506079c\n\n'
  + ':0840080008090a0b0c0d0e0f54\n\n:00000001ff\n\n';

console.log('=== [1] 手工向量（hex 字符串由独立 Python 实现生成）===');

{
  const r = parseHex(V1);
  eq(r.base, APP, 'v1 base');
  eq(r.size, 16, 'v1 size');
  eq(r.padded, 0, 'v1 padded');
  eq(r.dataBytes, 16, 'v1 dataBytes');
  bytesEq(r.data, bytes('000102030405060708090A0B0C0D0E0F'), 'v1 data');
}

{
  const r = parseHex(V2);
  eq(r.size, 20, 'v2 size（含空洞）');
  eq(r.padded, 12, 'v2 padded = 12');
  bytesEq(r.data, bytes('AABBCCDDFFFFFFFFFFFFFFFFFFFFFFFF11223344'), 'v2 data（空洞填 0xFF）');
}

{
  const r = parseHex(V3, { base: 0x0800FFFC });
  eq(r.base, 0x0800FFFC, 'v3 base');
  eq(r.size, 8, 'v3 size');
  eq(r.padded, 0, 'v3 padded（跨 64K 但连续，无洞）');
  bytesEq(r.data, bytes('0001020304050607'), 'v3 data');
}

{
  const r = parseHex(V4, { base: 0x1000 });        // type 02：upper = 0x0100 << 4
  eq(r.base, 0x1000, 'v4 base（扩展段地址）');
  bytesEq(r.data, bytes('01020304'), 'v4 data');
}

{
  const r = parseHex(V5);
  eq(r.size, 16, 'v5 size（EOF 之后的内容被忽略）');
  bytesEq(r.data, bytes('000102030405060708090A0B0C0D0E0F'), 'v5 data');
}

{
  const r = parseHex(V6);
  eq(r.size, 16, 'v6 size（小写 + 空行 + 空白）');
  bytesEq(r.data, bytes('000102030405060708090A0B0C0D0E0F'), 'v6 data');
}

console.log('=== [2] 默认 base 与 describeHex ===');

{
  eq(HEX_DEFAULT_BASE, APP, '默认 base = 0x08004000（= 固件 BL_APP_BASE）');
  const r = parseHex(V1);                          // 不给 base，走默认值
  eq(r.base, APP, 'v1 用默认 base 也能过');
  ok(typeof describeHex(r) === 'string' && describeHex(r).includes('0x08004000'),
     'describeHex 含起始地址');
  const r2 = parseHex(V2);
  ok(describeHex(r2).includes('空洞'), 'describeHex 在有空洞时提到填充');
}

console.log('=== [3] 随机往返（本文件编码 → 解析器解码）===');

function rng(seed) {
  return function () {
    seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function makeHex(base, data, line = 16) {
  const out = [];
  const rec = (typ, addr16, chunk) => {
    const b = [chunk.length, (addr16 >> 8) & 0xFF, addr16 & 0xFF, typ, ...chunk];
    const s = b.reduce((a, v) => (a + v) & 0xFF, 0);
    out.push(':' + [...b, (0x100 - s) & 0xFF]
      .map((v) => v.toString(16).padStart(2, '0').toUpperCase()).join(''));
  };
  let upper = -1;
  for (let off = 0; off < data.length;) {
    const a = base + off;
    const up = Math.floor(a / 0x10000);
    if (up !== upper) { rec(0x04, 0, [(up >> 8) & 0xFF, up & 0xFF]); upper = up; }
    const n = Math.min(line, data.length - off, 0x10000 - (a % 0x10000));
    rec(0x00, a % 0x10000, Array.from(data.subarray(off, off + n)));
    off += n;
  }
  rec(0x01, 0, []);
  return out.join('\n');
}

for (const [size, seed, line] of [[16, 1, 16], [1024, 2, 16], [4096, 3, 32],
                                  [65536 + 512, 4, 16], [200 * 1024, 5, 16]]) {
  const r = rng(seed);
  const data = Uint8Array.from({ length: size }, () => Math.floor(r() * 256));
  const txt = makeHex(APP, data, line);
  const got = parseHex(txt);
  eq(got.size, size, `${size} 字节往返 size（${line} 字节/行）`);
  bytesEq(got.data, data, `${size} 字节往返 data`);
}

{
  // 含 0xFF 的真实数据不能与「空洞填充」混淆 —— 空洞是补出来的，数据里的 0xFF 是文件里的
  const data = new Uint8Array(1024).fill(0xFF);
  const r = parseHex(makeHex(APP, data));
  eq(r.padded, 0, '数据全是 0xFF 时 padded 应为 0（不是把数据当成空洞）');
  bytesEq(r.data, data, '全 0xFF 数据往返');
}

console.log('=== [4] 错误向量（报错信息要说人话）===');

throws(() => parseHex([
  ':020000040800F2',
  ':044000000001020300',        // 校验和故意改错
  ':00000001FF',
].join('\n')), '校验和错误', 'e1 校验和错');

throws(() => parseHex([
  ':020000040800F2',
  '0440000000010203B6',         // 少了开头的 ':'
  ':00000001FF',
].join('\n')), "记录必须以 ':' 开头", 'e2 缺冒号');

throws(() => parseHex([
  ':020000040800F2',
  ':0840000000010203B6',        // 声明 8 字节，实际 4
  ':00000001FF',
].join('\n')), '声明 8 字节数据', 'e3 长度字段不符');

throws(() => parseHex(':00000001FF'), '没有任何数据记录', 'e4 只有 EOF');

throws(() => parseHex([
  ':020000040800F2',
  ':10000000000102030405060708090A0B0C0D0E0F78',   // 0x08000000 —— 会把 Bootloader 编进去
  ':00000001FF',
].join('\n')), '低于 APP 区起点', 'e5 地址偏低');

throws(() => parseHex([
  ':020000040800F2',
  ':10800000000102030405060708090A0B0C0D0E0FF8',   // 0x08008000
  ':00000001FF',
].join('\n')), '高于 APP 区起点', 'e6 地址偏高');

throws(() => parseHex(V1, { maxSize: 8 }), '超过 APP 区上限', 'e7 超过 maxSize');

throws(() => parseHex([
  ':020000040800F2',
  ':04400000ZZ01020304',
  ':00000001FF',
].join('\n')), '含非十六进制字符', 'e8 非十六进制字符');

throws(() => parseHex([
  ':020000040800F2',
  ':0440000600010203B0',        // 记录类型 06
  ':00000001FF',
].join('\n')), '未知的记录类型', 'e9 未知记录类型');

throws(() => parseHex([
  ':03000004080000F1',          // ELA 带 3 字节
  ':0440000000010203B6',
  ':00000001FF',
].join('\n')), '扩展线性地址记录应为 2 字节', 'e10 ELA 长度不对');

throws(() => parseHex([
  ':020000040800F2',
  ':0440000000010203',          // 声明 4 字节数据，但记录只有 8 字节（应为 9）
].join('\n')), '与实际记录长度不符', 'e11 记录被截断');

throws(() => parseHex([
  ':020000040800F2',
  ':044000000001020',           // 15 个 hex 字符（奇数个）
].join('\n')), '记录长度非法', 'e14 记录字符数为奇数');

throws(() => parseHex('这不是一个 hex 文件\n随便写点什么'), "记录必须以 ':' 开头", 'e12 完全不是 hex');

console.log('=== [5] 边界 ===');
{
  const r = parseHex(makeHex(APP, new Uint8Array([0x11, 0x22])));
  eq(r.size, 2, '最小能解析的镜像 = 2 字节（大小检查由调用方做）');
  // 只有 ELA + EOF、没有数据记录 → 必须抛错，不能返回一个 0 字节镜像
  throws(() => parseHex([':020000040800F2', ':00000001FF'].join('\n')),
         '没有任何数据记录', 'e13 只有 ELA 与 EOF');
}

console.log('=== [6] 真实 Keil 输出（build/ 里有产物才跑）===');
{
  // 这一节是最强的验证：拿 fromelf --i32 产出的 .hex，解析结果必须与
  // fromelf --bin 的 .bin 逐字节一致 —— 两条独立的转换路径互相对照。
  // build/ 是编译产物（不入库），没有就跳过，不算失败。
  const pairs = [['app_test.hex', 'app_test.bin'],
                 ['app_fail1.hex', 'app_fail1.bin'],
                 ['app_fail2.hex', 'app_fail2.bin']];
  let checked = 0;
  for (const [h, bn] of pairs) {
    const hp = resolve(ROOT, 'build', h);
    const bp = resolve(ROOT, 'build', bn);
    if (!existsSync(hp) || !existsSync(bp)) continue;
    const r = parseHex(readFileSync(hp, 'utf8'), { base: APP });
    bytesEq(r.data, new Uint8Array(readFileSync(bp)),
      `${h} 解析结果 = fromelf --bin`);
    checked++;
  }

  const blh = resolve(ROOT, 'build', 'bl_f401.hex');       // bootloader 自己的 hex
  if (existsSync(blh)) {
    throws(() => parseHex(readFileSync(blh, 'utf8'), { base: APP }),
      '低于 APP 区起点', 'bl_f401.hex 被拒（它是 0x08000000 起）');
  }
  console.log(checked
    ? `  已与 ${checked} 组真实产物逐字节比对`
    : '  （build/ 里没有 .hex/.bin，跳过 —— 先在 TestApp 跑一次 build_app.py）');
}

console.log();
console.log(`=== 结果：${pass} 项通过，${fail} 项失败 ===`);
process.exit(fail ? 1 : 0);
