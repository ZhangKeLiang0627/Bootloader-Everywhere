/**
 * YMODEM-1K 发送端（浏览器版）
 *
 * 与 TestApp/tools/ymodem_send.py 保持同一套逻辑与超时参数，
 * 两者行为应当一致 —— 命令行版是参照实现。
 *
 * 协议要点（对应 Bootloader 的 core/bl_ymodem）：
 *   · 接收方先发 'C'（0x43）表示「CRC 模式，请开始」
 *   · 首包用 SOH(128B)：文件名\0 + 十进制大小 + 填充
 *   · 数据包用 STX(1024B)，序号从 1 开始，逐包等 ACK
 *   · 结束后发 EOT，接收方 ACK 后再发 'C' 请批次结束包
 *   · 批次结束包 = SOH(128B) 全 0，接收方 ACK 后结束
 *
 * 为什么不需要把 CRC32 传过去：YMODEM 的帧级 CRC16 + ACK/NAK 已经保证了
 * 「PC → 板子」的传输正确性；整镜像 CRC32 是板子用来防 Flash 位翻转的，
 * 由它自己算并存下来。所以标准工具无需改造也能用。
 */

export const SOH = 0x01;      // 128 字节包
export const STX = 0x02;      // 1024 字节包
export const EOT = 0x04;
export const ACK = 0x06;
export const NAK = 0x15;
export const CAN = 0x18;
export const CRC_REQ = 0x43;  // 'C'

export const PKT_128 = 128;
export const PKT_1024 = 1024;

const WAIT_C_TIMEOUT = 20000;   // 等接收方的 'C'
const ACK_TIMEOUT = 3000;       // 等单包 ACK
const FIRST_ACK_TIMEOUT = 12000; // 首包：板子要先擦完扇区才回 ACK
const MAX_RETRY = 10;

/** CRC-16/XMODEM：poly=0x1021, init=0x0000 */
export function crc16Xmodem(data) {
    let crc = 0;
    for (let i = 0; i < data.length; i++) {
        crc ^= (data[i] << 8) & 0xFFFF;
        for (let b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xFFFF)
                                 : ((crc << 1) & 0xFFFF);
        }
    }
    return crc;
}

/**
 * 接收缓冲 —— 把串口来的字节攒起来，供按需取用。
 *
 * 关键：板端的调试日志和 YMODEM 的协议字节共用同一个串口，
 * 所以取字节时不能假设「下一个字节就是协议字节」，必须容忍夹杂的文本，
 * 并把它们原样显示出来（那也是有价值的现场信息）。
 */
export class ByteQueue {
    constructor() {
        this.buf = new Uint8Array(0);
        this.noise = 0;          // 统计被忽略的文本字节数
    }

    push(chunk) {
        const merged = new Uint8Array(this.buf.length + chunk.length);
        merged.set(this.buf, 0);
        merged.set(chunk, this.buf.length);
        this.buf = merged;
    }

    get length() { return this.buf.length; }

    /** 取一个字节；没有则返回 null */
    shift() {
        if (this.buf.length === 0) return null;
        const b = this.buf[0];
        this.buf = this.buf.subarray(1);
        return b;
    }

    clear() { this.buf = new Uint8Array(0); }
}

/**
 * 等待一个特定字节。
 *
 * 中间夹着的其它字节会被收集起来交给 onNoise 回调（通常是板端日志），
 * 而不是当作错误 —— 这是实测踩过的坑：第一次实现把非 ACK 字节直接
 * 判为失败，结果板子打印的 `[session] file=...` 让整条链路误报失败。
 */
async function waitForByte(io, queue, wanted, timeoutMs, label, onLog) {
    const deadline = Date.now() + timeoutMs;
    const noise = [];
    while (Date.now() < deadline) {
        const b = queue.shift();
        if (b === null) {
            await io.pump(30);
            continue;
        }
        if (b === wanted) {
            if (noise.length) onLog(decodeNoise(noise), 'board');
            return true;
        }
        if (b === CAN) {
            if (noise.length) onLog(decodeNoise(noise), 'board');
            onLog('接收方发送 CAN（取消）', 'err');
            return false;
        }
        noise.push(b);
        if (noise.length > 8192) break;
    }
    if (noise.length) onLog(decodeNoise(noise), 'board');
    onLog(`等待 ${label} 超时（${timeoutMs} ms）`, 'err');
    return false;
}

function decodeNoise(bytes) {
    const s = new TextDecoder('utf-8', { fatal: false }).decode(new Uint8Array(bytes));
    return s.replace(/\r/g, '');
}

export class YmodemSender {
    constructor(io, { onLog = () => {}, onProgress = () => {} } = {}) {
        this.io = io;
        this.onLog = onLog;
        this.onProgress = onProgress;
        this.queue = new ByteQueue();
        this.stats = { packets: 0, retries: 0, bytes: 0 };
    }

    /** 把串口收到的数据推进缓冲 */
    feed(chunk) { this.queue.push(chunk); }

    /** 读干缓冲并显示（等待接收方发 'C' 之前用） */
    async drainToLog() {
        if (this.queue.length === 0) return;
        const arr = [];
        let b;
        while ((b = this.queue.shift()) !== null) arr.push(b);
        this.queue.noise += arr.length;
        this.onLog(decodeNoise(arr), 'board');
    }

