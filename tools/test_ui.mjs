/**
 * 网页端冒烟测试 —— headless Chrome + CDP，**不需要 Web Serial**
 *
 *   node tools/test_ui.mjs [--url http://127.0.0.1:8090] [--shot out.png]
 *
 * 覆盖「不用点连接也能验」的全部逻辑：
 *   ① 页面加载无 JS 报错、载体层自检通过
 *   ② 步进器：+/− 与 1..127 钳位、手输非法值回正
 *   ③ 选中 .hex → 解析成功、按大小启用「开始升级」
 *   ④ .hex 起始地址不对 → 被拒、按钮保持禁用、错误写进框里
 *   ⑤ 拖入 .txt → 提示不支持
 *   ⑥ 选中 .bin → 也可用（大小检查生效）
 *
 * 为什么要它：页面里真正需要「真人 + Web Serial」的只有点连接之后的串口交互；
 * 其余（解析、校验、表单状态）都能无头验证，不该每次都靠人眼。
 * 最后顺手截一张整页图，外观那一项交给人看。
 */

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');

const argv = process.argv.slice(2);
const argOf = (name, dflt) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
};
const URL_BASE = argOf('--url', 'http://127.0.0.1:8090');
const SHOT = argOf('--shot', '');
const SHOT_EMPTY = argv.includes('--shot-empty');   // 截图前刷新回初始态（用于「改前/改后」对比）
const PORT = parseInt(argOf('--port', '0'), 10);   // 0 = 自动挑一个空闲端口

const CHROME_CANDIDATES = [
  process.env.CHROME_PATH,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);

let pass = 0, fail = 0;
const ok = (cond, what) => {
  if (cond) { pass++; console.log('  ok   ' + what); }
  else { fail++; console.log('  FAIL ' + what); }
};

// ------------------------------------------------------------------ 测试素材

const APP = 0x08004000;

/** 独立构造一个 hex（本文件自带编码器，不复用被测代码） */
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

/** 造一个「像样」的镜像：合法向量表 + 递增数据（不是全 0xFF，才能看出空洞填充） */
function fakeImage(size) {
  const d = new Uint8Array(size);
  for (let i = 0; i < size; i++) d[i] = i & 0xFF;
  const sp = 0x20018000, pc = APP + 0x201;          // 合法：SRAM 内 / 8 字节对齐 / Thumb
  d[0] = sp & 0xFF; d[1] = (sp >> 8) & 0xFF; d[2] = (sp >> 16) & 0xFF; d[3] = (sp >> 24) & 0xFF;
  d[4] = pc & 0xFF; d[5] = (pc >> 8) & 0xFF; d[6] = (pc >> 16) & 0xFF; d[7] = (pc >> 24) & 0xFF;
  return d;
}

const GOOD_IMG = fakeImage(2124);
const GOOD_HEX = makeHex(APP, GOOD_IMG);
const GOOD_BIN = Buffer.from(GOOD_IMG);

const BAD_HEX = [                                  // 起始 0x08000000 —— 会盖到 Bootloader
  ':020000040800F2',
  ':10000000000102030405060708090A0B0C0D0E0F78',
  ':00000001FF',
].join('\n');

const SMALL_HEX = makeHex(APP, fakeImage(8));      // 8 字节：小于最小镜像

// ------------------------------------------------------------------ CDP 客户端

