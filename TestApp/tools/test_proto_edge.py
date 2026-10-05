#!/usr/bin/env python3
"""边界与畸形输入测试（新协议 0xA5 帧的广度扩展）。

与 test_proto.py 的分工：
    test_proto.py        正常路径冒烟 T1-T5
    test_proto_edge.py   边界 / 畸形输入 E1-Ex（本文件）
    test_proto_perf.py   不同固件大小的耗时实测

每条 E 用例都断两件事：
    1. 设备给不给应答、给的是哪个错误码
    2. **APP 区有没有被动过**（读回前 8 字节比对）—— 这是「不会变砖」的直接证据

用法：
    python test_proto_edge.py              # 全部
    python test_proto_edge.py --only E1
    python test_proto_edge.py --keep-after # 跑完不恢复 APP 区（调试用）
"""

import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import serial                                                       # noqa: E402
from proto import (Link, Parser, encode, crc8, crc32_iso, CMD_START, CMD_DATA,
                   CMD_END, CMD_STATUS, CODE_NAME, BLOCK_SIZE, DATA_HEAD,
                   START_LEN)                                       # noqa: E402

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    ConnectHelper = None

ROOT = os.path.dirname(os.path.dirname(HERE))
PORT = 'COM3'
TARGET = 'stm32f401retx'
APP_BASE = 0x08004000
APP_SIZE = 496 * 1024                      # F401: 0x08080000 - 0x08004000
APP_PY = os.path.join(ROOT, 'build', 'app_test.bin')

RESULTS = []


def record(tag, what, ok, detail=''):
    RESULTS.append((tag, what, ok, detail))
    print('  [%s] %-32s %s %s' % (tag, what, 'PASS' if ok else '★FAIL', detail))


# ---------------------------------------------------------------- 探针

def probe_reset():
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


def erase_sector(addr):
    """擦除 addr 所在的 Flash 扇区（pyocd 的 FlashEraser）。

    ⚠️ 不能用 target.write_memory_block8() 改 Flash —— 实测它对本芯片的 Flash
    地址完全不生效（写完读回内容不变）：Flash 只能 1→0，未经擦除的写入本来就
    不可能成功，而 pyocd 的内存写路径也不会自动走 Flash 编程算法。
    要改 Flash 只有两条路：擦除扇区，或用 FileProgrammer 烧 hex/bin。
    """
    from pyocd.flash.eraser import FlashEraser
    s = ConnectHelper.session_with_chosen_probe(target_override=TARGET, frequency=1000000,
                                                options={'resume_on_disconnect': True})
    s.open()
    try:
        s.target.halt()
        FlashEraser(s, FlashEraser.Mode.SECTOR).erase(["0x%08X-0x%08X" % (addr, addr + 0x4000)])
    finally:
        s.close()


def app_head():
    """APP 区最前 8 字节 = 向量表前两个字（SP / PC）。"""
    return struct.unpack('<II', read_mem(APP_BASE, 8))


def app_head_hex():
    return ' '.join('%02X' % b for b in read_mem(APP_BASE, 8))


def erase_app_first_sector():
    """擦掉 APP 区第一个扇区 → 前 8 字节变 0xFF = 没有可启动固件。

    等价于「SWD 救砖」，人工版本是按住 PC0 上电后用上位机重刷。
    """
    erase_sector(APP_BASE)


# ---------------------------------------------------------------- 组帧

def open_link(device_id=1):
    return Link(PORT, 115200, device_id, verbose=False)


def raw_send(link, frame_bytes, wait=0.8):
    """发原始字节，收集 wait 秒内到达的所有应答帧（用于「预期无应答」的用例）。"""
    link.flush()
    link.ser.write(frame_bytes)
    link.ser.flush()
    frames = []
    deadline = time.time() + wait
    while time.time() < deadline:
        b = link.ser.read(1)
        if not b:
            continue
        f = link.parser.feed(b[0])
        if f is not None:
            frames.append(f)
    return frames


def raw_frame(device_id, cmd, data, force_len=None, bad_crc=False, tail=0x03):
    """手工组帧：可以强制一个与实际长度不符的 DataLen、或写坏 CRC8。"""
    n = len(data) if force_len is None else force_len
    core = bytes([device_id, cmd]) + struct.pack('<H', n) + bytes(data)
    c = crc8(core)
    if bad_crc:
        c ^= 0xFF
    return bytes([0xA5]) + core + bytes([c, tail])