    /** 发一包并等 ACK */
    async sendPacket(seq, payload, { expectAck = true, label = '',
                                     ackTimeout = ACK_TIMEOUT } = {}) {
        const is128 = payload.length === PKT_128;
        const head = is128 ? SOH : STX;
        const crc = crc16Xmodem(payload);
        const frame = new Uint8Array(3 + payload.length + 2);
        frame[0] = head;
        frame[1] = seq & 0xFF;
        frame[2] = (~seq) & 0xFF;
        frame.set(payload, 3);
        frame[frame.length - 2] = (crc >> 8) & 0xFF;
        frame[frame.length - 1] = crc & 0xFF;

        for (let attempt = 1; attempt <= MAX_RETRY; attempt++) {
            await this.io.write(frame);
            if (!expectAck) return true;

            const got = await this.waitProtocolByte(ackTimeout);
            if (got === ACK) {
                this.stats.packets++;
                this.stats.bytes += payload.length;
                return true;
            }
            if (got === NAK) {
                this.stats.retries++;
                this.onLog(`第 ${seq} 包收到 NAK，重传 (${attempt}/${MAX_RETRY})`, 'warn');
                continue;
            }
            if (got === CAN) {
                this.onLog(`第 ${seq} 包收到 CAN`, 'err');
                return false;
            }
            this.stats.retries++;
            this.onLog(`第 ${seq} 包${label ? ' ' + label : ''} 等 ACK 超时（${ackTimeout} ms），`
                       + `重传 (${attempt}/${MAX_RETRY})`, 'warn');
        }
        return false;
    }

    /** 等 ACK / NAK / CAN 之一，中途的文本交给日志 */
    async waitProtocolByte(timeoutMs) {
        const deadline = Date.now() + timeoutMs;
        const noise = [];
        while (Date.now() < deadline) {
            const b = this.queue.shift();
            if (b === null) {
                await this.io.pump(30);
                continue;
            }
            if (b === ACK || b === NAK || b === CAN) {
                if (noise.length) this.onLog(decodeNoise(noise), 'board');
                return b;
            }
            noise.push(b);
            if (noise.length >= 8192) break;
        }
        if (noise.length) this.onLog(decodeNoise(noise), 'board');
        return null;
    }

    /** EOT 可能需要发两次（部分实现第一遍回 NAK） */
    async sendEot() {
        for (let i = 0; i < 3; i++) {
            await this.io.write(new Uint8Array([EOT]));
            const got = await this.waitProtocolByte(ACK_TIMEOUT);
            if (got === ACK) return true;
            if (got === NAK) { this.onLog('EOT 收到 NAK，再发一次（正常流程）', 'dim'); continue; }
            if (got === null) { this.onLog('EOT 等 ACK 超时，重发', 'warn'); continue; }
        }
        return false;
    }

    /** 主流程 */
    async send(fileData, fileName) {
        const total = fileData.length;
        this.onLog(`文件：${fileName}`, 'info');
        this.onLog(`大小：${total} 字节 (${(total / 1024).toFixed(2)} KB)`, 'info');
        this.onLog(`分块：${Math.ceil(total / PKT_1024)} 个 1024B 数据包`, 'info');

        // ---- 1. 等握手 ----
        this.onLog('等待板子握手字符 \'C\' ...', 'step');
        if (!await waitForByte(this.io, this.queue, CRC_REQ, WAIT_C_TIMEOUT,
                               "'C'", this.onLog)) return false;
        this.onLog('收到 \'C\' —— 板子已就绪', 'ok');

        // ---- 2. 首包 ----
        const nameBytes = new TextEncoder().encode(fileName);
        const sizeStr = new TextEncoder().encode(String(total));
        const head = new Uint8Array(PKT_128);      // 默认 0 填充
        head.set(nameBytes.subarray(0, 63), 0);
        head[nameBytes.length] = 0;
        head.set(sizeStr, nameBytes.length + 1);

        this.onLog('发送首包（文件名 + 大小）...', 'step');
        if (!await this.sendPacket(0, head, { label: '首包',
                                              ackTimeout: FIRST_ACK_TIMEOUT })) {
            return false;
        }
        this.onLog('首包已确认（板子正在擦除 APP 扇区）', 'ok');

        // ---- 3. 等数据阶段的 'C' ----
        if (!await waitForByte(this.io, this.queue, CRC_REQ, WAIT_C_TIMEOUT,
                               "数据阶段的 'C'", this.onLog)) return false;
        this.onLog('开始传输数据', 'ok');

        // ---- 4. 数据包 ----
        let seq = 1;
        let off = 0;
        const t0 = Date.now();
        while (off < total) {
            const chunk = new Uint8Array(PKT_1024).fill(0x1A);   // Ctrl-Z 填充
            const n = Math.min(PKT_1024, total - off);
            chunk.set(fileData.subarray(off, off + n), 0);

            if (!await this.sendPacket(seq, chunk)) {
                this.onLog(`第 ${seq} 包发送失败，中止`, 'err');
                await this.io.write(new Uint8Array(8).fill(CAN));
                return false;
            }
            off += n;
            seq++;
            const el = (Date.now() - t0) / 1000;
            this.onProgress(Math.min(off, total), total,
                            el > 0 ? (off / 1024 / el) : 0);
        }

        // ---- 5. EOT ----
        this.onLog('发送 EOT ...', 'step');
        if (!await this.sendEot()) {
            this.onLog('EOT 未被确认', 'err');
            return false;
        }
        this.onLog('EOT 已确认', 'ok');

        // ---- 6. 批次结束包 ----
        this.onLog('发送批次结束包 ...', 'step');
        if (!await waitForByte(this.io, this.queue, CRC_REQ, WAIT_C_TIMEOUT,
                               "结束阶段的 'C'", this.onLog)) {
            this.onLog('未收到结束阶段的 \'C\' —— 数据已传完，通常不影响结果', 'warn');
            return true;
        }
        if (!await this.sendPacket(0, new Uint8Array(PKT_128), { label: '结束包' })) {
            this.onLog('结束包未确认 —— 数据已传完，通常不影响结果', 'warn');
            return true;
        }
        this.onLog('结束包已确认', 'ok');
        return true;
    }
}
