// 网页端 IAP 客户端逻辑测试：用一个「虚拟从机」把 Iap.send() 完整跑一遍。
//
// 浏览器里没法自动化（Web Serial 需要真人点授权），但协议时序是纯逻辑，
// 所以这里按 docs/PROTOCOL_DESIGN.md §0.5 的规则实现一个从机，与 Iap 对接。
// 测的是：START 的载荷、DATA 的逐帧校验与断点续传、END 的提交、以及重试。
//
// 运行：node tools/test_iap_sim.mjs

import {
  Iap, Parser, Crc32, crc32, encode,
  getLe16, getLe32, putLe16, putLe32,
  OVERHEAD, DIR_REPLY, CODE_NAME,
  CMD_START, CMD_DATA, CMD_END,
  START_LEN, DATA_HEAD,
} from '../docs/js/protocol.js';

let cases = 0;
const fails = [];

function check(ok, what) {
  cases++;
  if (!ok) fails.push(what);
}

const APP_BASE = 0x08004000;
const BLOCK = 512;

// ------------------------------------------------------------ 虚拟从机

class FakeDevice {
  constructor(opts = {}) {
    this.id = opts.id ?? 1;
    this.appBase = APP_BASE;
    this.blockSize = BLOCK;
    this.parser = new Parser();
    this.state = 'idle';
    this.mem = new Map();                    // offset（相对数据流起点）→ 字节
    this.declaredSize = 0;
    this.declaredCrc = 0;
    this.next = 0;
    this.lastReply = null;                   // 上次成功应答，用于幂等重发
    this.dropped = opts.dropReplyForFrame;   // 「丢一次应答」的帧号（测重试）
    this.droppedCount = 0;
  }

  /** 主机 → 从机。返回要回给主机的字节（可能多条应答拼在一起）。 */
  write(bytes) {
    const out = [];
    for (const b of bytes) {
      const f = this.parser.feed(b);
      if (f) {
        const r = this.handle(f);
        if (r) out.push(...r);
      }
    }
    return out;
  }

  reply(cmd, code, extra = null) {
    const body = [code, ...(extra || [])];
    return [...encode(this.id, cmd | DIR_REPLY, new Uint8Array(body))];
  }

  handle(f) {
    if (f.reply) return null;
    if (f.id !== this.id) return null;

    switch (f.cmd & ~DIR_REPLY) {
      case CMD_START: return this.onStart(f);
      case CMD_DATA: return this.onData(f);
      case CMD_END: return this.onEnd(f);
      default: return null;
    }
  }

  onStart(f) {
    if (f.data.length < START_LEN) return this.reply(CMD_START, 0x05);
    const size = getLe32(f.data, 0);
    const crc = getLe32(f.data, 4);
    if (size < 16) return this.reply(CMD_START, 0x05);
    this.declaredSize = size;
    this.declaredCrc = crc;
    this.next = 0;
    this.mem.clear();
    this.state = 'receiving';
    const extra = [...putLe16(this.blockSize)];
    return this.reply(CMD_START, 0x00, extra);       // OK + blockSize
  }

  onData(f) {
    if (this.state !== 'receiving') return this.reply(CMD_DATA, 0x06);
    if (f.data.length < DATA_HEAD) return this.reply(CMD_DATA, 0x05);

    const addr = getLe32(f.data, 0);
    const total = getLe16(f.data, 4);
    const index = getLe16(f.data, 6);
    const vlen = getLe16(f.data, 8);
    const cum = getLe32(f.data, 10);
    const payload = f.data.subarray(DATA_HEAD);

    const expectAddr = this.appBase + 8 + this.next;
    if (vlen !== payload.length || vlen === 0) return this.reply(CMD_DATA, 0x0e);
    if (index !== this.next / this.blockSize && addr >= expectAddr) {
      return this.reply(CMD_DATA, 0x0e);
    }
    if (addr < expectAddr) {
      // 重复帧（上次应答丢了）：不重写，重发上次应答 —— 与真实固件一致
      return this.lastReply ? [...this.lastReply] : null;
    }
    if (addr > expectAddr) {
      // 跳号：回期望地址（与其它 DATA 应答同格式：cumCrc32 + nextAddr）
      const c0 = new Crc32();
      for (let off = 0; off < this.next; off += this.blockSize) c0.update(this.mem.get(off));
      return this.reply(CMD_DATA, 0x07,
        [...putLe32(c0.value()), ...putLe32(expectAddr)]);
    }
    void total;

    // 累积 CRC32：只对已写入字节累加，且必须是「从数据流起点」的累积值
    const c = new Crc32();
    for (let off = 0; off < this.next; off += this.blockSize) {
      c.update(this.mem.get(off));
    }
    c.update(payload);
    if (c.value() !== cum) return this.reply(CMD_DATA, 0x08, [...putLe32(c.value()), ...putLe32(expectAddr)]);

    // 「丢一次应答」：数据照写、应答照生成并记入 lastReply，只是不发出去
    // —— 模拟的是「应答在回程链路上丢了」，而不是「从机没处理」。
    // 主机随后超时重传同一帧，从机看到 addr < expectAddr 就重发 lastReply（幂等）。
    if (this.dropped === index && this.droppedCount === 0) {
      this.droppedCount++;
      this.mem.set(this.next, payload);
      this.next += payload.length;
      this.lastReply = this.reply(CMD_DATA, 0x00,
        [...putLe32(c.value()), ...putLe32(this.appBase + 8 + this.next)]);
      return null;                                // 故意不发
    }

    this.mem.set(this.next, payload);
    this.next += payload.length;
    const ok = this.reply(CMD_DATA, 0x00,
      [...putLe32(c.value()), ...putLe32(this.appBase + 8 + this.next)]);
    this.lastReply = ok;                     // 供幂等重发
    return ok;
  }