def in_iap():
    """无副作用的冒烟检查：设备当前是否在 IAP 且协议通路正常。"""
    link = open_link()
    try:
        f = link.request(CMD_STATUS, b'', timeout=1.5, retries=2)
        return f is not None and f['data'][0] == 0x00
    finally:
        link.close()


def enter_iap():
    """复位进 IAP（探针复位会置 SFTRSTF → 走 15 秒唤回窗口）。"""
    probe_reset()
    time.sleep(0.4)
    if not in_iap():
        raise SystemExit('★ 设备未进入 IAP（STATUS 无应答）')


# ---------------------------------------------------------------- 用例

def expect_reply(tag, what, core_bytes, expect_code, cmd_reply):
    """发帧 → 期望一个指定错误码的应答；同时断言 APP 区未被动过。"""
    before = app_head_hex()
    enter_iap()
    link = open_link()
    try:
        frames = raw_send(link, core_bytes, wait=1.2)
    finally:
        link.close()
    after = app_head_hex()

    hit = [f for f in frames if f['cmd'] == cmd_reply]
    code = hit[0]['data'][0] if hit else None
    untouched = (before == after)
    ok = (code == expect_code) and untouched
    record(tag, what, ok, '码=%s(期望 %s) APP区未动=%s'
           % (CODE_NAME.get(code, code) if code is not None else '无应答',
              CODE_NAME.get(expect_code, expect_code), untouched))


def expect_silent(tag, what, core_bytes, wait=1.0):
    """发帧 → 期望设备完全无应答（载体层丢弃 / ID 不匹配）。"""
    before = app_head_hex()
    enter_iap()
    link = open_link()
    try:
        frames = raw_send(link, core_bytes, wait=wait)
    finally:
        link.close()
    after = app_head_hex()
    ok = (len(frames) == 0) and (before == after)
    record(tag, what, ok, '应答数=%d APP区未动=%s' % (len(frames), before == after))


def start_payload(size, crc, sp, pc):
    return struct.pack('<IIII', size, crc, sp, pc)


def real_sp_pc():
    raw = open(APP_PY, 'rb').read()
    return struct.unpack('<II', raw[:8])


# ---- E1 超大固件 -----------------------------------------------------------

def E1_oversize():
    """固件体积超过 APP 区 → START 必须拒绝，且**不擦除**。

    比对「发 START 前后 APP 区前 8 字节是否一致」；main 已保证进来时
    APP 区里是可启动固件，所以只要被擦过就一定能看出来。
    """
    sp, pc = real_sp_pc()
    oversize = APP_SIZE + 4096
    enter_iap()
    before = app_head_hex()
    link = open_link()
    try:
        f = link.request(CMD_START, start_payload(oversize, 0x12345678, sp, pc),
                         timeout=3.0, retries=1)
    finally:
        link.close()
    code = f['data'][0] if f else None
    after = app_head_hex()
    meaningful = (before != 'FF FF FF FF FF FF FF FF')
    untouched = (before == after) and meaningful
    record('E1', '超大固件被拒且不擦除', code == 0x05 and untouched,
           'size=%d 码=%s 前后一致=%s(比对有效=%s)'
           % (oversize, CODE_NAME.get(code, code) if code is not None else '无应答',
              before == after, meaningful))


# ---- E2 极小固件 -----------------------------------------------------------

def E2_tiny():
    """size < 16 → 拒绝（连向量表都不看）。"""
    sp, pc = real_sp_pc()
    expect_reply('E2', '极小固件(size=8)被拒',
                 raw_frame(1, CMD_START, start_payload(8, 0, sp, pc)),
                 0x05, CMD_START | 0x80)


# ---- E3 向量表非法 ---------------------------------------------------------

