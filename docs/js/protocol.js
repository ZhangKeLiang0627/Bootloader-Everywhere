/**
 * 载体层 + IAP 客户端（浏览器与 Node 通用）。
 *
 * 与固件的 Bootloader/protocol.cpp、上位机 TestApp/tools/proto.py 是同一套帧格式与规则
 * （见 docs/PROTOCOL_DESIGN.md §0.1 / §0.5）。三处都用同一组测试向量自检：
 *
 *     node tools/test_protocol_js.mjs
 *
 *   [0xA5] [ID] [CMD] [len:2 LE] [Data ≤1024] [CRC8] [0x03]      共 7 + N 字节
 *
 *   CMD 的 bit7 = 方向位（0 主→从，1 从→主），bit0-6 = 命令码
 *   CRC8 覆盖 [ID .. Data 末尾]，不含帧头与帧尾
 */

// ---------------------------------------------------------------- 常量

export const HEAD = 0xA5;
export const TAIL = 0x03;
export const DIR_REPLY = 0x80;

export const DATA_MAX = 1024;
export const OVERHEAD = 7;                       // 头1 + ID1 + CMD1 + len2 + CRC1 + 尾1
export const FRAME_MIN = OVERHEAD;               // 空载荷帧（END / STATUS）
export const FRAME_MAX = OVERHEAD + DATA_MAX;

export const CMD_START = 0x01;
export const CMD_DATA = 0x02;
export const CMD_END = 0x03;
export const CMD_STATUS = 0x04;

export const START_LEN = 16;                     // size4 + crc32_4 + sp4 + pc4
export const DATA_HEAD = 14;                     // addr4 + total2 + index2 + vlen2 + cumCrc32_4

export const CODE_NAME = {
  0x00: 'OK',
  0x01: '帧 CRC8 错',
  0x02: '长度非法',
  0x03: 'ID 或地址不匹配',
  0x04: '未知命令',
  0x05: '参数非法',
  0x06: '状态错误',
  0x07: '地址不连续',
  0x08: '累积 CRC32 不符',
  0x09: '擦除失败',
  0x0a: '写入失败',
  0x0b: '回读校验失败',
  0x0c: '向量表非法',
  0x0d: '总 CRC32 不符',
  0x0e: '冗余字段不一致',
  0x0f: '回读与写入不一致',
};

// ---------------------------------------------------------------- CRC8

/**
 * CRC-8/SMBUS：poly 0x07 / init 0x00 / MSB-first（不反射）/ xorout 0x00。
 *
 * 注意与 CRC32（LSB-first）位序方向相反 —— 这是两端最容易写错的地方，
 * 测试向量里的 0x12 / 0xCBF43926 就是用来抓这个错的。
 */
export function crc8(data) {
  let c = 0;
  for (let i = 0; i < data.length; i++) {
    c ^= data[i];
    for (let k = 0; k < 8; k++) {
      c = (c & 0x80) ? ((c << 1) ^ 0x07) & 0xff : (c << 1) & 0xff;
    }
  }
  return c & 0xff;
}

// ---------------------------------------------------------------- CRC32

const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    t[i] = c >>> 0;
  }
  return t;
})();

/**
 * CRC32 / ISO-HDLC，表驱动 + 可增量。
 *
 * 必须增量累计：DATA 每帧都要给出「从数据流起点到本帧末尾」的累积值，
 * 若每帧从头重算就是 O(n²)（200KB / 400 帧 ≈ 40MB 的重复计算）。
 */
export class Crc32 {
  constructor() {
    this.v = 0xffffffff;
  }

  reset() {
    this.v = 0xffffffff;
    return this;
  }

  clone() {
    const c = new Crc32();
    c.v = this.v;
    return c;
  }

  update(data) {
    let c = this.v;
    for (let i = 0; i < data.length; i++) {
      c = CRC32_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
    }
    this.v = c >>> 0;
    return this;
  }

  value() {
    return (this.v ^ 0xffffffff) >>> 0;
  }
}

export function crc32(data) {
  return new Crc32().update(data).value();
}

// ---------------------------------------------------------------- 组帧

/** 组一帧。data 可为 null（无载荷）。 */
export function encode(id, cmd, data = null) {
  const n = data ? data.length : 0;
  const out = new Uint8Array(OVERHEAD + n);

  out[0] = HEAD;
  out[1] = id;
  out[2] = cmd;
  out[3] = n & 0xff;
  out[4] = (n >>> 8) & 0xff;
  if (n) out.set(data, 5);

  out[5 + n] = crc8(out.subarray(1, 5 + n));
  out[6 + n] = TAIL;
  return out;
}

