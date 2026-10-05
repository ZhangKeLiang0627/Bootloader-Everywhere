#!/usr/bin/env python3
"""新协议（0xA5 帧）的板端回归测试。

用法：
    python test_proto.py                 # 跑全部
    python test_proto.py --only T1

前置：板上已烧好当前 Bootloader；APP 区里有一个可启动的测试固件。
"""

import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import serial                                                    # noqa: E402
from proto import (Link, Parser, encode, crc32_iso, CMD_START, CMD_DATA,
                   CMD_END, CODE_NAME, BLOCK_SIZE, DATA_HEAD)    # noqa: E402

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    ConnectHelper = None

ROOT = os.path.dirname(os.path.dirname(HERE))
PORT = 'COM3'
TARGET = 'stm32f401retx'
APP_BASE = 0x08004000
APP_PY = os.path.join(ROOT, 'build', 'app_test.bin')

RESULTS = []


def record(tag, what, ok, detail=''):
    RESULTS.append((tag, what, ok, detail))
    print('  [%s] %-30s %s %s' % (tag, what, 'PASS' if ok else '★FAIL', detail))


def probe_reset():
    """探针复位。会置 SFTRSTF → Bootloader 判为软件复位 → 进 15 秒唤回窗口。"""
    s = ConnectHelper.session_with_chosen_probe(target_override=TARGET, frequency=1000000,
                                                options={'resume_on_disconnect': True})
    s.open()
    s.target.reset_and_halt()
    s.target.resume()
    s.close()


def read_mem(addr, n):
    s = ConnectHelper.session_with_chosen_probe(target_override=TARGET, frequency=1000000,
                                                options={'resume_on_disconnect': True})
    s.open()
    t = s.target
    t.halt()
    data = bytes(t.read_memory_block8(addr, n))
    t.resume()
    s.close()
    return data


def find_app_py():
    for p in (APP_PY, os.path.join(ROOT, 'build', 'app_fail1.bin')):
        if os.path.exists(p):
            return p
    raise SystemExit('★ 找不到测试固件，先跑 TestApp/build_app.py')


def do_start(link, raw, crc_total, timeout=12.0):
    payload = struct.pack('<IIII', len(raw), crc_total,
                          struct.unpack('<I', raw[:4])[0],
                          struct.unpack('<I', raw[4:8])[0])
    return link.request(CMD_START, payload, timeout=timeout, retries=2, label='START')


def do_end(link, timeout=5.0):
    return link.request(CMD_END, b'', timeout=timeout, retries=2, label='END')


def data_frame(link, body, offset, total_pkts, corrupt=False, jump=False):
    idx = offset // BLOCK_SIZE
    chunk = bytearray(body[offset:offset + BLOCK_SIZE])
    addr = APP_BASE + 8 + offset
    if jump:
        addr += BLOCK_SIZE
    cum = crc32_iso(body[:offset + len(chunk)])
    if corrupt:
        chunk[0] ^= 0xFF                       # 数据被改，但 cumCrc32 没跟着改
    payload = struct.pack('<IHHHI', addr, total_pkts, idx, len(chunk), cum) + bytes(chunk)
    return link.request(CMD_DATA, payload, timeout=0.5, retries=1, label='DATA#%d' % idx)


def full_upgrade(link, raw, verbose=False):
    """一次完整升级；返回 (ok, 帧数)。"""
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)

    f = do_start(link, raw, crc_total)
    if f is None or f['data'][0] != 0x00:
        return False, 0

    offset = 0
    frames = 0
    while offset < len(body):
        f = data_frame(link, body, offset, total_pkts)
        if f is None or f['data'][0] != 0x00:
            return False, frames
        dev_crc, expect = struct.unpack('<II', f['data'][1:9])
        if dev_crc != crc32_iso(body[:expect - APP_BASE - 8]):
            return False, frames
        offset = expect - APP_BASE - 8
        frames += 1

    f = do_end(link)
    ok = (f is not None and f['data'][0] == 0x00)
    return ok, frames


def open_link():
    return Link(PORT, 115200, 1, verbose=False)


# ---------------------------------------------------------------- 用例

def T1_normal():
    """正常升级：从 APP 唤回 → 完整升级 → 跳 APP 并运行。"""
    raw = open(find_app_py(), 'rb').read()
    probe_reset()
    time.sleep(0.3)
    link = open_link()
    try:
        link.recall()                       # 已在 IAP，这些字节会被解析器当噪声忽略
        ok, frames = full_upgrade(link, raw)
    finally:
        link.close()
    time.sleep(1.5)
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.05)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < 3.0:
            n = ser.in_waiting
            buf += ser.read(n) if n else (ser.read(1) or b'')
            time.sleep(0.01)
        ser.close()
    except Exception:
        buf = b''
    alive = buf.count(b'alive')
    record('T1', '正常升级', ok and alive >= 2, 'frames=%d alive=%d' % (frames, alive))