def E3_bad_vectors():
    """SP = 0（不在 SRAM 区）→ 拒绝，且在擦除之前。"""
    _, pc = real_sp_pc()
    expect_reply('E3', '向量表非法(SP=0)被拒',
                 raw_frame(1, CMD_START, start_payload(4096, 0x1111, 0x00000000, pc)),
                 0x0C, CMD_START | 0x80)

    # 再补一个 PC 不对齐的（落在 APP 区内但最低位为 0）
    expect_reply('E3b', '向量表非法(PC 非 Thumb)被拒',
                 raw_frame(1, CMD_START, start_payload(4096, 0x1111, 0x20000000, APP_BASE + 4)),
                 0x0C, CMD_START | 0x80)


# ---- E4 START 载荷不足 -----------------------------------------------------

def E4_short_start():
    """START 载荷少于 16 字节 → 参数错误。"""
    expect_reply('E4', 'START 载荷不足被拒',
                 raw_frame(1, CMD_START, b'\x00' * 8), 0x05, CMD_START | 0x80)


# ---- E5 未知命令 -----------------------------------------------------------

def E5_unknown_cmd():
    """载体能解析、但命令层不认识 → 未知命令。"""
    expect_reply('E5', '未知命令被拒',
                 raw_frame(1, 0x7E, b'\x01\x02'), 0x04, 0x7E | 0x80)


# ---- E6 ID 不匹配（静默） --------------------------------------------------

def E6_wrong_id():
    """ID 不是本机 → 完全静默（连错误都不回），避免在多从机总线上撞车。"""
    sp, pc = real_sp_pc()
    expect_silent('E6', '非本机 ID 被静默丢弃',
                  raw_frame(2, CMD_START, start_payload(4096, 0x1111, sp, pc)))


# ---- E7 无 START 直接 DATA ------------------------------------------------

def E7_data_without_start():
    """没 START 就发数据 → 状态错误。"""
    enter_iap()
    link = open_link()
    try:
        # 先确保不是 Receiving 态：设备刚复位，状态是 Idle
        payload = struct.pack('<IHHHI', APP_BASE + 8, 1, 0, BLOCK_SIZE, 0) + bytes(BLOCK_SIZE)
        f = link.request(CMD_DATA, payload, timeout=1.0, retries=1)
    finally:
        link.close()
    code = f['data'][0] if f else None
    record('E7', '未 START 直接 DATA 被拒', code == 0x06,
           '码=%s' % (CODE_NAME.get(code, code) if code is not None else '无应答'))


# ---- E8 总 CRC32 不符 ------------------------------------------------------

def E8_total_crc_mismatch():
    """数据全对、但 START 声明的总 CRC32 是错的 → END 必须拒绝且不提交。"""
    raw = open(APP_PY, 'rb').read()
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    sp, pc = real_sp_pc()
    wrong_crc = crc32_iso(body) ^ 0xFFFFFFFF

    enter_iap()
    link = open_link()
    try:
        f = link.request(CMD_START, start_payload(len(raw), wrong_crc, sp, pc),
                         timeout=8.0, retries=1)
        if f is None or f['data'][0] != 0x00:
            record('E8', '总 CRC32 不符被拒', False, 'START 失败')
            return
        offset = 0
        while offset < len(body):
            idx = offset // BLOCK_SIZE
            chunk = body[offset:offset + BLOCK_SIZE]
            cum = crc32_iso(body[:offset + len(chunk)])
            payload = (struct.pack('<IHHHI', APP_BASE + 8 + offset, total_pkts, idx,
                                   len(chunk), cum) + chunk)
            f = link.request(CMD_DATA, payload, timeout=0.5, retries=2)
            if f is None or f['data'][0] != 0x00:
                record('E8', '总 CRC32 不符被拒', False, 'DATA#%d 失败' % idx)
                return
            offset = struct.unpack('<I', f['data'][5:9])[0] - APP_BASE - 8
        f = link.request(CMD_END, b'', timeout=5.0, retries=1)
    finally:
        link.close()

    code = f['data'][0] if f else None
    head = app_head()
    not_committed = (head == (0xFFFFFFFF, 0xFFFFFFFF))
    record('E8', '总 CRC32 不符被拒', code == 0x0D and not_committed,
           'END 码=%s 未提交=%s SP/PC=0x%08X/0x%08X'
           % (CODE_NAME.get(code, code) if code is not None else '无应答',
              not_committed, head[0], head[1]))


# ---- E9/E10/E12 冗余字段不一致 --------------------------------------------

