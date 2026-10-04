#!/usr/bin/env python3
"""
断电暴力测试引导 —— LUMOS-bootloader

手动配合的断电测试：脚本在每个「断电机时」停下并醒目提示「现在断电」，
你在看到提示的瞬间拔掉板子电源（或 USB），数 2 秒再插回。
脚本检测到重新启动后，自动读取状态并判定是否变砖。

用法（每个时机单独跑一次，共 5 次）：
    python power_test.py --phase 1   首包 ACK 后（擦除刚完成）
    python power_test.py --phase 2   数据传输中
    python power_test.py --phase 3   传输完成、commit 前后
    python power_test.py --phase 4   升级后首次启动（TESTING 态）
    python power_test.py --phase 5   APP 稳定运行中

预期（每个时机断电再上电后都不应变砖）：
    phase 1/2 → state=DOWNLOAD，停在 IAP 可重刷
    phase 3   → state=DOWNLOAD 或 TESTING，停在 IAP 或首次启动
    phase 4/5 → clean boot 转 VALID 或直接跳 APP

依赖：pyserial + pyocd。串口默认 COM3，目标 stm32f401retx。
"""

import argparse
import os
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from board_test import (DEFAULT_BAUD, open_probe)  # noqa: E402
from ymodem_send import (YmodemSender, SOH, STX, ACK, CRC_REQ,
                         crc16_xmodem, PKT_128)  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
APP = os.path.join(ROOT, "build", "app_test.bin")
PORT = "COM3"
TARGET = "stm32f401retx"
REQ_ADDR = 0x20017FFC
REQ_MAGIC = 0xB007B007


def banner(msg):
    print()
    print("=" * 66)
    print("  " + msg)
    print("=" * 66)


def power_prompt(why):
    """醒目地提示用户断电，并等待重新上电（检测 bootloader banner 重启）。"""
    print()
    print("  ██████████████████████████████████████████████████████")
    print("  ██  ⏸  现在断电！  " + why)
    print("  ██  拔掉板子电源（或 USB），数 2 秒，再插回。")
    print("  ██████████████████████████████████████████████████████")
    print()

    # 重新打开串口，清掉断电前残留，等待「重新上电的 banner」
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    ser.reset_input_buffer()
    buf = bytearray()
    t0 = time.time()
    timeout = 60.0
    saw_banner = False
    while time.time() - t0 < timeout:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n)
            buf += chunk
            if b"LUMOS-bootloader" in buf:
                saw_banner = True
                # banner 出现后再读 2 秒，收集完整的 decide 日志
                time.sleep(2.0)
                buf += ser.read(ser.in_waiting)
                break
        else:
            b = ser.read(1)
            if b:
                buf += b
        time.sleep(0.02)
    txt = buf.decode("utf-8", "replace")
    if not saw_banner:
        print("  ★ 60 秒内未检测到重新上电（bootloader banner）。")
    print("  --- 重新上电后的输出 ---")
    for line in txt.splitlines():
        if line.strip():
            print("  |", line.strip())
    ser.close()
    return txt, saw_banner


def enter_iap_ram_flag():
    """用 RAM 标志进 IAP（backdoor 已默认关闭）。"""
    s = open_probe(TARGET)
    try:
        s.open()
        t = s.target
        t.halt()
        t.write32(REQ_ADDR, REQ_MAGIC)
        t.resume()
    finally:
        s.close()
    # 复位
    s = open_probe(TARGET)
    try:
        s.open()
        s.target.reset_and_halt()
        s.target.resume()
    finally:
        s.close()
    time.sleep(1.5)


def wait_byte(ser, wanted, timeout=8.0):
    deadline = time.time() + timeout
    noise = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == wanted:
            return True, noise
        noise += b
    return False, noise


def first_packet(fname, size):
    payload = fname.encode() + b"\x00" + str(size).encode() + b" "
    payload = payload.ljust(PKT_128, b"\x00")[:PKT_128]
    crc = crc16_xmodem(payload)
    return bytes([SOH, 0x00, 0xFF]) + payload + bytes([(crc >> 8) & 0xFF, crc & 0xFF])


def data_packet(seq, data):
    crc = crc16_xmodem(data)
    return bytes([STX, seq & 0xFF, (~seq) & 0xFF]) + data + \
           bytes([(crc >> 8) & 0xFF, crc & 0xFF])


# ------------------------------------------------------------------- phases