def T2_repeat5():
    """连续升级 5 次，验证状态机每次都能重来。"""
    raw = open(find_app_py(), 'rb').read()
    oks = 0
    for i in range(5):
        probe_reset()
        time.sleep(0.3)
        link = open_link()
        try:
            link.flush()
            ok, _ = full_upgrade(link, raw)
        finally:
            link.close()
        if ok:
            oks += 1
        time.sleep(0.4)
    record('T2', '连续升级 x5', oks == 5, '%d/5' % oks)


def T3_abort_midway():
    """START 后只发 2 帧就放弃 → 复位后必须停在 IAP、SP/PC 仍为 0xFFFFFFFF。"""
    raw = open(find_app_py(), 'rb').read()
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)

    probe_reset()
    time.sleep(0.3)
    link = open_link()
    try:
        link.flush()
        f = do_start(link, raw, crc_total)
        if f is None or f['data'][0] != 0x00:
            record('T3', '传输中断不变砖', False, 'START 失败')
            return
        for off in (0, BLOCK_SIZE):
            data_frame(link, body, off, total_pkts)
    finally:
        link.close()

    time.sleep(0.5)
    probe_reset()
    time.sleep(1.5)
    head = struct.unpack('<II', read_mem(APP_BASE, 8))
    record('T3', '传输中断不变砖', head == (0xFFFFFFFF, 0xFFFFFFFF),
           'SP=0x%08X PC=0x%08X' % head)


def T4_corrupt_frame_rejected():
    """一帧数据被篡改 → 设备必须拒绝（0x08）且不写入；随后重发正确数据仍能成功。"""
    raw = open(find_app_py(), 'rb').read()
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)

    probe_reset()
    time.sleep(0.3)
    link = open_link()
    try:
        link.flush()
        f = do_start(link, raw, crc_total)
        if f is None or f['data'][0] != 0x00:
            record('T4', '篡改帧被拒且可恢复', False, 'START 失败')
            return

        offset = 0
        ok_all = True
        corrupt_done = False
        bad_code = None
        not_advanced = None

        while offset < len(body):
            idx = offset // BLOCK_SIZE
            if idx == 1 and not corrupt_done:
                # 这一帧先发被篡改的版本 → 期望 0x08，且从机状态不得推进
                f = data_frame(link, body, offset, total_pkts, corrupt=True)
                bad_code = f['data'][0] if f else None
                if f:
                    dev_crc = struct.unpack('<I', f['data'][1:5])[0]
                    not_advanced = (dev_crc == crc32_iso(body[:offset]))
                corrupt_done = True

            f = data_frame(link, body, offset, total_pkts)      # 正确版本
            if f is None or f['data'][0] != 0x00:
                ok_all = False
                break
            offset = struct.unpack('<I', f['data'][5:9])[0] - APP_BASE - 8

        f = do_end(link)
        ok_end = f is not None and f['data'][0] == 0x00
    finally:
        link.close()

    ok = ok_all and bad_code == 0x08 and not_advanced and ok_end
    record('T4', '篡改帧被拒且可恢复', ok,
           '坏帧码=0x%02X 状态未推进=%s 整片完成=%s 收尾=%s'
           % (bad_code or 0, not_advanced, ok_all, ok_end))


def T5_addr_gap():
    """跳号帧 → 设备回 0x07 并给出期望地址，主机据此续传。"""
    raw = open(find_app_py(), 'rb').read()
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)

    probe_reset()
    time.sleep(0.3)
    link = open_link()
    try:
        link.flush()
        f = do_start(link, raw, crc_total)
        if f is None or f['data'][0] != 0x00:
            record('T5', '跳号被拒并给出续传点', False, 'START 失败')
            return

        f = data_frame(link, body, 0, total_pkts)
        ok0 = f is not None and f['data'][0] == 0x00

        # 跳过第 1 帧，直接发第 2 帧
        f = data_frame(link, body, 2 * BLOCK_SIZE, total_pkts, jump=True)
        code = f['data'][0] if f else None
        want = struct.unpack('<I', f['data'][1:5])[0] if f else 0
        expect = APP_BASE + 8 + BLOCK_SIZE
    finally:
        link.close()

    ok = ok0 and code == 0x07 and want == expect
    record('T5', '跳号被拒并给出续传点', ok,
           '码=0x%02X 期望=0x%08X (应为 0x%08X)' % (code or 0, want, expect))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    args = ap.parse_args()

    if ConnectHelper is None:
        raise SystemExit('★ 需要 pyocd')

    cases = [T1_normal, T2_repeat5, T3_abort_midway, T4_corrupt_frame_rejected, T5_addr_gap]
    print('=== Bootloader-Everywhere 协议回归（%s）===' % PORT)
    for c in cases:
        tag = c.__name__.split('_')[0]
        if args.only and args.only != tag:
            continue
        try:
            c()
        except Exception as e:
            record(tag, c.__doc__.split('\n')[0] if c.__doc__ else tag, False,
                   '异常 %s: %s' % (type(e).__name__, str(e)[:80]))

    npass = sum(1 for _, _, ok, _ in RESULTS if ok)
    print('\n%d/%d 通过 → %s' % (npass, len(RESULTS), 'PASS' if npass == len(RESULTS) else 'FAIL'))
    return 0 if npass == len(RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