def _start_small(link, size, crc=0x12345678):
    sp, pc = real_sp_pc()
    return link.request(CMD_START, start_payload(size, crc, sp, pc), timeout=8.0, retries=1)


def E9_vlen_mismatch():
    """vlen 与实际数据长度不符 → 冗余字段不一致。"""
    enter_iap()
    link = open_link()
    try:
        f = _start_small(link, 2048)
        if f is None or f['data'][0] != 0x00:
            record('E9', 'vlen 与实际长度不符被拒', False, 'START 失败')
            return
        chunk = bytes(BLOCK_SIZE)
        payload = (struct.pack('<IHHHI', APP_BASE + 8, 4, 0, BLOCK_SIZE - 10,
                               crc32_iso(chunk[:BLOCK_SIZE - 10])) + chunk)
        f = link.request(CMD_DATA, payload, timeout=1.0, retries=1)
    finally:
        link.close()
    code = f['data'][0] if f else None
    record('E9', 'vlen 与实际长度不符被拒', code == 0x0E,
           '码=%s' % (CODE_NAME.get(code, code) if code is not None else '无应答'))


def E10_vlen_zero():
    """vlen = 0 的数据帧 → 拒绝（空数据帧没有意义）。"""
    enter_iap()
    link = open_link()
    try:
        f = _start_small(link, 2048)
        if f is None or f['data'][0] != 0x00:
            record('E10', 'vlen=0 的数据帧被拒', False, 'START 失败')
            return
        payload = struct.pack('<IHHHI', APP_BASE + 8, 4, 0, 0, 0)
        f = link.request(CMD_DATA, payload, timeout=1.0, retries=1)
    finally:
        link.close()
    code = f['data'][0] if f else None
    record('E10', 'vlen=0 的数据帧被拒', code == 0x0E,
           '码=%s' % (CODE_NAME.get(code, code) if code is not None else '无应答'))


def E12_index_mismatch():
    """index 与 addr 推算值不符 → 冗余字段不一致。"""
    enter_iap()
    link = open_link()
    try:
        f = _start_small(link, 2048)
        if f is None or f['data'][0] != 0x00:
            record('E12', 'index 与 addr 不符被拒', False, 'START 失败')
            return
        chunk = bytes(BLOCK_SIZE)
        payload = (struct.pack('<IHHHI', APP_BASE + 8, 4, 7, BLOCK_SIZE,
                               crc32_iso(chunk)) + chunk)   # index 应为 0，故意写 7
        f = link.request(CMD_DATA, payload, timeout=1.0, retries=1)
    finally:
        link.close()
    code = f['data'][0] if f else None
    record('E12', 'index 与 addr 不符被拒', code == 0x0E,
           '码=%s' % (CODE_NAME.get(code, code) if code is not None else '无应答'))


# ---- E11 地址越界 ----------------------------------------------------------

def E11_addr_out_of_range():
    """addr + vlen 超出 START 声明的范围 → 地址错误（0x03）。

    注意要落在下一关之前：addr 必须等于期望地址（否则先触 0x07），
    index 必须与 addr 一致（否则先触 0x0E）。
    """
    enter_iap()
    link = open_link()
    try:
        # 声明只有 256 字节数据，却发 512 字节的帧
        f = _start_small(link, 8 + 256)
        if f is None or f['data'][0] != 0x00:
            record('E11', 'addr+vlen 越界被拒', False, 'START 失败')
            return
        chunk = bytes(BLOCK_SIZE)
        payload = (struct.pack('<IHHHI', APP_BASE + 8, 1, 0, BLOCK_SIZE,
                               crc32_iso(chunk)) + chunk)
        f = link.request(CMD_DATA, payload, timeout=1.0, retries=1)
    finally:
        link.close()
    code = f['data'][0] if f else None
    record('E11', 'addr+vlen 越界被拒', code == 0x03,
           '码=%s' % (CODE_NAME.get(code, code) if code is not None else '无应答'))


# ---- E13/E14 载体层丢弃 ----------------------------------------------------

def E13_len_oversize():
    """DataLen 超过载体上限 1024 → 载体层立即丢帧，设备完全无应答。"""
    expect_silent('E13', 'DataLen>1024 被载体层丢弃',
                  raw_frame(1, CMD_START, b'\x00' * 32, force_len=2000))


