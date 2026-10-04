/**
 * LUMOS-bootloader 网页上位机
 *
 * 用 Web Serial API 直接操作串口，把编译好的 .bin 通过 YMODEM-1K
 * 发给开发板。整个页面是纯前端，没有任何后端依赖，可以直接挂
 * GitHub Pages —— 有网的地方打开就能刷固件。
 *
 * 浏览器要求：Chrome / Edge 89+（Web Serial API）。
 * 且页面必须是安全上下文（https:// 或 localhost）。
 */

import { YmodemSender } from './ymodem.js';

const $ = (id) => document.getElementById(id);

// ------------------------------------------------------------------ 状态

let port = null;
let writer = null;
let reader = null;
let readLoopRunning = false;
let sender = null;
let busy = false;

const DEFAULT_BAUD = 115200;

// ------------------------------------------------------------------ 日志

function log(msg, kind = 'info') {
    const box = $('log');
    const line = document.createElement('div');
    line.className = 'line ' + kind;
    const t = new Date();
    const ts = `${String(t.getHours()).padStart(2, '0')}:`
             + `${String(t.getMinutes()).padStart(2, '0')}:`
             + `${String(t.getSeconds()).padStart(2, '0')}`;
    line.textContent = `[${ts}] ${msg}`;
    box.appendChild(line);
    box.scrollTop = box.scrollHeight;

    // 防止长时间运行把 DOM 撑爆
    while (box.childElementCount > 2000) box.removeChild(box.firstChild);
}

function clearLog() { $('log').innerHTML = ''; }

// ------------------------------------------------------------ Web Serial

async function connect() {
    if (!('serial' in navigator)) {
        log('这个浏览器不支持 Web Serial API。请用 Chrome / Edge，'
            + '并且通过 https:// 或 localhost 打开页面。', 'err');
        return;
    }
    const baud = parseInt($('baud').value, 10) || DEFAULT_BAUD;
    try {
        port = await navigator.serial.requestPort();
        await port.open({ baudRate: baud, dataBits: 8, stopBits: 1, parity: 'none' });
    } catch (e) {
        log('打开串口失败：' + e.message, 'err');
        port = null;
        return;
    }

    writer = port.writable.getWriter();
    setConnected(true);
    log(`串口已连接 @ ${baud} 8N1`, 'ok');
    startSniffer();          // 先挂上监听，板子的启动日志立刻可见
    startReadLoop();
}

function setConnected(on) {
    $('btnConnect').disabled = on;
    $('btnDisconnect').disabled = !on;
    $('btnSend').disabled = !on || busy;
    $('btnBoot').disabled = !on || busy;
    $('baud').disabled = on;
    $('status').textContent = on ? '已连接' : '未连接';
    $('status').className = 'badge ' + (on ? 'on' : 'off');
}

/**
 * 「进入 Bootloader」：向运行中的 APP 发送唤回指令。
 *
 * 协议：连续发 5 个 0x7F（DEL）。APP 侧检测到该序列后，写 RAM 标志
 * 并软件复位，Bootloader 上电看到标志即进入 IAP。
 * 用 5 个而非单个，是为了降低与数据流里的 0x7F 撞车的概率。
 */
async function enterBoot() {
    if (!writer) { log('请先连接串口', 'err'); return; }
    log('发送「进入 Bootloader」指令 ...', 'step');
    for (let i = 0; i < 5; i++) {
        await writer.write(new Uint8Array([0x7F]));
        await io.pump(25);
    }
    log('已发送。若设备运行的是支持该指令的 APP，会复位回到 Bootloader。', 'dim');
}

async function disconnect() {
    stopReadLoop();
    try { if (writer) { writer.releaseLock(); writer = null; } } catch (_) {}
    try { if (reader) { reader.releaseLock(); reader = null; } } catch (_) {}
    try { if (port) { await port.close(); } } catch (_) {}
    port = null;
    sender = null;      // 清掉缓冲里的残留数据，避免污染下次传输
    setConnected(false);
    log('串口已断开', 'dim');
}