class Cdp {
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    ws.addEventListener('message', (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id && this.pending.has(m.id)) {
        const { resolve: res, reject } = this.pending.get(m.id);
        this.pending.delete(m.id);
        if (m.error) reject(new Error(JSON.stringify(m.error)));
        else res(m.result);
      }
    });
  }

  cmd(method, params = {}, timeoutMs = 20000) {
    const id = ++this.id;
    return new Promise((res, rej) => {
      const t = setTimeout(() => {
        this.pending.delete(id);
        rej(new Error(`CDP ${method} 超时`));
      }, timeoutMs);
      this.pending.set(id, {
        resolve: (v) => { clearTimeout(t); res(v); },
        reject: (e) => { clearTimeout(t); rej(e); },
      });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }

  /** 求值（自动 await Promise，返回可序列化值） */
  async eval(expr) {
    const r = await this.cmd('Runtime.evaluate', {
      expression: expr, awaitPromise: true, returnByValue: true,
    });
    if (r.exceptionDetails) {
      throw new Error('页面内求值异常：'
        + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
    }
    return r.result.value;
  }

  /** 轮询直到表达式为真 */
  async waitFor(expr, timeoutMs = 10000, label = expr, stepMs = 100) {
    const t0 = Date.now();
    for (;;) {
      if (await this.eval(expr)) { return true; }
      if (Date.now() - t0 > timeoutMs) { throw new Error(`等待超时：${label}`); }
      await new Promise((r) => setTimeout(r, stepMs));
    }
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** 让系统分一个空闲端口（避免与上一次没退干净的 headless 抢端口） */
function freePort() {
  return new Promise((res, rej) => {
    const srv = createServer();
    srv.on('error', rej);
    srv.listen(0, '127.0.0.1', () => {
      const p = srv.address().port;
      srv.close(() => res(p));
    });
  });
}

/** 把一段文本当成文件塞进 input 并触发 change（等价于用户选文件） */
function injectFileExpr(name, text, mime = 'text/plain') {
  return `(() => {
    const file = new File([${JSON.stringify(text)}], ${JSON.stringify(name)},
                          { type: ${JSON.stringify(mime)} });
    const dt = new DataTransfer();
    dt.items.add(file);
    const input = document.getElementById('file');
    input.files = dt.files;
    input.dispatchEvent(new Event('change'));
    return true;
  })()`;
}

function snapshotExpr() {
  return `(() => {
    const log = document.getElementById('log');
    return {
      dzClass: document.getElementById('dropzone').className,
      dzText: document.getElementById('dzText').textContent,
      dzFile: document.getElementById('dzFile').textContent,
      fwStat: document.getElementById('fwStat').textContent,
      sendDisabled: document.getElementById('btnSend').disabled,
      devId: document.getElementById('devId').value,
      idDownDisabled: document.getElementById('idDown').disabled,
      logTail: [...log.children].slice(-6).map((e) => e.textContent),
    };
  })()`;
}

// ------------------------------------------------------------------ 主流程

let chrome = null;
let cdp = null;

async function main() {
  const bin = CHROME_CANDIDATES.find((p) => existsSync(p));
  if (!bin) throw new Error('找不到 Chrome/Edge —— 可用 CHROME_PATH 指定');
  console.log(`浏览器: ${bin}`);
  console.log(`目标页: ${URL_BASE}/\n`);

  const profile = resolve(tmpdir(), 'wb-ui-test-' + process.pid);
  mkdirSync(profile, { recursive: true });

  const port = PORT || await freePort();
  console.log(`调试端口: ${port}`);

  chrome = spawn(bin, [
    '--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
    '--disable-extensions', '--hide-scrollbars',
    `--remote-debugging-port=${port}`, '--remote-allow-origins=*',
    `--user-data-dir=${profile}`,
    '--window-size=900,1500',
    'about:blank',
  ], { stdio: 'ignore' });

  // 等 target 出现
  let wsUrl = '';
  for (let i = 0; i < 100 && !wsUrl; i++) {
    await sleep(200);
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === 'page' && t.webSocketDebuggerUrl);
      if (page) wsUrl = page.webSocketDebuggerUrl;
    } catch (_) { /* 还没起来 */ }
  }
  if (!wsUrl) throw new Error('连不上 headless 浏览器的调试端口');

  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => {
    ws.addEventListener('open', res, { once: true });
    ws.addEventListener('error', () => rej(new Error('CDP WebSocket 连接失败')), { once: true });
  });
  cdp = new Cdp(ws);

  // 收集页面 JS 报错/console error —— 页面自己有 bug 时要看得见
  const pageErrors = [];
  ws.addEventListener('message', (ev) => {
    const m = JSON.parse(ev.data);
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
      pageErrors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
    }
    if (m.method === 'Runtime.exceptionThrown') {
      pageErrors.push('未捕获异常：'
        + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
    }
  });

  await cdp.cmd('Page.enable');
  await cdp.cmd('Runtime.enable');
  await cdp.cmd('Emulation.setDeviceMetricsOverride',
    { width: 900, height: 1500, deviceScaleFactor: 1, mobile: false });

  // 注入一个假的 Web Serial —— 「点连接之后」才成立的那几条断言（按钮是否真的把
  // 「已选固件」算进去、连接后地址是否被锁）全靠它。只实现页面用到的那几个成员。
  await cdp.cmd('Page.addScriptToEvaluateOnNewDocument', {
    source: `(() => {
      const fakePort = {
        open: async () => {},
        close: async () => {},
        getInfo: () => ({ usbVendorId: 0x0483, usbProductId: 0x5740 }),
        writable: new WritableStream({ write: async () => {} }),
        readable: new ReadableStream({ start() {} }),   // 不推数据：reader 一直等
      };
      Object.defineProperty(navigator, 'serial', {
        configurable: true,
        value: {
          requestPort: async () => fakePort,
          addEventListener: () => {},
          removeEventListener: () => {},
        },
      });
    })();`,
  });

  await cdp.cmd('Page.navigate', { url: URL_BASE + '/' });
  await cdp.waitFor('document.readyState === "complete"', 15000, '页面加载');
  await cdp.waitFor('document.getElementById("log").childElementCount > 0', 10000, '首条日志');

  // ---------------------------------------------------------------- ① 加载
  console.log('=== [1] 页面加载 ===');
  const boot = await cdp.eval(`(() => {
    const log = document.getElementById('log');
    return { lines: [...log.children].map((e) => e.textContent),
             sendDisabled: document.getElementById('btnSend').disabled,
             serial: ('serial' in navigator) };
  })()`);
  ok(boot.lines.some((l) => l.includes('载体层自检通过')), '载体层自检通过');
  ok(!boot.lines.some((l) => l.includes('自检失败')), '没有自检失败项');
  ok(boot.serial === true, '页面识别到 Web Serial（测试注入的 mock）');
  ok(boot.sendDisabled === true, '未连串口、未选固件 → 「开始升级」禁用');
  ok(pageErrors.length === 0, '页面无 JS 报错' + (pageErrors.length ? '：' + pageErrors[0] : ''));

  // ---------------------------------------------------------------- ② 步进器
  console.log('\n=== [2] 从机地址步进器 ===');
  const stepClick = (id) => `document.getElementById('${id}').click();`;
  ok((await cdp.eval(`(() => { ${stepClick('idUp')}${stepClick('idUp')}
        return document.getElementById('devId').value; })()`)) === '3', '+ 两次 → 3');
  ok((await cdp.eval(`(() => { ${stepClick('idDown')}
        return document.getElementById('devId').value; })()`)) === '2', '− 一次 → 2');
  ok((await cdp.eval(`(() => {
        const el = document.getElementById('devId');
        el.value = '0'; el.dispatchEvent(new Event('change'));
        return el.value; })()`)) === '1', '输入 0 被钳到下限 1');
  ok((await cdp.eval(`(() => {
        const el = document.getElementById('devId');
        el.value = '999'; el.dispatchEvent(new Event('change'));
        return el.value; })()`)) === '127', '输入 999 被钳到上限 127');
  ok((await cdp.eval(`(() => {
        const el = document.getElementById('devId');
        el.value = 'abc'; el.dispatchEvent(new Event('change'));
        return el.value; })()`)) === '1', '输入非数字回落到 1');
  ok((await cdp.eval(`(() => {
        const el = document.getElementById('devId');
        el.value = ''; el.dispatchEvent(new Event('change'));
        return el.value; })()`)) === '1', '清空后回落到 1');
  await cdp.eval(`(() => { const el = document.getElementById('devId');
        el.value = '1'; el.dispatchEvent(new Event('change')); })()`);

  // 布局没塌（"好看"没法自动断言，但"没塌"可以：同一行、居中、尺寸合理）
  const geo = await cdp.eval(`(() => {
    const box = (id) => { const b = document.getElementById(id).getBoundingClientRect();
      return { x: Math.round(b.x), y: Math.round(b.y),
               w: Math.round(b.width), h: Math.round(b.height),
               cy: Math.round(b.y + b.height / 2) }; };
    return { btn: box('btnConnect'), st: box('idStepper'), inp: box('devId') };
  })()`);
  const dcy = Math.abs(geo.btn.cy - geo.st.cy);
  ok(dcy <= 4, `步进器与按钮垂直居中（中心差 ${dcy}px）`);
  // 顶部 y 允许差 1-2px：两者高度不同，居中对齐时顶端自然有点差
  ok(Math.abs(geo.btn.y - geo.st.y) <= 2,
    `与按钮在同一行（顶部 y ${geo.btn.y} / ${geo.st.y}）`);
  ok(geo.st.h >= 26 && geo.st.h <= 48, `步进器高度合理（${geo.st.h}px，按钮 ${geo.btn.h}px）`);
  ok(geo.inp.h >= 18 && geo.inp.h <= 34, `数字输入区高度合理（${geo.inp.h}px）`);
  ok(geo.st.x + geo.st.w <= 900, `未溢出视口（右边缘 ${geo.st.x + geo.st.w}）`);

  // ---------------------------------------------------------------- ③ 连接
  console.log('\n=== [3] 连接串口（测试注入的假 Web Serial）===');
  await cdp.eval(`document.getElementById('btnConnect').click()`);
  await cdp.waitFor('document.getElementById("status").textContent === "已连接"',
    10000, '连上串口');
  const conn = await cdp.eval(`(() => ({
    badge: document.getElementById('status').textContent,
    badgeClass: document.getElementById('status').className,
    devIdDisabled: document.getElementById('devId').disabled,
    idDownDisabled: document.getElementById('idDown').disabled,
    connectDisabled: document.getElementById('btnConnect').disabled,
    disconnectDisabled: document.getElementById('btnDisconnect').disabled,
    sendDisabled: document.getElementById('btnSend').disabled,
    stepperDim: document.getElementById('idStepper').classList.contains('disabled'),
    logTail: [...document.getElementById('log').children].slice(-2).map((e) => e.textContent),
  }))()`);
  ok(conn.badge === '已连接' && conn.badgeClass.includes('on'), '状态徽标变「已连接」');
  ok(conn.devIdDisabled === true && conn.idDownDisabled === true && conn.stepperDim,
    '连接后从机地址被锁定（中途不许改）');
  ok(conn.connectDisabled === true && conn.disconnectDisabled === false,
    '「连接串口」禁用、「断开」可用');
  ok(conn.sendDisabled === true, '已连接但还没选固件 → 仍禁用');
  ok(conn.logTail.some((l) => l.includes('串口已连接 @ 115200')), '日志记录连接与地址');

  // ---------------------------------------------------------------- ④ 好 hex
  console.log('\n=== [4] 选中合法 .hex ===');
  ok(await cdp.eval(injectFileExpr('app_test.hex', GOOD_HEX)), '注入 .hex 文件');
  await cdp.waitFor('["picked","invalid"].some((c) => '
    + 'document.getElementById("dropzone").classList.contains(c))', 10000, '.hex 解析结束');
  let s = await cdp.eval(snapshotExpr());
  ok(s.dzClass.includes('picked'), '.hex 解析通过（框变绿）');
  ok(s.dzFile.includes('0x08004000'), '显示起始地址 0x08004000');
  ok(/KB/.test(s.dzFile), `镜像大小已显示（${s.dzFile.slice(0, 60)}）`);
  ok(s.sendDisabled === false, '已连接 + 已选固件 → 「开始升级」可用');
  ok(s.fwStat.startsWith('HEX'), '状态显示 HEX');
  ok(s.logTail.some((l) => l.includes('已选择固件')), '日志记录已选固件');
  ok(s.logTail.some((l) => /向量表 SP=0x20018000/.test(l)), '日志打印向量表');

  // ---------------------------------------------------------------- ④ 坏 hex
  console.log('\n=== [5] 起始地址错的 .hex ===');
  ok(await cdp.eval(injectFileExpr('bad.hex', BAD_HEX)), '注入地址错的 .hex');
  await cdp.waitFor('document.getElementById("dropzone").classList.contains("invalid")',
    10000, '坏 hex 被拒');
  s = await cdp.eval(snapshotExpr());
  ok(s.dzClass.includes('invalid'), '框变红（invalid）');
  ok(s.dzFile.includes('0x08000000') && s.dzFile.includes('低于'), `错误写进框里（${s.dzFile.slice(0, 70)}）`);
  ok(s.sendDisabled === true, '坏固件被拒 → 「开始升级」保持禁用（固件是必要条件）');
  ok(s.logTail.some((l) => l.includes('固件不可用')), '日志给出「固件不可用」');

  // ---------------------------------------------------------------- ⑤ 非法后缀
  console.log('\n=== [6] 拖入 .txt ===');
  await cdp.eval(`(() => {
    const dt = new DataTransfer();
    dt.items.add(new File(['hi'], 'notes.txt', { type: 'text/plain' }));
    const dz = document.getElementById('dropzone');
    const ev = new Event('drop', { bubbles: true, cancelable: true });
    ev.dataTransfer = dt;
    dz.dispatchEvent(ev);
  })()`);
  s = await cdp.eval(snapshotExpr());
  ok(s.logTail.some((l) => l.includes('不支持 notes.txt')), '提示不支持 .txt');
  ok(s.sendDisabled === true, '仍保持禁用（未被 .txt 影响）');

  // ---------------------------------------------------------------- ⑥ 小 hex + bin
  console.log('\n=== [7] 过小的 .hex 与正常 .bin ===');
  await cdp.eval(injectFileExpr('tiny.hex', SMALL_HEX));
  await cdp.waitFor('document.getElementById("dropzone").classList.contains("invalid")',
    10000, '过小 hex 被拒');
  s = await cdp.eval(snapshotExpr());
  ok(s.dzFile.includes('至少要 16 字节'), `过小被拒（${s.dzFile.slice(0, 50)}）`);

  const binName = 'app_test.bin';
  await cdp.eval(`(() => {
    const bin = new Uint8Array(${JSON.stringify(Array.from(GOOD_BIN))});
    const dt = new DataTransfer();
    dt.items.add(new File([bin], ${JSON.stringify(binName)},
                          { type: 'application/octet-stream' }));
    const input = document.getElementById('file');
    input.files = dt.files;
    input.dispatchEvent(new Event('change'));
  })()`);
  await cdp.waitFor('document.getElementById("dropzone").classList.contains("picked")',
    10000, '.bin 就绪');
  s = await cdp.eval(snapshotExpr());
  ok(s.fwStat.startsWith('BIN'), `.bin 也可用（${s.fwStat}）`);
  ok(s.sendDisabled === false, '已连接 + 已选 .bin → 「开始升级」可用');

  // ---------------------------------------------------------------- 静默期检查
  console.log('\n=== [8] 全程无 JS 报错 ===');
  ok(pageErrors.length === 0,
    '收尾检查无报错' + (pageErrors.length ? '：' + pageErrors.slice(0, 2).join(' | ') : ''));

  // ---------------------------------------------------------------- 截图
  if (SHOT) {
    console.log('\n=== [9] 整页截图 ===');
    if (SHOT_EMPTY) {
      await cdp.cmd('Page.reload');                             // 回到「刚打开」的状态
      await cdp.waitFor('document.readyState === "complete"', 15000, '刷新');
      await cdp.waitFor('document.getElementById("log").childElementCount > 0', 10000, '自检日志');
    } else {
      await cdp.eval(injectFileExpr('app_test.hex', GOOD_HEX));  // 让框处于「已选好」态
      await cdp.waitFor('document.getElementById("dropzone").classList.contains("picked")',
        10000, '截图前的就绪态');
    }
    await sleep(200);
    const shot = await cdp.cmd('Page.captureScreenshot',
      { format: 'png', captureBeyondViewport: true });
    const out = resolve(SHOT);
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, Buffer.from(shot.data, 'base64'));
    console.log('  已保存 ' + out);
  }
}

try {
  await main();
} catch (e) {
  fail++;
  console.log('\n★ 测试中断：' + e.message);
} finally {
  try { cdp?.ws.close(); } catch (_) {}
  try { chrome?.kill(); } catch (_) {}
}

console.log();
console.log(`=== 结果：${pass} 项通过，${fail} 项失败 ===`);
process.exit(fail ? 1 : 0);