// ---------------------------------------------------------------- 解析

/**
 * 流式解析：逐字节喂入，凑齐一帧返回对象，否则 null。
 *
 * 校验不过会退回扫描帧头并重新检查当前字节（它可能就是新的帧头）。
 * 返回的帧长保证落在 [FRAME_MIN, FRAME_MAX]（一帧到来时的长度自检）。
 */
export class Parser {
  constructor() {
    this.reset();
  }

  reset() {
    this.st = 'head';
    this.crc = 0;
    this.buf = [];
    this.id = 0;
    this.cmd = 0;
    this.n = 0;
    return this;
  }

  feed(byte) {
    // 每个字节最多被判两次（一次按原状态，一次作为可能的新帧头）
    for (let guard = 0; guard < 3; guard++) {
      switch (this.st) {
        case 'head':
          if (byte !== HEAD) return null;
          this.crc = 0;
          this.buf = [];
          this.st = 'id';
          return null;
        case 'id':
          this.id = byte;
          this.crc = step(this.crc, byte);
          this.st = 'cmd';
          return null;
        case 'cmd':
          this.cmd = byte;
          this.crc = step(this.crc, byte);
          this.st = 'lenlo';
          return null;
        case 'lenlo':
          this.n = byte;
          this.crc = step(this.crc, byte);
          this.st = 'lenhi';
          return null;
        case 'lenhi':
          this.n |= byte << 8;
          this.crc = step(this.crc, byte);
          if (this.n > DATA_MAX) {
            this.st = 'head';                 // 长度非法：丢弃整帧（不等收完）
            continue;
          }
          this.st = this.n === 0 ? 'crc' : 'data';
          return null;
        case 'data':
          this.buf.push(byte);
          this.crc = step(this.crc, byte);
          if (this.buf.length >= this.n) this.st = 'crc';
          return null;
        case 'crc':
          if (byte !== this.crc) {
            this.st = 'head';                 // CRC 错：整帧丢弃，重新找帧头
            continue;
          }
          this.st = 'tail';
          return null;
        default: {                            // tail
          this.st = 'head';
          if (byte !== TAIL) continue;
          const total = OVERHEAD + this.n;
          if (total < FRAME_MIN || total > FRAME_MAX) {
            return null;                      // 长度越界：丢帧，让主机重传
          }
          return {
            id: this.id,
            cmd: this.cmd,
            code: this.cmd & ~DIR_REPLY,
            reply: (this.cmd & DIR_REPLY) !== 0,
            data: new Uint8Array(this.buf),
            total,
          };
        }
      }
    }
    return null;
  }
}

function step(crc, byte) {
  crc ^= byte;
  for (let k = 0; k < 8; k++) {
    crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
  }
  return crc;
}

// ---------------------------------------------------------------- 小端读写

export function getLe16(p, off) {
  return (p[off] | (p[off + 1] << 8)) >>> 0;
}

export function getLe32(p, off) {
  return (p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (p[off + 3] << 24)) >>> 0;
}

export function putLe16(v) {
  return new Uint8Array([v & 0xff, (v >>> 8) & 0xff]);
}

export function putLe32(v) {
  return new Uint8Array([v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]);
}

// ---------------------------------------------------------------- IAP 客户端

/** 一次升级用到的参数（与 proto.py 的默认值一致）。 */
export const DEFAULTS = {
  deviceId: 1,
  blockSize: 512,
  appBase: 0x08004000,
  startTimeout: 12000,
  frameTimeout: 500,
  endTimeout: 5000,
  retries: 5,
  recallTimeout: 300,
};

/**
 * 主机侧 IAP 流程：唤回 → START → DATA（逐帧一应一答）→ END。
 *
 * io 需提供：
 *   write(Uint8Array) → Promise    写串口
 *   pump(ms) → Promise             让出事件循环（浏览器里给 reader 循环机会推进数据）
 */
export class Iap {
  constructor(io, opts = {}) {
    this.io = io;
    this.cfg = { ...DEFAULTS, ...opts };
    this.parser = new Parser();
    this.waiter = null;               // { cmd, finish }
    this.stats = { frames: 0, retries: 0, bytes: 0 };
    this.onLog = opts.onLog || (() => {});
    this.onProgress = opts.onProgress || (() => {});
  }

  /** 是否正在等应答（决定串口字节该走协议还是当文本日志显示）。 */
  expectsReply() {
    return this.waiter !== null;
  }

