#!/usr/bin/env python3
"""不同固件大小的升级耗时实测（新协议 0xA5 帧，115200 8N1）。

测什么：START（含擦除）/ DATA 逐帧 / END（整片校验 + 提交）三段各花多久，
并与理论模型对照，算出「协议效率」。

注意：除了最小的那个用真固件，其余尺寸用伪数据填充 —— 校验能过、但跳进去会崩，
所以每个用例测完都会把 APP 区前 8 字节清成 0xFF（等价 SWD 救砖），
全部跑完再刷回真固件。

用法：
    python test_proto_perf.py                 # 2KB / 64KB / 200KB
    python test_proto_perf.py --sizes 2,64,200,496
    python test_proto_perf.py --no-restore
"""

import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from proto import (Link, crc32_iso, CMD_START, CMD_DATA, CMD_END,   # noqa: E402
                   CODE_NAME, BLOCK_SIZE)

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    ConnectHelper = None

ROOT = os.path.dirname(os.path.dirname(HERE))
PORT = 'COM3'
TARGET = 'stm32f401retx'
APP_BASE = 0x08004000
APP_PY = os.path.join(ROOT, 'build', 'app_test.bin')

# 运行期波特率（由 --baud 覆盖）。固件里由宿主 usart.c 决定，必须两边一致。
BAUD = 115200

# 模型参数（与 docs/PROTOCOL_DESIGN.md 一致）
SHELL = 7
DATA_HEAD = 14
ACK_BODY = 8
WORD_US = 16.0
DATA_FRAME = SHELL + DATA_HEAD + BLOCK_SIZE            # 533
ACK_FRAME = SHELL + 1 + ACK_BODY                       # 16


# ---------------------------------------------------------------- 探针

def _session():
    return ConnectHelper.session_with_chosen_probe(target_override=TARGET, frequency=1000000,
                                                   options={'resume_on_disconnect': True})


def probe_reset():
    s = _session()
    s.open()
    s.target.reset_and_halt()
    s.target.resume()
    s.close()


def erase_sector(addr):
    """擦除 addr 所在扇区。

    注意：不能用 target.write_memory_block8() 改 Flash —— 实测对 Flash 地址
    完全不生效（Flash 只能 1→0，未经擦除写不进去）。要改只能擦扇区或烧文件。
    """
    from pyocd.flash.eraser import FlashEraser
    s = _session()
    s.open()
    try:
        s.target.halt()
        FlashEraser(s, FlashEraser.Mode.SECTOR).erase(
            ["0x%08X-0x%08X" % (addr, addr + 0x4000)])
    finally:
        s.close()


def erase_app_head():
    """让 APP 区变回「没有可启动固件」，保证下一次能进 IAP。"""
    erase_sector(APP_BASE)


# ---------------------------------------------------------------- 链路

def open_link():
    return Link(PORT, BAUD, 1, verbose=False)


def in_iap():
    link = open_link()
    try:
        f = link.request(0x04, b'', timeout=1.5, retries=2)      # CMD_STATUS
        return f is not None and f['data'][0] == 0x00
    finally:
        link.close()


def enter_iap():
    probe_reset()
    time.sleep(0.4)
    if not in_iap():
        raise RuntimeError('设备未进入 IAP')


# ---------------------------------------------------------------- 固件构造

def real_sp_pc():
    raw = open(APP_PY, 'rb').read()
    return struct.unpack('<II', raw[:8])


def make_fw(total_size, real=False):
    """造一个 total_size 字节的固件：前 8 字节用合法向量表，其余填充。"""
    if real:
        return open(APP_PY, 'rb').read()
    sp, pc = real_sp_pc()
    body = bytes((i * 37 + 11) & 0xFF for i in range(total_size - 8))   # 确定性伪数据
    return struct.pack('<II', sp, pc) + body


# ---------------------------------------------------------------- 一次升级

def upgrade_timed(raw, commit=True):
    """返回各阶段耗时 dict。commit=False 时不发 END（不提交，APP 区保持不可启动）。"""
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)
    sp, pc = struct.unpack('<II', raw[:8])

    link = open_link()
    try:
        t0 = time.time()
        f = link.request(CMD_START,
                         struct.pack('<IIII', len(raw), crc_total, sp, pc),
                         timeout=20.0, retries=1)
        t_start = time.time() - t0
        if f is None or f['data'][0] != 0x00:
            return {'ok': False, 'err': 'START %s'
                    % (CODE_NAME.get(f['data'][0], f['data'][0]) if f else '无应答')}

        offset = 0
        frames = 0
        t0 = time.time()
        while offset < len(body):
            idx = offset // BLOCK_SIZE
            chunk = body[offset:offset + BLOCK_SIZE]
            cum = crc32_iso(body[:offset + len(chunk)])
            payload = (struct.pack('<IHHHI', APP_BASE + 8 + offset, total_pkts, idx,
                                   len(chunk), cum) + chunk)
            f = link.request(CMD_DATA, payload, timeout=1.0, retries=3)
            if f is None or f['data'][0] != 0x00:
                return {'ok': False, 'err': 'DATA#%d %s'
                        % (idx, CODE_NAME.get(f['data'][0], '无应答') if f else '无应答')}
            offset = struct.unpack('<I', f['data'][5:9])[0] - APP_BASE - 8
            frames += 1
        t_data = time.time() - t0

        t_end = 0.0
        ok_end = True
        if commit:
            t0 = time.time()
            f = link.request(CMD_END, b'', timeout=10.0, retries=1)
            t_end = time.time() - t0
            ok_end = (f is not None and f['data'][0] == 0x00)
    finally:
        link.close()

    return {'ok': ok_end, 'size': len(raw), 'frames': frames,
            't_start': t_start, 't_data': t_data, 't_end': t_end,
            't_total': t_start + t_data + t_end}