  onEnd() {
    if (this.state !== 'receiving') return this.reply(CMD_END, 0x06);

    const c = new Crc32();
    for (let off = 0; off < this.next; off += this.blockSize) c.update(this.mem.get(off));
    if (c.value() !== this.declaredCrc) return this.reply(CMD_END, 0x0d);
    if ((this.next + 8) !== this.declaredSize) return this.reply(CMD_END, 0x0b);

    this.state = 'idle';
    return this.reply(CMD_END, 0x00, [...putLe32(this.declaredSize)]);
  }

  /** 从机内存里的数据流（按 offset 拼起来） */
  readAll() {
    const parts = [];
    for (let off = 0; off < this.next; off += this.blockSize) parts.push(this.mem.get(off));
    const n = parts.reduce((a, p) => a + p.length, 0);
    const out = new Uint8Array(n);
    let k = 0;
    for (const p of parts) { out.set(p, k); k += p.length; }
    return out;
  }
}

// ------------------------------------------------------------ 测试脚手架

function makeImage(payloadLen) {
  const raw = new Uint8Array(8 + payloadLen);
  // SP 落在 SRAM 且 8 字节对齐；PC 落在 APP 区内且 Thumb 位置 1
  raw.set(putLe32(0x20018000), 0);
  raw.set(putLe32(APP_BASE + 0x101), 4);
  for (let i = 0; i < payloadLen; i++) raw[8 + i] = (i * 31 + 7) & 0xff;
  return raw;
}

async function runSend(label, raw, dev, opts = {}) {
  let iap;
  const logs = [];
  const io = {
    async write(bytes) {
      const replies = dev.write(bytes);          // 扁平字节数组（可能含多条应答）
      if (replies.length) iap.feed(new Uint8Array(replies));
      await io.pump(0);
    },
    async pump() { /* 模拟环境不需要真等 */ },
  };
  const steps = [];                            // 步骤行：记录它有没有被结算
  iap = new Iap(io, {
    deviceId: 1,
    blockSize: BLOCK,
    frameTimeout: 200,
    startTimeout: 500,
    endTimeout: 500,
    retries: 5,
    // 假 onLog：返回一个句柄，用来记录「步骤有没有被结算」。
    // 真页面里这个句柄对应一行 DOM（见 docs/js/app.js 的 log()）——
    // 步骤行不结算的话，页面上会留一个永远挂着省略号的壳。
    onLog: (m, kind = 'info') => {
      logs.push(m);
      if (process.env.SIM_VERBOSE) console.log('      ' + m);
      if (kind !== 'run') return undefined;
      const rec = { text: m, mark: null, note: '' };
      steps.push(rec);
      return {
        settle(mark, note) { rec.mark = mark; rec.note = note || ''; },
      };
    },
  });
  const r = await iap.send(raw, { name: label });
  return { r, logs, steps, iap };
}

// ------------------------------------------------------------ 用例

async function testNormal() {
  console.log('[1] 正常升级（3 帧）');
  const raw = makeImage(1200);                        // 1200 → 3 帧（512+512+176）
  const dev = new FakeDevice();
  const { r } = await runSend('normal', raw, dev);

  check(r.ok === true, '升级成功');
  check(r.frames === 3, `帧数 3（实际 ${r.frames}）`);
  check(r.written === raw.length, `板端确认 ${raw.length} 字节（实际 ${r.written}）`);
  const got = dev.readAll();
  check(got.length === raw.length - 8, `落盘字节数 ${raw.length - 8}（实际 ${got.length}）`);
  let same = got.length === raw.length - 8;
  for (let i = 0; same && i < got.length; i++) same = got[i] === raw[8 + i];
  check(same, '落盘内容与镜像 [8, size) 逐字节一致');
}

