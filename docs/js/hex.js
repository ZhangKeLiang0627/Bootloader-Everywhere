/**
 * Intel HEX 解析 —— 把 .hex 还原成「从 APP 区起点开始的连续镜像」。
 *
 * 零依赖，浏览器与 Node 通用（与 protocol.js 同定位：与具体工程解耦，可整对拷走）。
 *
 * 只做刷机需要的部分：
 *   - 数据记录（type 00）
 *   - 扩展线性地址（type 04）/ 扩展段地址（type 02）
 *   - 文件结束（type 01）
 *   - 起始地址记录（type 03 / 05）忽略 —— 那是调试器用的入口点，不影响烧写布局
 * 记录之间的空洞用 0xFF 填充：这正是 Flash 擦除后的值，等价于「那些字节不写」。
 *
 * 为什么必须知道起始地址：IAP 只往 APP 区写，镜像第 0 字节必须落在
 * BL_APP_BASE（默认 0x08004000，Bootloader 占前 16 KB）。
 * .bin 里没有地址信息，只能信任；.hex 自带地址，所以这里能提前把「编错地址」挡住 ——
 * 这比让板子在 START 阶段回一个错误码要早得多，也更容易看懂。
 */

export const HEX_DEFAULT_BASE = 0x08004000;   // 与固件 BL_APP_BASE 一致

function hex32(v) {
  return (v >>> 0).toString(16).toUpperCase().padStart(8, '0');
}

function fmtBytes(n) {
  if (n < 1024) return n + ' 字节';
  const kb = n / 1024;
  return kb < 1024 ? kb.toFixed(1) + ' KB' : (kb / 1024).toFixed(2) + ' MB';
}

/**
 * 解析 Intel HEX。
 *
 * @param {string} text        hex 文件全文
 * @param {object} [opts]
 * @param {number} [opts.base]    期望的镜像起始地址，默认 0x08004000
 * @param {number} [opts.maxSize] 镜像大小上限（字节），0 表示不检查
 * @returns {{data: Uint8Array, base: number, size: number,
 *            dataBytes: number, padded: number}}
 *          dataBytes = 文件里实有的数据字节；padded = 补的 0xFF 字节
 * @throws {Error} 格式错误、地址不符、超限 —— message 是可直接展示给用户的中文说明
 */
export function parseHex(text, opts = {}) {
  const wantBase = (opts.base === undefined ? HEX_DEFAULT_BASE : opts.base) >>> 0;
  const maxSize = opts.maxSize === undefined ? 0 : opts.maxSize;

  const lines = String(text).split(/\r?\n/);
  const segs = [];                     // { addr, bytes }
  let upper = 0;                       // 扩展地址高位（type 02 段基址 / type 04 线性基址）
  let sawEof = false;

  const fail = (msg, ln) => { throw new Error(ln ? `第 ${ln} 行：${msg}` : msg); };

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line || sawEof) continue;              // 空行、EOF 之后的内容都跳过
    if (line[0] !== ':') fail("记录必须以 ':' 开头", i + 1);

    const body = line.slice(1);
    if ((body.length & 1) !== 0 || body.length < 10) fail('记录长度非法', i + 1);

    const b = new Uint8Array(body.length >> 1);
    for (let k = 0; k < b.length; k++) {
      const v = parseInt(body.substr(k * 2, 2), 16);
      if (Number.isNaN(v)) fail('含非十六进制字符', i + 1);
      b[k] = v;
    }

    const len = b[0];                            // 本记录的数据字节数
    if (b.length !== len + 5) fail(`声明 ${len} 字节数据，与实际记录长度不符`, i + 1);

    let sum = 0;
    for (let k = 0; k < b.length; k++) sum = (sum + b[k]) & 0xFF;
    if (sum !== 0) fail('校验和错误（文件可能损坏或被截断）', i + 1);

    const type = b[3];
    const addr16 = (b[1] << 8) | b[2];           // 记录地址字段（大端）
    const val = (b[4] << 8) | b[5];              // 02 / 04 记录的数据字段（大端）

    if (type === 0x00) {
      segs.push({ addr: (upper + addr16) >>> 0, bytes: b.slice(4, 4 + len) });
    } else if (type === 0x01) {
      sawEof = true;
    } else if (type === 0x02) {
      if (len !== 2) fail('扩展段地址记录应为 2 字节', i + 1);
      upper = (val << 4) >>> 0;
    } else if (type === 0x04) {
      if (len !== 2) fail('扩展线性地址记录应为 2 字节', i + 1);
      upper = (val << 16) >>> 0;
    } else if (type === 0x03 || type === 0x05) {
      // 起始段地址 / 起始线性地址：只描述「从哪开始执行」，不影响烧写布局
    } else {
      fail(`未知的记录类型 0x${type.toString(16)}`, i + 1);
    }
  }

  if (!segs.length) throw new Error('文件里没有任何数据记录 —— 这不像一份 Intel HEX');

  segs.sort((a, b) => a.addr - b.addr);

  const lo = segs[0].addr;
  let hi = 0;
  for (const s of segs) hi = Math.max(hi, s.addr + s.bytes.length);

  if (lo !== wantBase) {
    const hint = lo < wantBase
      ? ' —— 这份 hex 可能把 Bootloader 也编进去了，请只编译 APP'
      : ` —— 请把 APP 工程的 IROM 起点设为 0x${hex32(wantBase)}`;
    throw new Error(`固件起始地址 0x${hex32(lo)}，`
      + `${lo < wantBase ? '低于' : '高于'} APP 区起点 0x${hex32(wantBase)}${hint}`);
  }

  const size = hi - lo;
  if (maxSize > 0 && size > maxSize) {
    throw new Error(`固件 ${fmtBytes(size)}，超过 APP 区上限 ${fmtBytes(maxSize)}`);
  }

  const data = new Uint8Array(size);
  data.fill(0xFF);                             // 空洞 = Flash 擦除态
  for (const s of segs) data.set(s.bytes, s.addr - lo);

  let dataBytes = 0;
  for (const s of segs) dataBytes += s.bytes.length;

  return { data, base: lo, size, dataBytes, padded: size - dataBytes };
}

/** 便于页面展示：把一个 hex 文件的解析结果说成一句人话。 */
export function describeHex(r) {
  const parts = [`HEX 起始 0x${hex32(r.base)}`, `镜像 ${fmtBytes(r.size)}`];
  if (r.padded > 0) parts.push(`其中空洞填充 0xFF ${fmtBytes(r.padded)}`);
  return parts.join('，');
}