# ---------------------------------------------------------------- 模型

def model(kb, block=BLOCK_SIZE):
    """理论模型：帧数 x（线上 + 写 Flash），再加上扇区擦除。"""
    body = kb * 1024 - 8
    pkts = (body + block - 1) // block
    bpm = BAUD / 10.0 / 1000.0
    per_frame_ms = (DATA_FRAME + ACK_FRAME) / bpm + (block / 4 * WORD_US / 1000.0)
    # 扇区擦除（数据手册典型值，实测约 0.4-0.5x）
    sectors = [(16, 0.4), (16, 0.4), (16, 0.4), (64, 1.0), (128, 2.0), (128, 2.0), (128, 2.0)]
    left, t_erase = kb, 0.0
    for s_kb, s_t in sectors:
        if left <= 0:
            break
        t_erase += s_t
        left -= s_kb
    return {'frames': pkts, 't_tx': pkts * per_frame_ms / 1000.0, 't_erase': t_erase,
            't_total': pkts * per_frame_ms / 1000.0 + t_erase}


def restore_app():
    raw = open(APP_PY, 'rb').read()
    enter_iap()
    r = upgrade_timed(raw, commit=True)
    return r.get('ok', False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sizes', default='2,64,200',
                    help='逗号分隔的 KB 数；2 表示用真固件（app_test.bin）')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--no-restore', action='store_true')
    args = ap.parse_args()

    if ConnectHelper is None:
        raise SystemExit('★ 需要 pyocd')

    global BAUD
    BAUD = args.baud
    sizes = [int(x) for x in args.sizes.split(',') if x.strip()]
    print('=== 升级耗时实测（%s @%d 8N1，块 %d B）===' % (PORT, BAUD, BLOCK_SIZE))
    print()
    print('  模型基线：DATA 帧 %d B + 应答 %d B，每帧线上 %.2f ms，写 Flash %.2f ms'
          % (DATA_FRAME, ACK_FRAME,
             (DATA_FRAME + ACK_FRAME) / (115200 / 10.0 / 1000.0),
             BLOCK_SIZE / 4 * WORD_US / 1000.0))
    print()

    rows = []
    for kb in sizes:
        real = (kb == 2)
        raw = make_fw(kb * 1024, real=real)
        erase_app_head()                      # 先让 APP 区不可启动，保证能进 IAP
        try:
            enter_iap()
        except RuntimeError as e:
            print('  %4d KB → ★ %s' % (kb, e))
            continue

        r = upgrade_timed(raw, commit=True)
        m = model(kb)
        if not r.get('ok'):
            print('  %4d KB → ★ 升级失败：%s' % (kb, r.get('err')))
            rows.append((kb, None, m))
            continue

        eff = (m['t_tx'] * 100.0 / r['t_data']) if r['t_data'] > 0 else 0
        print('  %4d KB（%s）：帧 %d' % (kb, '真固件' if real else '伪数据', r['frames']))
        print('        START(擦除) %6.2f s   模型 %5.2f s' % (r['t_start'], m['t_erase']))
        print('        DATA        %6.2f s   模型 %5.2f s' % (r['t_data'], m['t_tx']))
        print('        END(校验提交) %5.2f s' % r['t_end'])
        print('        合计        %6.2f s   模型 %5.2f s' % (r['t_total'], m['t_total']))
        print('        协议效率（模型传输时间 / 实测传输时间）= %.0f%%' % eff)
        print()
        rows.append((kb, r, m))

    if not args.no_restore:
        print('恢复 APP 区（刷回真固件）...')
        print('  %s' % ('✓ 已恢复' if restore_app() else '★ 恢复失败，需手动按住 PC0 重刷'))
        print()

    print('=== 汇总 ===')
    print('  %-8s %10s %10s %10s %10s' % ('大小', 'START', 'DATA', 'END', '合计'))
    for kb, r, m in rows:
        if r is None:
            print('  %-8s %10s' % ('%d KB' % kb, '★失败'))
        else:
            print('  %-8s %9.2fs %9.2fs %9.2fs %9.2fs'
                  % ('%d KB' % kb, r['t_start'], r['t_data'], r['t_end'], r['t_total']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