function startReadLoop() {
    if (readLoopRunning) return;
    readLoopRunning = true;
    (async () => {
        try {
            reader = port.readable.getReader();
            while (readLoopRunning) {
                const { value, done } = await reader.read();
                if (done) break;
                if (value && value.length && sender) sender.feed(value);
            }
        } catch (e) {
            if (readLoopRunning) log('读取串口出错：' + e.message, 'err');
        } finally {
            try { if (reader) { reader.releaseLock(); reader = null; } } catch (_) {}
            readLoopRunning = false;
        }
    })();
}

function stopReadLoop() { readLoopRunning = false; }

// ------------------------------------------------------- io 适配（给发送端）

const io = {
    async write(bytes) {
        if (!writer) throw new Error('串口未连接');
        await writer.write(bytes);
    },
    /** 让出事件循环，给 reader 循环机会把数据推进来 */
    pump: (ms) => new Promise((r) => setTimeout(r, ms)),
};

// ------------------------------------------------------------------ 发送

function setBusy(on) {
    busy = on;
    $('btnSend').disabled = on || !port;
    $('btnConnect').disabled = on || !!port;
    $('btnSend').textContent = on ? '发送中…' : '开始升级';
}

function onProgress(done, total, kbps) {
    const pct = total > 0 ? (done * 100 / total) : 0;
    $('bar').style.width = pct.toFixed(1) + '%';
    $('bar').textContent = pct.toFixed(1) + '%';
    $('stat').textContent = `${done} / ${total} 字节　${kbps.toFixed(1)} KB/s`;
}

async function doSend() {
    const fileInput = $('file');
    const f = fileInput.files && fileInput.files[0];
    if (!f) { log('请先选择一个 .bin 文件', 'err'); return; }
    if (!port) { log('请先连接串口', 'err'); return; }

    setBusy(true);
    $('bar').style.width = '0%';
    $('bar').textContent = '0%';
    $('stat').textContent = '';

    try {
        const fileData = new Uint8Array(await f.arrayBuffer());
        sender = new YmodemSender(io, { onLog: log, onProgress });

        const ok = await sender.send(fileData, f.name);

        if (ok) {
            log(`传输完成：${sender.stats.packets} 包，${sender.stats.bytes} 字节，`
                + `重传 ${sender.stats.retries} 次`, 'ok');
            log('板子随后会复位并跳转到新固件 —— 留意下方串口输出。', 'ok');
        } else {
            log(`传输失败：已确认 ${sender.stats.packets} 包，重传 ${sender.stats.retries} 次`, 'err');
        }
    } catch (e) {
        log('发送异常：' + e.message, 'err');
    } finally {
        setBusy(false);
    }
}

/** 只读取串口（不发送），用于观察板子输出、进 IAP 长按等 */
function startSniffer() {
    if (sender) return;
    sender = new YmodemSender(io, {
        onLog: (msg, kind) => log(msg, kind === 'board' ? 'board' : kind),
        onProgress: () => {},
    });
    log('已开始监听串口输出（可直接观察板子日志）', 'dim');
}

// ------------------------------------------------------------------ 绑定

window.addEventListener('DOMContentLoaded', () => {
    $('btnConnect').addEventListener('click', connect);
    $('btnDisconnect').addEventListener('click', disconnect);
    $('btnSend').addEventListener('click', doSend);
    $('btnBoot').addEventListener('click', enterBoot);
    $('btnClear').addEventListener('click', clearLog);

    if (!('serial' in navigator)) {
        log('提示：当前浏览器不支持 Web Serial。请用 Chrome / Edge 打开，'
            + '并确保是 https:// 或 localhost。', 'warn');
    }
});

// 断线时清理
navigator.serial?.addEventListener('disconnect', () => {
    log('串口设备被拔出', 'warn');
    disconnect();
});