async function testRetryAfterDroppedReply() {
  console.log('[2] 第 1 帧的应答被丢掉 → 主机重传，从机幂等重发');
  const raw = makeImage(1200);
  const dev = new FakeDevice({ dropReplyForFrame: 1 });
  const { r, logs } = await runSend('retry', raw, dev);

  check(r.ok === true, '升级仍成功');
  check(dev.droppedCount === 1, '确实丢过一次应答');
  check(logs.some((m) => m.includes('重试')), '主机日志里出现重试');
  const got = dev.readAll();
  check(got.length === raw.length - 8, '落盘字节数正确（重传没有重复写入）');
}

async function testBadFirstFrame() {
  console.log('[3] 累积 CRC32 不符的帧被拒（改一个数据字节）');
  const raw = makeImage(1200);
  const dev = new FakeDevice();
  let iap;
  let corrupted = false;
  const io = {
    async write(bytes) {
      let b = bytes;
      // 篡改第 1 个 DATA 帧里的一个数据字节（只在第一次发它时改）
      // DATA 帧内偏移：0=A5 1=id 2=cmd 3..4=len 5..8=addr 9..10=total
      //                 11..12=index 13..14=vlen 15..18=cum 19+=data
      if (!corrupted && bytes.length > DATA_HEAD + 5
          && (bytes[2] & ~DIR_REPLY) === CMD_DATA && bytes[11] === 0 && bytes[12] === 0) {
        b = Uint8Array.from(bytes);
        b[DATA_HEAD + 10] ^= 0xff;
        corrupted = true;
      }
      const replies = dev.write(b);
      if (replies.length) iap.feed(new Uint8Array(replies));
      await io.pump(0);
    },
    async pump() {},
  };
  iap = new Iap(io, { deviceId: 1, blockSize: BLOCK, frameTimeout: 200,
                      startTimeout: 500, endTimeout: 500, retries: 5,
                      onLog: () => {} });
  const r = await iap.send(raw, { name: 'corrupt' });

  check(corrupted === true, '确实篡改过一帧');
  check(r.ok === true, '主机从该帧续传后整片仍完成');
  const got = dev.readAll();
  let same = got.length === raw.length - 8;
  for (let i = 0; same && i < got.length; i++) same = got[i] === raw[8 + i];
  check(same, '最终落盘内容正确（坏帧没被写进去）');
}

async function testStepsAreSettled() {
  console.log('[4] 步骤行都被结算（不留悬挂的省略号）');
  const raw = makeImage(1200);

  // 正常路径：板子照常回应 → START / DATA / END 三个步骤都该结算成 ✔ 并带上结果
  const good = await runSend('steps-ok', raw, new FakeDevice());
  check(good.r.ok === true, '正常路径升级成功');
  check(good.steps.length >= 3, `起了 ${good.steps.length} 个步骤行（START / DATA / END）`);
  check(good.steps.every((s) => s.mark === '✔'),
    '每个步骤都被结算为 ✔（页面上不会留挂着省略号的壳）');
  check(good.steps.every((s) => s.note.length > 0),
    '每个 ✔ 都带结果（blockSize / 帧数 / written）');

  // 失败路径：板子完全不应答 → 也必须结算，否则步骤行永远停在「进行中」
  const silent = { write: () => [], readAll: () => new Uint8Array(0) };
  const bad = await runSend('steps-silent', raw, silent);
  check(bad.r.ok === false, '板子不应答 → 升级失败');
  check(bad.steps.length === 1, `只起了 START 一个步骤行（实际 ${bad.steps.length}）`);
  check(bad.steps[0].mark === '✗', '失败路径下 START 也被结算为 ✗（不是 null）');
}

async function main() {
  console.log('=== 网页端 IAP 逻辑测试（虚拟从机）===\n');
  // 卡死保护：逻辑错成死循环时不要一直挂着
  const guard = setTimeout(() => { console.log('\n★ 超时（15s）—— 疑似死循环'); process.exit(2); }, 15000);
  guard.unref();
  await testNormal();
  await testRetryAfterDroppedReply();
  await testBadFirstFrame();
  await testStepsAreSettled();

  console.log(`\n${cases} 项断言，${fails.length} 项失败 → ${fails.length ? 'FAIL' : 'PASS'}`);
  for (const f of fails) console.log('  ✘ ' + f);
  process.exitCode = fails.length ? 1 : 0;
}

main();
void OVERHEAD;
void crc32;
void CODE_NAME;
