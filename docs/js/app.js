/**
 * Bootloader-Everywhere 网页上位机
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
    $('baud').disabled = on;
    $('status').textContent = on ? '已连接' : '未连接';
    $('status').className = 'badge ' + (on ? 'on' : 'off');
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

/**
 * 唤回 Bootloader：向串口发 0x7F(DEL)。
 *
 * - 若设备在跑 APP：APP 的串口监听收到 0x7F 后写 RAM 标志并软复位，
 *   Bootloader 上电看到标志即进 IAP。
 * - 若设备已在 IAP：0x7F 被 Bootloader 的握手等待当作噪声忽略，无害。
 *
 * 连发 5 个：APP 主循环是 100ms 轮询，单发一个可能恰好落在轮询间隙被
 * 丢弃，连发保证至少有一个被读到。
 */
async function wakeUpBootloader() {
    log('唤回 Bootloader ...', 'step');
    for (let i = 0; i < 5; i++) {
        if (!writer) break;
        try { await writer.write(new Uint8Array([0x7F])); } catch (_) { break; }
        await io.pump(30);
    }
    // 等设备复位并进入 IAP（真正的等待由 sender.send() 的握手超时兜底）
    await io.pump(400);
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

        // 1. 自动唤回 Bootloader（无需用户再手动点「进入 Bootloader」）
        await wakeUpBootloader();

        // 2. 发送（send() 内部先等 'C' 握手，设备进 IAP 后自动开始）
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

/** 只读取串口（不发送），用于观察板子输出 */
function startSniffer() {
    if (sender) return;
    sender = new YmodemSender(io, {
        onLog: (msg, kind) => log(msg, kind === 'board' ? 'board' : kind),
        onProgress: () => {},
    });
    log('已开始监听串口输出（可直接观察板子日志）', 'dim');
}

// ------------------------------------------------------------ 文件选择

function setupDropzone() {
    const dz = $('dropzone');
    const file = $('file');
    const dzIcon = $('dzIcon');
    const dzText = $('dzText');
    const dzFile = $('dzFile');

    function showFile(f) {
        if (!f) {
            dzIcon.textContent = '⇪';
            dzText.textContent = '点击选择固件，或拖拽 .bin 文件到这里';
            dzFile.textContent = '';
            return;
        }
        const kb = f.size / 1024;
        const sizeStr = kb >= 1024 ? (kb / 1024).toFixed(2) + ' MB'
                                   : kb.toFixed(1) + ' KB';
        dzIcon.textContent = '✓';
        dzText.textContent = f.name;
        dzFile.textContent = `${sizeStr} · ${f.size.toLocaleString()} 字节`;
    }

    dz.addEventListener('click', () => file.click());
    file.addEventListener('change', () => showFile(file.files[0]));

    dz.addEventListener('dragover', (e) => {
        e.preventDefault();
        dz.classList.add('dragover');
    });
    dz.addEventListener('dragleave', () => dz.classList.remove('dragover'));
    dz.addEventListener('drop', (e) => {
        e.preventDefault();
        dz.classList.remove('dragover');
        const f = e.dataTransfer.files && e.dataTransfer.files[0];
        if (!f) return;
        if (!f.name.toLowerCase().endsWith('.bin')) {
            log('请选择 .bin 固件文件', 'warn');
            return;
        }
        const dt = new DataTransfer();
        dt.items.add(f);
        file.files = dt.files;
        showFile(f);
        log(`已选择固件：${f.name}`, 'info');
    });
}

// ------------------------------------------------------------------ 绑定

window.addEventListener('DOMContentLoaded', () => {
    $('btnConnect').addEventListener('click', connect);
    $('btnDisconnect').addEventListener('click', disconnect);
    $('btnSend').addEventListener('click', doSend);
    $('btnClear').addEventListener('click', clearLog);

    setupDropzone();

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