def E14_bad_crc8():
    """帧 CRC8 错 → 载体层丢帧，无应答。"""
    sp, pc = real_sp_pc()
    expect_silent('E14', '帧 CRC8 错被载体层丢弃',
                  raw_frame(1, CMD_START, start_payload(4096, 0x1111, sp, pc), bad_crc=True))


# ---- E15 提交后崩（会污染 APP 区，放最后）----------------------------------

def E15_commits_then_crashes():
    """「能过全部校验、但一跑就崩」的固件 → 会被提交、会跳进去崩。

    这是唯一「校验拦不住」的一类（内容合法但逻辑坏）。本用例要证明的是：
    **即使这样也不会永久变砖** —— 因为 Bootloader 自己不可被覆盖，
    APP 区前 8 字节随时可以被清掉（PC0 按键上电 / SWD）重新进 IAP。
    """
    sp, pc = real_sp_pc()
    # 前 8 字节用真固件的合法向量表，其余用伪数据 —— 校验全过，但一跑就崩
    body = b'\xAA' * (1024 - 8)
    raw = struct.pack('<II', sp, pc) + body
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)

    enter_iap()
    link = open_link()
    try:
        f = link.request(CMD_START, start_payload(len(raw), crc_total, sp, pc),
                         timeout=8.0, retries=1)
        if f is None or f['data'][0] != 0x00:
            record('E15', '坏固件被提交(校验拦不住)', False, 'START 失败')
            return
        offset = 0
        while offset < len(body):
            idx = offset // BLOCK_SIZE
            chunk = body[offset:offset + BLOCK_SIZE]
            cum = crc32_iso(body[:offset + len(chunk)])
            payload = (struct.pack('<IHHHI', APP_BASE + 8 + offset, total_pkts, idx,
                                   len(chunk), cum) + chunk)
            f = link.request(CMD_DATA, payload, timeout=0.5, retries=2)
            if f is None or f['data'][0] != 0x00:
                record('E15', '坏固件被提交(校验拦不住)', False, 'DATA#%d 失败' % idx)
                return
            offset = struct.unpack('<I', f['data'][5:9])[0] - APP_BASE - 8
        f = link.request(CMD_END, b'', timeout=5.0, retries=1)
    finally:
        link.close()

    code = f['data'][0] if f else None
    committed = (app_head() == (sp, pc))

    # 复位 → Bootloader 会因向量表合法而跳进这个坏固件
    probe_reset()
    time.sleep(1.5)
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.05)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < 2.5:
            n = ser.in_waiting
            buf += ser.read(n) if n else (ser.read(1) or b'')
            time.sleep(0.01)
        ser.close()
    except Exception:
        buf = b''
    alive = buf.count(b'alive')

    # 恢复：擦掉 APP 区第一个扇区 → 前 8 字节变 0xFF（= 没有可启动固件）
    erase_app_first_sector()
    head_after = app_head()
    erased = (head_after == (0xFFFFFFFF, 0xFFFFFFFF))

    # 复位，抓启动日志确认走的是「没有可启动固件」这条路，
    # 而不是「软件复位 → 15 秒窗口」（探针复位会置 SFTRSTF，两者容易混淆）
    ser = serial.Serial(PORT, 115200, timeout=0.05)
    ser.reset_input_buffer()
    probe_reset()
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < 2.0:
        n = ser.in_waiting
        buf += ser.read(n) if n else (ser.read(1) or b'')
        time.sleep(0.01)
    ser.close()
    log = buf.decode('utf-8', 'replace')
    decision = 'no bootable firmware' in log

    ok = (code == 0x00) and committed and alive == 0 and erased and decision
    record('E15', '坏固件被提交但不变砖', ok,
           'END=%s 已提交=%s 崩=%s 已擦回0xFF=%s 判无固件=%s'
           % (CODE_NAME.get(code, code) if code is not None else '无应答',
              committed, alive == 0, erased, decision))


