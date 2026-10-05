/**
 * Bootloader-Everywhere 网页上位机
 *
 * 用 Web Serial API 直接操作串口，把编译好的 .bin 按自定义 0xA5 帧协议
 * （框架见 ./protocol.js，规格见 ../PROTOCOL_DESIGN.md §0）发给板子。
 * 纯前端、无后端，直接挂 GitHub Pages —— 有网的地方打开就能刷固件。
 *
 * 浏览器要求：Chrome / Edge 89+（Web Serial API），且页面在安全上下文
 * （https:// 或 localhost）。
 */

import { Iap, selftest } from './protocol.js';

const $ = (id) => document.getElementById(id);

const DEFAULT_BAUD = 115200;      // 与固件一致，协议里是定死的（不做波特率协商）

// ------------------------------------------------------------------ 状态

let port = null;
let writer = null;
let reader = null;
let readLoopRunning = false;
let iap = null;
let busy = false;

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

    while (box.childElementCount > 2000) box.removeChild(box.firstChild);
}

function clearLog() { $('log').innerHTML = ''; }

// ------------------------------------------------------- 板子日志（文本）

// 空闲时串口字节是板子的文本日志；等应答时那些字节是协议帧。
const decoder = new TextDecoder('utf-8', { fatal: false });
let textPending = '';

function showText(bytes) {
    textPending += decoder.decode(bytes, { stream: true });
    for (;;) {
        const i = textPending.indexOf('\n');
        if (i < 0) break;
        const line = textPending.slice(0, i).replace(/\r$/, '');
        textPending = textPending.slice(i + 1);
        if (line.trim()) log(line, 'board');
    }
    if (textPending.length > 512) {          // 长时间没有换行也不能一直攒着
        log(textPending, 'board');
        textPending = '';
    }
}

// ------------------------------------------------------------ Web Serial

async function connect() {
    if (!('serial' in navigator)) {
        log('这个浏览器不支持 Web Serial API。请用 Chrome / Edge，'
            + '并且通过 https:// 或 localhost 打开页面。', 'err');
        return;
    }
    try {
        port = await navigator.serial.requestPort();
        await port.open({ baudRate: DEFAULT_BAUD, dataBits: 8, stopBits: 1, parity: 'none' });
    } catch (e) {
        log('打开串口失败：' + e.message, 'err');
        port = null;
        return;
    }

    writer = port.writable.getWriter();
    makeIap();
    setConnected(true);
    log(`串口已连接 @ ${DEFAULT_BAUD} 8N1`, 'ok');
    startReadLoop();
}

function makeIap() {
    iap = new Iap({
        write: async (bytes) => { await writer.write(bytes); },
        pump: (ms) => new Promise((r) => setTimeout(r, ms)),
    }, {
        deviceId: parseInt($('devId').value, 10) || 1,
        onLog: log,
        onProgress: onProgress,
    });
    iap.parser.reset();
}

function setConnected(on) {
    $('btnConnect').disabled = on;
    $('btnDisconnect').disabled = !on;
    $('btnSend').disabled = !on || busy;
    $('devId').disabled = on;
    $('status').textContent = on ? '已连接' : '未连接';
    $('status').className = 'badge ' + (on ? 'on' : 'off');
}

async function disconnect() {
    stopReadLoop();
    try { if (writer) { writer.releaseLock(); writer = null; } } catch (_) {}
    try { if (reader) { reader.releaseLock(); reader = null; } } catch (_) {}
    try { if (port) { await port.close(); } } catch (_) {}
    port = null;
    iap = null;
    textPending = '';
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
                if (!value || !value.length || !iap) continue;
                // 等应答时字节属于协议（交给解析器）；空闲时当板子的文本日志显示
                if (iap.expectsReply()) iap.feed(value);
                else showText(value);
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

// ------------------------------------------------------------------ 发送

function setBusy(on) {
    busy = on;
    $('btnSend').disabled = on || !port;
    $('btnConnect').disabled = on || !!port;
    $('btnSend').textContent = on ? '发送中…' : '开始升级';
}

function onProgress(done, total) {
    const pct = total > 0 ? (done * 100 / total) : 0;
    $('bar').style.width = pct.toFixed(1) + '%';
    $('bar').textContent = pct.toFixed(1) + '%';
    $('stat').textContent = `${done} / ${total} 字节`;
}

async function doSend() {
    const f = $('file').files && $('file').files[0];
    if (!f) { log('请先选择一个 .bin 文件', 'err'); return; }
    if (!port) { log('请先连接串口', 'err'); return; }

    setBusy(true);
    $('bar').style.width = '0%';
    $('bar').textContent = '0%';
    $('stat').textContent = '';

    try {
        const raw = new Uint8Array(await f.arrayBuffer());

        // 板子可能正在跑 APP，先把它唤回 Bootloader（也可以从 App 菜单改）
        await iap.recall();

        const r = await iap.send(raw, { name: f.name });
        if (r.ok) {
            log(`升级完成：${r.frames} 帧，重传 ${r.retries} 次，板端确认 ${r.written} 字节`, 'ok');
            $('bar').style.width = '100%';
            $('bar').textContent = '100%';
            log('板子随后跳转到新固件 —— 留意下方串口输出。', 'ok');
        } else {
            log(`升级失败（${r.reason}${r.code !== undefined ? ' code=0x' + r.code.toString(16) : ''}）`, 'err');
        }
    } catch (e) {
        log('发送异常：' + e.message, 'err');
    } finally {
        setBusy(false);
    }
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
        const sizeStr = kb >= 1024 ? (kb / 1024).toFixed(2) + ' MB' : kb.toFixed(1) + ' KB';
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

    // 页面一打开就在本地跑一遍载体层自检：与固件、Python 上位机共用同一组向量，
    // 三处都过才算 CRC 变体与字节序一致。不一致的话刷机会一帧都过不了。
    const st = selftest();
    if (st.fails.length) {
        for (const f of st.fails) log('自检失败：' + f, 'err');
        log(`载体层自检 ${st.fails.length} 项失败 —— 请不要用这个页面刷机`, 'err');
    } else {
        log(`载体层自检通过（${st.cases} 项）`, 'dim');
    }

    if (!('serial' in navigator)) {
        log('提示：当前浏览器不支持 Web Serial。请用 Chrome / Edge 打开，'
            + '并确保是 https:// 或 localhost。', 'warn');
    }
});

navigator.serial?.addEventListener('disconnect', () => {
    log('串口设备被拔出', 'warn');
    disconnect();
});
