#!/usr/bin/env python3
"""升级上位机 / 载体层参考实现。

与固件里的 protocol.cpp 是同一套帧格式与规则（docs/PROTOCOL_DESIGN.md §0.1、§0.5）。
`selftest` 用的向量与 tools/test_protocol.cpp 完全相同 —— 两端都过才算一致。

用法：
    python proto.py selftest                       # 只看两端口径是否一致，不碰串口
    python proto.py send build/app_test.bin        # 升级（默认 COM3）
    python proto.py send fw.bin --port COM7 --no-recall
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    serial = None

# ---------------------------------------------------------------- 载体层

HEAD = 0xA5
TAIL = 0x03
DIR_REPLY = 0x80
DATA_MAX = 1024
OVERHEAD = 7
FRAME_MIN = OVERHEAD        # 最小帧长 = 空载荷帧（END / STATUS）
FRAME_MAX = OVERHEAD + DATA_MAX

CMD_START = 0x01
CMD_DATA = 0x02
CMD_END = 0x03
CMD_STATUS = 0x04

BLOCK_SIZE = 512
DATA_HEAD = 14          # addr4 + total2 + index2 + vlen2 + cumCrc32_4
START_LEN = 16          # size4 + crc32_4 + sp4 + pc4

CODE_NAME = {
    0x00: "OK", 0x01: "帧CRC8错", 0x02: "长度非法", 0x03: "ID或地址不匹配",
    0x04: "未知命令", 0x05: "参数非法", 0x06: "状态错误", 0x07: "地址不连续",
    0x08: "累积CRC32不符", 0x09: "擦除失败", 0x0A: "写入失败", 0x0B: "回读校验失败",
    0x0C: "向量表非法", 0x0D: "总CRC32不符", 0x0E: "冗余字段不一致",
    0x0F: "回读与写入不一致",
}


def crc8(data):
    """CRC-8/SMBUS：poly 0x07 / init 0x00 / MSB-first（不反射）/ xorout 0x00。

    注意与 CRC32（LSB-first）位序方向相反。
    """
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if (c & 0x80) else ((c << 1) & 0xFF)
    return c


def crc32_iso(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def encode(fid, cmd, data=b""):
    body = bytes([fid, cmd]) + struct.pack('<H', len(data)) + data
    return bytes([HEAD]) + body + bytes([crc8(body)]) + bytes([TAIL])


class Parser:
    """流式解析：逐字节喂入，凑齐一帧返回 dict，否则 None。"""

    def __init__(self):
        self.reset()

    def reset(self):
        self.st = 'head'
        self.crc = 0
        self.buf = bytearray()
        self.fid = self.cmd = 0
        self.n = 0

    def feed(self, byte):
        # 每个字节最多被判两次（一次按原状态，一次作为可能的新帧头）
        for _ in range(3):
            if self.st == 'head':
                if byte != HEAD:
                    return None
                self.crc, self.buf = 0, bytearray()
                self.st = 'id'
                return None
            if self.st == 'id':
                self.fid = byte
                self.crc = self._step(self.crc, byte)
                self.st = 'cmd'
                return None
            if self.st == 'cmd':
                self.cmd = byte
                self.crc = self._step(self.crc, byte)
                self.st = 'lenlo'
                return None
            if self.st == 'lenlo':
                self.n = byte
                self.crc = self._step(self.crc, byte)
                self.st = 'lenhi'
                return None
            if self.st == 'lenhi':
                self.n |= byte << 8
                self.crc = self._step(self.crc, byte)
                if self.n > DATA_MAX:
                    self.st = 'head'
                    continue
                self.st = 'crc' if self.n == 0 else 'data'
                return None
            if self.st == 'data':
                self.buf.append(byte)
                self.crc = self._step(self.crc, byte)
                if len(self.buf) >= self.n:
                    self.st = 'crc'
                return None
            if self.st == 'crc':
                if byte != self.crc:
                    self.st = 'head'
                    continue
                self.st = 'tail'
                return None
            # tail
            self.st = 'head'
            if byte != TAIL:
                continue
            total = OVERHEAD + self.n
            if not (FRAME_MIN <= total <= FRAME_MAX):
                return None          # 一帧到来时的长度自检：越界就丢，让主机重传
            return {'id': self.fid, 'cmd': self.cmd, 'code': self.cmd & ~DIR_REPLY,
                    'reply': bool(self.cmd & DIR_REPLY), 'data': bytes(self.buf),
                    'total': total}

        return None

    @staticmethod
    def _step(crc, byte):
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
        return crc


# ---------------------------------------------------------------- 链路层

class Link:
    def __init__(self, port, baud=115200, device_id=1, verbose=True):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.parser = Parser()
        self.device_id = device_id
        self.verbose = verbose

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def flush(self):
        self.ser.reset_input_buffer()
        self.parser.reset()

    def request(self, cmd, data=b"", timeout=1.0, retries=5, label=""):
        """发一条命令并等它的应答（帧级重试）。返回应答 dict 或 None。"""
        for attempt in range(1, retries + 1):
            self.flush()
            self.ser.write(encode(self.device_id, cmd, data))
            self.ser.flush()
            f = self.recv(timeout, want=cmd | DIR_REPLY)
            if f is not None:
                return f
            if self.verbose and attempt < retries:
                print("    %s 无应答，重试 %d/%d" % (label or hex(cmd), attempt, retries))
        return None

    def recv(self, timeout, want=None):
        deadline = time.time() + timeout
        while time.time() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            f = self.parser.feed(b[0])
            if f is None:
                continue
            if not f['reply'] or f['id'] != self.device_id:
                continue
            if want is not None and f['cmd'] != want:
                continue
            return f
        return None

    def recall(self):
        """发唤回关键字让 APP 软复位（逐字节慢发：APP 每 100ms 才轮询一次）。"""
        if self.verbose:
            print("发唤回关键字 #Bootloader-Everywhere ...")
        for ch in b"#Bootloader-Everywhere":
            self.ser.write(bytes([ch]))
            self.ser.flush()
            time.sleep(0.15)


# ---------------------------------------------------------------- 升级流程

def send(args):
    raw = open(args.file, 'rb').read()
    if len(raw) < 16:
        print("★ 固件太小（至少 16 字节）")
        return 1
    sp, pc = struct.unpack('<II', raw[:8])
    body = raw[8:]                       # 数据流 = 镜像 [8, size)
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)

    print("固件 %s：%d 字节  SP=0x%08X PC=0x%08X" % (args.file, len(raw), sp, pc))
    print("  数据 %d 字节 / %d 帧 / 整片 CRC32 = 0x%08X" % (len(body), total_pkts, crc_total))

    link = Link(args.port, args.baud, args.id)
    try:
        if args.recall:
            link.recall()
            link.flush()

        # ---- START ----
        print("START（擦除中，可能数秒）...")
        t0 = time.time()
        payload = struct.pack('<IIII', len(raw), crc_total, sp, pc)
        f = link.request(CMD_START, payload, timeout=args.start_timeout, retries=2, label="START")
        if f is None:
            print("★ START 无应答 —— 设备没在 IAP？按住 PC0 上电重试")
            return 1
        code = f['data'][0]
        blk = struct.unpack('<H', f['data'][1:3])[0] if len(f['data']) >= 3 else 0
        print("  ← %s  blockSize=%d  (%.1f s)" % (CODE_NAME.get(code, code), blk, time.time() - t0))
        if code != 0x00:
            return 1

        # ---- DATA ----
        offset = 0                       # 已写入的字节数（相对数据流起点）
        addr0 = args.app_base + 8
        last_ack = None
        chunk_fail = 0
        last_offset = -1
        stuck = 0
        t0 = time.time()
        while offset < len(body):
            # 卡死保护：从机持续报同一个错、或报无效的期望地址时 offset 不推进，
            # 必须在同一个位置上停下来，不能无限重试
            if offset == last_offset:
                stuck += 1
                if stuck > args.retries:
                    print("★ 位置 %d 连续 %d 次无法推进，中止" % (offset, stuck))
                    return 1
            else:
                stuck = 0
                last_offset = offset

            idx = offset // BLOCK_SIZE
            data = body[offset:offset + BLOCK_SIZE]
            addr = addr0 + offset
            cum = crc32_iso(body[:offset + len(data)])
            payload = (struct.pack('<IHHHI', addr, total_pkts, idx, len(data), cum) + data)

            f = link.request(CMD_DATA, payload, timeout=args.frame_timeout,
                             retries=args.retries, label="DATA#%d" % idx)
            if f is None:
                chunk_fail += 1
                if chunk_fail > args.retries:
                    print("★ 第 %d 帧连续失败，中止" % idx)
                    return 1
                continue
            chunk_fail = 0
            code = f['data'][0]
            # 所有 DATA 应答都是同一格式：[code][cumCrc32:4][nextAddr:4]
            dev_crc, expect = (struct.unpack('<II', f['data'][1:9]) if len(f['data']) >= 9
                               else (0, 0))
            if code != 0x00:
                print("  帧 %d ← %s（期望地址 0x%08X），从该处续传"
                      % (idx, CODE_NAME.get(code, code), expect))
                if expect >= addr0 and expect < addr0 + len(body):
                    offset = expect - addr0          # 断点续传
                continue
            if dev_crc != cum:
                print("  ★ 帧 %d 从机回读 CRC32 = 0x%08X，与主机 0x%08X 不符" % (idx, dev_crc, cum))
                return 1
            last_ack = expect
            offset = expect - addr0
            if args.progress and (idx % args.progress == 0 or offset >= len(body)):
                pct = offset * 100.0 / len(body)
                print("  %5.1f%%  %6d/%d 字节" % (pct, offset, len(body)))

        print("数据传输完成，用时 %.1f 秒" % (time.time() - t0))

        # ---- END ----
        print("END（回读校验 + 提交）...")
        f = link.request(CMD_END, b"", timeout=args.end_timeout, retries=2, label="END")
        if f is None:
            print("★ END 无应答")
            return 1
        code = f['data'][0]
        written = struct.unpack('<I', f['data'][1:5])[0] if len(f['data']) >= 5 else 0
        print("  ← %s  written=%d" % (CODE_NAME.get(code, code), written))
        if code != 0x00:
            return 1

        print("\n✔ 升级完成，设备正在跳转 APP")
        if last_ack is not None:
            print("  （从机最后确认的期望地址 0x%08X = 数据末尾）" % last_ack)
        return 0
    finally:
        link.close()


# ---------------------------------------------------------------- 自检

def selftest(_args):
    """与 tools/test_protocol.cpp 完全相同的向量。"""
    fails = []

    def eq(got, want, what):
        if got != want:
            fails.append("%s: got %r want %r" % (what, got, want))

    # CRC8 向量
    eq(crc8(b""), 0x00, "crc8(空)")
    eq(crc8(bytes([1, 2, 2, 0, 0x34, 0x12])), 0x12, "crc8(示例载荷)")
    eq(crc8(bytes([0])), 0x00, "crc8(00)")
    eq(crc8(bytes([0xA5, 0x03])), 0x50, "crc8(A5 03)")
    eq(crc8(bytes(range(256))), 0x14, "crc8(00..FF)")
    eq(crc8(b"123456789"), 0xF4, 'crc8("123456789")')
    # CRC32 向量
    eq(crc32_iso(b"123456789"), 0xCBF43926, 'crc32("123456789")')
    eq(crc32_iso(b""), 0x00000000, "crc32(空)")
    eq(crc32_iso(bytes([1, 2, 3])), 0x55BC801D, "crc32(01 02 03)")

    # 组帧
    eq(encode(0x01, 0x02, bytes([0x34, 0x12])),
       bytes([0xA5, 0x01, 0x02, 0x02, 0x00, 0x34, 0x12, 0x12, 0x03]), "示例帧字节")
    eq(len(encode(0x01, 0x04, b"")), OVERHEAD, "空载荷帧长")
    eq(FRAME_MIN, 7, "FRAME_MIN")
    eq(FRAME_MAX, 1031, "FRAME_MAX")

    # 解析 + 重同步
    good = encode(0x01, 0x02, bytes([0x34, 0x12]))
    p = Parser()
    got = None
    for b in bytes([0x00, 0xA5, 0xFF, 0xA5, 0xA5]) + good:   # 与 C 侧同一组前缀
        r = p.feed(b)
        if r:
            got = r
    eq(None if got is None else got['data'], good[5:7], "垃圾字节后重同步")

    bad = bytearray(good)
    bad[3] ^= 0x01
    p = Parser()
    hit = any(p.feed(b) for b in bad)
    eq(hit, False, "CRC 不匹配被拒绝")
    hit = any(p.feed(b) for b in good)
    eq(hit, True, "坏帧后好帧可解析")

    # 满载荷且数据含 A5/03
    big = (bytes([0xA5, 0x03]) * 512)
    f2 = encode(0x01, 0x03, big)
    eq(len(f2), OVERHEAD + 1024, "满载荷帧长 1031")
    p = Parser()
    got = None
    for b in f2:
        r = p.feed(b)
        if r:
            got = r
    eq(got is not None and got['data'] == big, True, "载荷含 A5/03 可解析")

    # 帧长自检：空载荷帧 total 应等于下限；len 被改大到越界的帧应被丢弃
    f_empty = None
    p = Parser()
    for b in encode(0x01, 0x04, b""):
        r = p.feed(b)
        if r:
            f_empty = r
    eq(None if f_empty is None else f_empty['total'], FRAME_MIN, "空载荷帧 total = FRAME_MIN")

    manual = bytearray(encode(0x01, 0x02, bytes([0x5A]) * 8))
    manual[3] = 0xFF          # len 低字节
    manual[4] = 0x7F          # len 高字节 → 0x7FFF，远超 DATA_MAX
    p = Parser()
    hit = any(p.feed(b) for b in bytes(manual))
    eq(hit, False, "len 越界的帧被丢弃")

    crafted = bytearray([0xA5, 0x01, 0x02, 0xFF, 0x04, 0, 0x03])   # len=1279 但只有 1 字节
    crafted[5] = crc8(crafted[1:5])                                # 让 CRC 自洽
    p = Parser()
    hit = any(p.feed(b) for b in bytes(crafted))
    eq(hit, False, "len 越界但 CRC 自洽的帧仍被丢弃")

    print("=== Python 侧载体层自检 ===")
    if fails:
        for f in fails:
            print("  ✘", f)
        print("\n%d 项失败 → FAIL" % len(fails))
        return 1
    print("  全部通过 → PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(description="IAP 升级上位机（自定义帧协议）")
    sub = ap.add_subparsers(dest='cmd', required=True)

    s = sub.add_parser('selftest', help='只在 PC 上校验载体层，不碰串口')
    s.set_defaults(func=selftest)

    p = sub.add_parser('send', help='升级一个 bin')
    p.add_argument('file')
    p.add_argument('--port', default='COM3')
    p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--id', type=int, default=1, help='从机地址')
    p.add_argument('--app-base', type=lambda v: int(v, 0), default=0x08004000)
    p.add_argument('--recall', dest='recall', action='store_true', default=True,
                   help='先发唤回关键字（默认开）')
    p.add_argument('--no-recall', dest='recall', action='store_false')
    p.add_argument('--retries', type=int, default=5, help='同一帧重传上限')
    p.add_argument('--start-timeout', type=float, default=12.0)
    p.add_argument('--frame-timeout', type=float, default=0.5)
    p.add_argument('--end-timeout', type=float, default=5.0)
    p.add_argument('--progress', type=int, default=50, help='每 N 帧打印进度；0 = 关')
    p.set_defaults(func=send)

    args = ap.parse_args()
    if serial is None and args.func is send:
        print("★ 需要 pyserial")
        return 1
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