  /** 串口读循环把收到的字节喂进来。 */
  feed(bytes) {
    for (let i = 0; i < bytes.length; i++) {
      const f = this.parser.feed(bytes[i]);
      if (f) this._onFrame(f);
    }
  }

  _onFrame(f) {
    if (!f.reply || f.id !== this.cfg.deviceId) return;   // 只关心发给本机地址的应答
    const w = this.waiter;
    if (!w) return;                                       // 没人在等：噪声，丢弃
    if (w.cmd !== 0 && f.cmd !== w.cmd) return;           // 不是等的那个命令
    w.finish(f);
  }

  /** 发一条命令并等应答；帧级重试。返回应答帧或 null。 */
  async request(cmd, data, timeoutMs, retries, label) {
    for (let attempt = 1; attempt <= retries; attempt++) {
      const reply = await this._sendAndWait(cmd, data, timeoutMs);
      if (reply) return reply;
      if (attempt < retries) {
        this.stats.retries++;
        this.onLog(`${label} 无应答，重试 ${attempt}/${retries}`, 'warn');
      }
    }
    return null;
  }

  _sendAndWait(cmd, data, timeoutMs) {
    return new Promise((resolve) => {
      let done = false;
      const finish = (v) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        this.waiter = null;
        resolve(v);
      };

      // 发之前丢半截帧：上一次超时留下的残片不能算进这次的应答
      this.parser.reset();
      this.waiter = { cmd: cmd | DIR_REPLY, finish };
      const timer = setTimeout(() => finish(null), timeoutMs);

      this.io.write(encode(this.cfg.deviceId, cmd, data))
        .catch((e) => { this.onLog('写串口失败：' + e.message, 'err'); finish(null); });
    });
  }

  /**
   * 唤回 Bootloader：发关键字，APP 收到后软复位，Bootloader 进 15s 限时窗口。
   *
   * 必须逐字节慢发 —— APP 主循环 100ms 才轮询一次串口，一口气连发会因
   * overrun 丢掉大部分字节，状态机凑不齐关键字。
   */
  async recall() {
    this.onLog('唤回 Bootloader …', 'step');
    const magic = '#Bootloader-Everywhere';
    for (const ch of magic) {
      try {
        await this.io.write(new Uint8Array([ch.charCodeAt(0)]));
      } catch (_) {
        break;
      }
      await this.io.pump(150);
    }
    await this.io.pump(this.cfg.recallTimeout);
  }

  /** 把数据流（镜像 [8, size)）写进 APP 区。 */
  async send(raw, opt = {}) {
    const appBase = this.cfg.appBase;
    const blockSize = this.cfg.blockSize;

    if (raw.length < 16) {
      this.onLog('固件太小（至少 16 字节）', 'err');
      return { ok: false, reason: 'size' };
    }

    const sp = getLe32(raw, 0);
    const pc = getLe32(raw, 4);
    const body = raw.subarray(8);
    const totalPkts = Math.ceil(body.length / blockSize);
    const totalCrc = crc32(body);

    this.onLog(`固件 ${opt.name || ''}：${raw.length} 字节  SP=0x${hex8(sp)} PC=0x${hex8(pc)}`);
    this.onLog(`数据 ${body.length} 字节 / ${totalPkts} 帧 / 整片 CRC32 = 0x${hex8(totalCrc)}`);

    // ---- START ----
    this.onLog('START（板子擦除中，可能数秒）…', 'step');
    const t0 = now();
    const startPayload = new Uint8Array(16);
    startPayload.set(putLe32(raw.length), 0);
    startPayload.set(putLe32(totalCrc), 4);
    startPayload.set(putLe32(sp), 8);
    startPayload.set(putLe32(pc), 12);

    let f = await this.request(CMD_START, startPayload, this.cfg.startTimeout, 2, 'START');
    if (!f) {
      this.onLog('START 无应答 —— 设备没在 IAP？按住按钮上电重试', 'err');
      return { ok: false, reason: 'start-no-reply' };
    }
    let code = f.data[0];
    const blk = f.data.length >= 3 ? getLe16(f.data, 1) : 0;
    this.onLog(`  ← ${codeName(code)}  blockSize=${blk}  (${((now() - t0) / 1000).toFixed(1)} s)`,
               code === 0 ? 'ok' : 'err');
    if (code !== 0) return { ok: false, reason: 'start', code };

    // ---- DATA ----
    // 从机已确认的字节数与对应的累积 CRC32（增量维护，不从头重算）
    let acked = 0;
    let ackedCrc = new Crc32();
    let offset = 0;
    let consecutiveFail = 0;
    let lastOffset = -1;
    let stuck = 0;
    const t1 = now();

    while (offset < body.length) {
      // 卡死保护：从机可能持续报同一个错、或报一个无效的期望地址，
      // 这时 offset 一直不推进 —— 必须在同一个位置上停下来，不能无限重试。
      if (offset === lastOffset) {
        if (++stuck > this.cfg.retries) {
          this.onLog(`位置 ${offset} 连续 ${stuck} 次无法推进，中止`, 'err');
          return { ok: false, reason: 'stuck', offset };
        }
      } else {
        stuck = 0;
        lastOffset = offset;
      }

      const data = body.subarray(offset, offset + blockSize);
      const addr = appBase + 8 + offset;
      const end = offset + data.length;

      const c = ackedCrc.clone().update(body.subarray(acked, end));
      const cum = c.value();

      const payload = new Uint8Array(DATA_HEAD + data.length);
      payload.set(putLe32(addr), 0);
      payload.set(putLe16(totalPkts), 4);
      payload.set(putLe16(offset / blockSize), 6);
      payload.set(putLe16(data.length), 8);
      payload.set(putLe32(cum), 10);
      payload.set(data, DATA_HEAD);

      f = await this.request(CMD_DATA, payload, this.cfg.frameTimeout, this.cfg.retries,
                             `DATA#${offset / blockSize}`);
      if (!f) {
        if (++consecutiveFail > this.cfg.retries) {
          this.onLog(`第 ${offset / blockSize} 帧连续失败，中止`, 'err');
          return { ok: false, reason: 'data-timeout' };
        }
        continue;
      }
      consecutiveFail = 0;

      code = f.data[0];
      // 所有 DATA 应答都是同一格式：[code][cumCrc32:4][nextAddr:4]
      // （含 0x07 跳号 —— 它只是 cumCrc32 无意义，地址字段照给）
      const devCrc = f.data.length >= 9 ? getLe32(f.data, 1) : 0;
      const expect = f.data.length >= 9 ? getLe32(f.data, 5) : 0;

      if (code !== 0) {
        this.onLog(`  帧 ${offset / blockSize} ← ${codeName(code)}（期望地址 0x${hex8(expect)}），从该处续传`,
                   'warn');
        if (expect >= appBase + 8 && expect < appBase + 8 + body.length) {
          offset = expect - appBase - 8;       // 断点续传
        }
        continue;
      }

      if (devCrc !== cum) {
        this.onLog(`  帧 ${offset / blockSize} 板端读回 CRC32 = 0x${hex8(devCrc)}，与主机 0x${hex8(cum)} 不符`,
                   'err');
        return { ok: false, reason: 'readback' };
      }

      ackedCrc = c;                            // 只有确认写入才推进
      acked = end;
      offset = expect - appBase - 8;           // 从机报的期望地址 = 下一帧起点
      this.stats.frames++;
      this.stats.bytes = offset;
      this.onProgress(offset, body.length);
    }

    this.onLog(`数据传输完成，用时 ${((now() - t1) / 1000).toFixed(1)} 秒`, 'ok');

    // ---- END ----
    this.onLog('END（回读校验 + 提交）…', 'step');
    f = await this.request(CMD_END, null, this.cfg.endTimeout, 2, 'END');
    if (!f) {
      this.onLog('END 无应答', 'err');
      return { ok: false, reason: 'end-no-reply' };
    }
    code = f.data[0];
    const written = f.data.length >= 5 ? getLe32(f.data, 1) : 0;
    this.onLog(`  ← ${codeName(code)}  written=${written}`, code === 0 ? 'ok' : 'err');
    if (code !== 0) return { ok: false, reason: 'end', code };

    return { ok: true, written, frames: this.stats.frames, retries: this.stats.retries };
  }
}