def restore_app():
    """恢复：完整刷一个真固件回去。"""
    raw = open(APP_PY, 'rb').read()
    body = raw[8:]
    total_pkts = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    crc_total = crc32_iso(body)
    sp, pc = struct.unpack('<II', raw[:8])

    enter_iap()
    link = open_link()
    try:
        f = link.request(CMD_START, start_payload(len(raw), crc_total, sp, pc),
                         timeout=8.0, retries=1)
        if f is None or f['data'][0] != 0x00:
            return False
        offset = 0
        while offset < len(body):
            idx = offset // BLOCK_SIZE
            chunk = body[offset:offset + BLOCK_SIZE]
            cum = crc32_iso(body[:offset + len(chunk)])
            payload = (struct.pack('<IHHHI', APP_BASE + 8 + offset, total_pkts, idx,
                                   len(chunk), cum) + chunk)
            f = link.request(CMD_DATA, payload, timeout=0.5, retries=3)
            if f is None or f['data'][0] != 0x00:
                return False
            offset = struct.unpack('<I', f['data'][5:9])[0] - APP_BASE - 8
        f = link.request(CMD_END, b'', timeout=5.0, retries=1)
        return f is not None and f['data'][0] == 0x00
    finally:
        link.close()


CASES = [
    ('E1', E1_oversize), ('E2', E2_tiny), ('E3', E3_bad_vectors),
    ('E4', E4_short_start), ('E5', E5_unknown_cmd), ('E6', E6_wrong_id),
    ('E7', E7_data_without_start), ('E8', E8_total_crc_mismatch),
    ('E9', E9_vlen_mismatch), ('E10', E10_vlen_zero),
    ('E11', E11_addr_out_of_range), ('E12', E12_index_mismatch),
    ('E13', E13_len_oversize), ('E14', E14_bad_crc8),
    ('E15', E15_commits_then_crashes),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--keep-after', action='store_true', help='跑完不恢复 APP 区（调试用）')
    args = ap.parse_args()

    if ConnectHelper is None:
        raise SystemExit('★ 需要 pyocd')

    print('=== 边界与畸形输入测试（%s）===' % PORT)
    print('前置：APP 区里有一个可启动固件（找不到先跑 test_proto.py）')
    print('  当前 APP 向量表: %s' % app_head_hex())
    print()

    # 前置：E1/E2/E3 等用例靠「前后比对」来判断 APP 区有没有被动过，
    # 若 APP 区本来就是擦空态（例如上一步跑了 T3/T5），比对就失去意义。
    # 所以先确保里面有一个可启动的固件。
    head = app_head()
    head_ok = (0x20000000 <= head[0] < 0x20100000 and (head[0] & 7) == 0
               and APP_BASE <= head[1] < APP_BASE + APP_SIZE and (head[1] & 1) != 0)
    if not head_ok:
        print('  APP 区不可启动（SP=0x%08X PC=0x%08X），先刷回真固件作为比对基准 ...'
              % head)
        if restore_app():
            print('  ✓ 已刷入基准固件')
        else:
            print('  ⚠ 刷入失败（继续跑，但 E1/E2/E3 的比对结果不可信）')
        print()

    # 冒烟：先确认设备在 IAP 且协议通路正常，否则后面的边界测试没意义
    if not in_iap():
        print('  ⚠ 设备不在 IAP（STATUS 无应答），先复位进 IAP ...')
        try:
            enter_iap()
        except SystemExit as e:
            print(e)
            return 1
    print('  冒烟检查：STATUS 应答正常，设备在 IAP ✓')
    print()

    any_crash = False
    for tag, fn in CASES:
        if args.only and args.only != tag:
            continue
        try:
            fn()
        except Exception as e:
            record(tag, fn.__doc__.split('\n')[0] if fn.__doc__ else tag, False,
                   '异常 %s: %s' % (type(e).__name__, str(e)[:90]))
            any_crash = True

    if not args.keep_after:
        print()
        print('恢复 APP 区（完整刷回真固件）...')
        ok = restore_app()
        print('  %s' % ('✓ 已恢复' if ok else '★ 恢复失败，需手动按住 PC0 重刷'))

    npass = sum(1 for _, _, ok, _ in RESULTS if ok)
    print('\n%d/%d 通过 → %s' % (npass, len(RESULTS), 'PASS' if npass == len(RESULTS) else 'FAIL'))
    return 0 if npass == len(RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