def phase_1():
    """首包 ACK 后（擦除刚完成）断电。"""
    banner("Phase 1：首包 ACK 后断电")
    enter_iap_ram_flag()
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    wait_byte(ser, CRC_REQ, 8)
    img = open(APP, "rb").read()
    ser.write(first_packet(os.path.basename(APP), len(img)))
    ser.flush()
    ok, _ = wait_byte(ser, ACK, 15)
    print("  首包已 ACK（此刻 APP 扇区已擦除，状态=DOWNLOAD）:", ok)
    ser.close()
    txt, saw_banner = power_prompt("擦除完成、数据尚未写入时")
    jumped = "decision: JUMP" in txt
    in_iap = "waiting for YMODEM" in txt
    ok = saw_banner and in_iap and not jumped
    print()
    print("  判定: banner=%s 停在IAP=%s 误跳转=%s" % (saw_banner, in_iap, jumped))
    print("  ★ %s" % ("PASS（停在 IAP，可重刷，不变砖）" if ok else "FAIL"))


def phase_2():
    """数据传输中断电。"""
    banner("Phase 2：数据传输中断电")
    enter_iap_ram_flag()
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    wait_byte(ser, CRC_REQ, 8)
    img = open(APP, "rb").read()
    ser.write(first_packet(os.path.basename(APP), len(img)))
    ser.flush()
    wait_byte(ser, ACK, 15)
    wait_byte(ser, CRC_REQ, 15)
    # 发 1 个数据包
    ser.write(data_packet(1, img[:1024].ljust(1024, b"\x1a")))
    ser.flush()
    time.sleep(0.2)
    print("  已发 1 个数据包（写入进行中）")
    ser.close()
    txt, saw_banner = power_prompt("固件写到一半时")
    jumped = "decision: JUMP" in txt
    in_iap = "waiting for YMODEM" in txt
    ok = saw_banner and in_iap and not jumped
    print()
    print("  判定: banner=%s 停在IAP=%s 误跳转=%s" % (saw_banner, in_iap, jumped))
    print("  ★ %s" % ("PASS（停在 IAP，可重刷，不变砖）" if ok else "FAIL"))


def phase_3():
    """传输完成、commit 前后断电。"""
    banner("Phase 3：传输完成后断电")
    enter_iap_ram_flag()
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    wait_byte(ser, CRC_REQ, 8)
    sender = YmodemSender(ser, verbose=False)
    if not sender.send_file(APP):
        print("  ★ 升级失败")
        ser.close()
        return
    # 升级完成后 bootloader 会立即复位，这里抢在它复位前断电
    ser.close()
    txt, saw_banner = power_prompt("升级完成、bootloader 即将复位跳转时")
    ok = saw_banner
    print()
    print("  判定: banner=%s" % saw_banner)
    print("  ★ %s" % ("PASS（bootloader 存活，DOWNLOAD/TESTING 态不变砖）" if ok else "FAIL"))


def phase_4():
    """升级后首次启动（TESTING 态）断电。"""
    banner("Phase 4：升级后首次启动断电")
    enter_iap_ram_flag()
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    wait_byte(ser, CRC_REQ, 8)
    sender = YmodemSender(ser, verbose=False)
    sender.send_file(APP)
    time.sleep(2.0)  # 等它复位、首次启动、跳 APP
    ser.close()
    txt, saw_banner = power_prompt("APP 首次启动（TESTING 态）运行时")
    ok = saw_banner
    print()
    print("  判定: banner=%s" % saw_banner)
    print("  ★ %s" % ("PASS（bootloader 存活，clean boot 转 VALID，不变砖）" if ok else "FAIL"))


def phase_5():
    """APP 稳定运行中（VALID 态）断电。"""
    banner("Phase 5：APP 稳定运行中断电")
    enter_iap_ram_flag()
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    wait_byte(ser, CRC_REQ, 8)
    sender = YmodemSender(ser, verbose=False)
    sender.send_file(APP)
    time.sleep(10.0)  # 等它转 VALID、稳定运行
    ser.close()
    txt, saw_banner = power_prompt("APP 稳定运行（VALID 态）时")
    ok = saw_banner
    print()
    print("  判定: banner=%s" % saw_banner)
    print("  ★ %s" % ("PASS（bootloader 存活，直接跳回 APP，不变砖）" if ok else "FAIL"))


def main():
    global PORT, TARGET

    ap = argparse.ArgumentParser(description="断电暴力测试引导")
    ap.add_argument("--phase", type=int, required=True, choices=range(1, 6))
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--target", default=TARGET)
    args = ap.parse_args()

    PORT, TARGET = args.port, args.target

    phases = {1: phase_1, 2: phase_2, 3: phase_3, 4: phase_4, 5: phase_5}
    phases[args.phase]()
    return 0


if __name__ == "__main__":
    sys.exit(main())