function codeName(code) {
  return CODE_NAME[code] !== undefined ? CODE_NAME[code] : `未知 0x${code.toString(16)}`;
}

function now() {
  return (typeof performance !== 'undefined' && performance.now)
    ? performance.now() : Date.now();
}

function hex8(v) {
  return (v >>> 0).toString(16).toUpperCase().padStart(8, '0');
}

// ---------------------------------------------------------------- 自检

/**
 * 与固件 selfTest()、tools/test_protocol.cpp、proto.py selftest 完全相同的向量。
 * 三处都过，才算「三份实现的 CRC 变体与字节序一致」。
 */
export function selftest() {
  const fails = [];
  let cases = 0;

  const eq = (got, want, what) => {
    cases++;
    const g = (got && got.length !== undefined && typeof got !== 'string')
      ? Array.from(got) : got;
    const w = (want && want.length !== undefined && typeof want !== 'string')
      ? Array.from(want) : want;
    if (JSON.stringify(g) !== JSON.stringify(w)) {
      fails.push(`${what}: got ${JSON.stringify(g)} want ${JSON.stringify(w)}`);
    }
  };

  // CRC8 向量
  eq(crc8(new Uint8Array(0)), 0x00, 'crc8(空)');
  eq(crc8(new Uint8Array([1, 2, 2, 0, 0x34, 0x12])), 0x12, 'crc8(示例载荷)');
  eq(crc8(new Uint8Array([0])), 0x00, 'crc8(00)');
  eq(crc8(new Uint8Array([0xa5, 0x03])), 0x50, 'crc8(A5 03)');
  eq(crc8(new Uint8Array(Array.from({ length: 256 }, (_, i) => i))), 0x14, 'crc8(00..FF)');
  const ascii = new TextEncoder().encode('123456789');
  eq(crc8(ascii), 0xf4, 'crc8("123456789")');

  // CRC32 向量
  eq(crc32(ascii), 0xcbf43926, 'crc32("123456789")');
  eq(crc32(new Uint8Array(0)), 0x00000000, 'crc32(空)');
  eq(crc32(new Uint8Array([1, 2, 3])), 0x55bc801d, 'crc32(01 02 03)');

  // 增量与一次性一致
  const inc = new Crc32().update(ascii.subarray(0, 4)).update(ascii.subarray(4));
  eq(inc.value(), 0xcbf43926, '增量 CRC32 与一次性一致');

  // 组帧
  eq(encode(0x01, 0x02, new Uint8Array([0x34, 0x12])),
     new Uint8Array([0xa5, 0x01, 0x02, 0x02, 0x00, 0x34, 0x12, 0x12, 0x03]), '示例帧字节');
  eq(encode(0x01, 0x04).length, OVERHEAD, '空载荷帧长 = 7');
  eq(FRAME_MIN, 7, 'FRAME_MIN');
  eq(FRAME_MAX, 1031, 'FRAME_MAX');

  // 解析 + 垃圾前缀重同步（与 C / Python 侧同一组前缀）
  const good = encode(0x01, 0x02, new Uint8Array([0x34, 0x12]));
  const junk = new Uint8Array([0x00, 0xa5, 0xff, 0xa5, 0xa5]);
  let p = new Parser();
  let got = null;
  for (const b of [...junk, ...good]) {
    const r = p.feed(b);
    if (r) got = r;
  }
  eq(got ? got.data : null, new Uint8Array([0x34, 0x12]), '垃圾字节后重同步');
  eq(got ? got.total : 0, OVERHEAD + 2, '帧总长');

  // CRC 破坏 → 拒绝，紧跟着的好帧仍能解析
  const bad = Uint8Array.from(good);
  bad[3] ^= 0x01;
  p = new Parser();
  eq([...bad].map((b) => p.feed(b)).some(Boolean), false, 'CRC 不匹配被拒绝');
  eq([...good].map((b) => p.feed(b)).some(Boolean), true, '坏帧后好帧可解析');

  // 满载荷 + 载荷含 A5/03
  const big = new Uint8Array(1024);
  for (let i = 0; i < big.length; i += 2) {
    big[i] = 0xa5;
    big[i + 1] = 0x03;
  }
  const f2 = encode(0x01, 0x03, big);
  eq(f2.length, FRAME_MAX, '满载荷帧长 1031');
  p = new Parser();
  got = null;
  for (const b of f2) {
    const r = p.feed(b);
    if (r) got = r;
  }
  eq(got ? got.data : null, big, '载荷含 A5/03 可解析');

  // 帧长自检：len 被改大到越界的帧应被丢弃
  const over = Uint8Array.from(encode(0x01, 0x02, new Uint8Array(8)));
  over[3] = 0xff;
  over[4] = 0x7f;                            // len = 0x7FFF
  p = new Parser();
  eq([...over].map((b) => p.feed(b)).some(Boolean), false, 'len 越界的帧被丢弃');

  // len 越界但 CRC 自洽：唯一拦截点就是长度自检
  const crafted = new Uint8Array([0xa5, 0x01, 0x02, 0xff, 0x04, 0, 0x03]);
  crafted[5] = crc8(crafted.subarray(1, 5)); // 让 CRC 自洽
  p = new Parser();
  eq([...crafted].map((b) => p.feed(b)).some(Boolean), false, 'len 越界但 CRC 自洽的帧仍被丢弃');

  return { cases, fails };
}
