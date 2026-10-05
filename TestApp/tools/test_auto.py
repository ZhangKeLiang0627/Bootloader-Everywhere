#!/usr/bin/env python3
"""
自动化暴力测试编排 —— Bootloader-Everywhere

覆盖「不需要手动断电」的核心链路，逐项判定 PASS / FAIL 并输出汇总。
断电类测试（需 USB 继电器或手动拔插）不在本范围内。

用法：
    python test_auto.py             # 跑全部
    python test_auto.py --only T3   # 只跑某一项

测试项：
    T1  正常升级          —— 软件复位进窗口 → YMODEM 传输 → 回读校验 → 提交 → 跳新固件
    T2  连续升级压力 x5   —— 反复升级正常固件，验证反复擦写无累积错误
    T3  软件复位唤回      —— APP 运行态复位后进入限时窗口（IAP_TIMED），不升级
    T4  窗口超时跳回 APP  —— 15s 无上位机 → 自动跳回 APP
    T5  传输中断不变砖    —— 首包后断流 → SP/PC 仍是 0xFF → 停在 IAP 可重刷
"""

import os
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from board_test import (DEFAULT_BAUD, enter_iap, open_probe,
                        software_reset)                      # noqa: E402
from ymodem_send import YmodemSender, crc16_xmodem           # noqa: E402
from ymodem_send import SOH, STX                             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))          # .../Bootloader-Everywhere
BUILD = os.path.join(ROOT, "build")
PORT = "COM3"
TARGET = "stm32f401retx"

APP_TEST = os.path.join(BUILD, "app_test.bin")
APP_BASE = 0x08004000        # 与 bl_config.h 的 BL_APP_BASE 一致

RESULTS = []


def log(msg):
    print("  " + msg)


def record(tid, name, ok, detail=""):
    RESULTS.append((tid, name, ok, detail))
    mark = "PASS" if ok else "FAIL"
    print("\n[%s] %s  →  %s%s" % (tid, name, mark,
                                  ("  " + detail) if detail else ""))


# ------------------------------------------------------------------ 工具

def read_for(ser, seconds):
    """在已打开的串口上读 seconds 秒，返回原始字节。"""
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        else:
            b = ser.read(1)
            if b:
                buf += b
        time.sleep(0.01)
    return bytes(buf)


def wait_byte(ser, wanted, timeout=6.0):
    """等到某个字节，返回 (是否收到, 期间收到的其它字节)。"""
    deadline = time.time() + timeout
    noise = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == wanted:
            return True, bytes(noise)
        noise += b
    return False, bytes(noise)


def pin_reset():
    """引脚复位（模拟用户按复位键）。"""
    s = open_probe(TARGET)
    try:
        s.open()
        s.target.reset_and_halt()
        s.target.resume()
    finally:
        s.close()


def window_and_upgrade(app_path, observe_s=6.0):
    """软件复位进窗口 → YMODEM 传固件 → 无缝观察。返回原始字节。"""
    if not enter_iap(TARGET, PORT):
        return b""
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    try:
        ser.reset_input_buffer()
        if not YmodemSender(ser, verbose=False).send_file(app_path):
            print("  ★ 升级失败")
        return read_for(ser, observe_s)
    finally:
        ser.close()


def reset_and_capture(seconds=5.0):
    """软件复位并抓取启动日志。

    若规定时间内没抓到 bootloader banner（复位没生效，比如探针抖动），
    自动重试一次 —— 否则会把「上一轮残留的周期性输出」误当成启动日志。
    """
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    try:
        buf = bytearray()
        for _ in range(2):
            ser.reset_input_buffer()
            software_reset(TARGET)
            t0 = time.time()
            while time.time() - t0 < seconds:
                n = ser.in_waiting
                if n:
                    buf += ser.read(n)
                else:
                    b = ser.read(1)
                    if b:
                        buf += b
                time.sleep(0.01)
            if b"== Bootloader-Everywhere ==" in buf:
                break
        return bytes(buf)
    finally:
        ser.close()


def probe_head():
    """读 APP 区开头 64 字节，返回 (SP, PC, 第 8 字节起是否有已写入的数据)。

    这是验证「向量表最后写」机制的决定性证据：传输中断后，
    SP/PC 应该还是擦除态 0xFFFFFFFF，而其后紧跟的数据块已经写进去了。
    """
    s = open_probe(TARGET)
    try:
        s.open()
        t = s.target
        t.halt()
        raw = bytes(t.read_memory_block8(APP_BASE, 64))
        t.resume()
    finally:
        s.close()
    sp = int.from_bytes(raw[0:4], "little")
    pc = int.from_bytes(raw[4:8], "little")
    written = any(x != 0xFF for x in raw[8:64])
    return sp, pc, written


def ensure_app_running():
    """把设备带到一个「有效固件正在跑」的干净起点。"""
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    try:
        ser.reset_input_buffer()
        pin_reset()
        txt = read_for(ser, 2.5).decode("utf-8", "replace")
        if "alive" in txt or "Bootloader-Everywhere APP" in txt:
            return True
    finally:
        ser.close()

    print("  (无有效固件，先升级一次)")
    return b"alive" in window_and_upgrade(APP_TEST, 6.0)


# --------------------------------------------------------------- 测试项

def T1_normal_upgrade():
    """软件复位进窗口 → 升级正常固件 → 校验通过 → 直接跳新固件。"""
    raw = window_and_upgrade(APP_TEST, 6.0)
    txt = raw.decode("utf-8", "replace")
    done = "outcome=DONE" in txt
    jumped = "jumping to app" in txt
    alive = "alive" in txt
    ok = done and jumped and alive
    record("T1", "正常升级并直接跳转", ok,
           "DONE=%s jump=%s alive=%s" % (done, jumped, alive))


def T2_stress_repeated_upgrade():
    """连续升级正常固件 N 次，每次都应成功并跳转。"""
    N = 5
    ok_all = True
    detail = []
    for i in range(N):
        raw = window_and_upgrade(APP_TEST, 6.0)
        txt = raw.decode("utf-8", "replace")
        ok = ("outcome=DONE" in txt) and ("alive" in txt)
        detail.append("%d:%s" % (i + 1, "v" if ok else "x"))
        ok_all = ok_all and ok
        if not ok:
            print("  ★ 第 %d 次异常" % (i + 1))
    record("T2", "连续升级压力 x%d" % N, ok_all, " ".join(detail))


def T3_recall_window():
    """APP 运行态下软件复位 → 进限时窗口（IAP_TIMED），不升级。"""
    ensure_app_running()
    txt = reset_and_capture(4.0).decode("utf-8", "replace")
    sft = "reset cause = 3" in txt
    window = ("IAP_TIMED" in txt) or ("upgrade window" in txt)
    waiting = "waiting for YMODEM" in txt
    ok = sft and window and waiting
    record("T3", "软件复位唤回进窗口", ok,
           "sft=%s window=%s waiting=%s" % (sft, window, waiting))


def T4_window_timeout():
    """窗口内 15s 无上位机 → 超时跳回 APP。"""
    ensure_app_running()
    txt = reset_and_capture(19.0).decode("utf-8", "replace")
    timeout = "window timeout" in txt
    jumped = "jumping to app" in txt
    alive = "alive" in txt
    ok = timeout and jumped and alive
    record("T4", "窗口超时跳回 APP", ok,
           "timeout=%s jump=%s alive=%s" % (timeout, jumped, alive))


def T5_interrupted_transfer():
    """首包 + 1 个数据包后断流。

    期望：SP/PC 没被提交（仍是 0xFF），所以上电判「无可启动固件」留在 IAP；
    而其后的数据块已经写进 Flash —— 正好证明「提交 = 写这两个字」。
    """
    ensure_app_running()
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    try:
        ser.reset_input_buffer()
        software_reset(TARGET)
        # 等 bootloader 进入等待（会主动发 'C'）
        if b"C" not in read_for(ser, 2.0):
            record("T5", "传输中断不变砖", False, "未进入等待")
            return
        # 发首包：触发擦除。用日志判断是否被接受，
        # 不去盲等 ACK —— bootloader 会周期性补发 'C'，盲等容易读错字节。
        payload = (b"test_interrupt.bin\x00" + b"2000 ").ljust(128, b"\x00")[:128]
        crc = crc16_xmodem(payload)
        ser.write(bytes([SOH, 0x00, 0xFF]) + payload
                  + bytes([(crc >> 8) & 0xFF, crc & 0xFF]))
        ser.flush()
        accepted = b"[session] file=" in read_for(ser, 8.0)
        if not accepted:
            record("T5", "传输中断不变砖", False, "首包未被接受")
            return
        # 发 1 个数据包后立刻断流
        data = bytes(range(256)) * 4
        crc = crc16_xmodem(data)
        ser.write(bytes([STX, 0x01, 0xFE]) + data
                  + bytes([(crc >> 8) & 0xFF, crc & 0xFF]))
        ser.flush()
        time.sleep(0.2)
    finally:
        ser.close()

    # 断流后复位，检查 bootloader 是否正确停在 IAP
    txt = reset_and_capture(5.0).decode("utf-8", "replace")
    iap = "decision: IAP" in txt
    bricked = "decision: JUMP" in txt

    # 决定性证据：SP/PC 未提交，而其后的数据已写入
    sp, pc, data_written = probe_head()
    spc_erased = (sp == 0xFFFFFFFF) and (pc == 0xFFFFFFFF)

    ok = iap and (not bricked) and spc_erased and data_written
    record("T5", "传输中断不变砖（SP/PC 未提交）", ok,
           "IAP=%s 误跳转=%s sp=0x%08X pc=0x%08X 数据已写=%s"
           % (iap, bricked, sp, pc, data_written))


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]

    print("=" * 64)
    print("Bootloader-Everywhere 自动化暴力测试")
    print("=" * 64)
    print("固件路径: %s" % BUILD)
    print("串口: %s  目标: %s" % (PORT, TARGET))
    print()

    tests = [("T1", T1_normal_upgrade),
             ("T2", T2_stress_repeated_upgrade),
             ("T3", T3_recall_window),
             ("T4", T4_window_timeout),
             ("T5", T5_interrupted_transfer)]

    for tid, fn in tests:
        if only and tid != only:
            continue
        print("-" * 64)
        print("▶ %s" % tid)
        print("-" * 64)
        try:
            fn()
        except Exception as e:
            record(tid, fn.__doc__ or tid, False, "异常: %s" % e)

    print()
    print("=" * 64)
    print("汇总")
    print("=" * 64)
    npass = sum(1 for _, _, ok, _ in RESULTS if ok)
    for tid, name, ok, detail in RESULTS:
        print("  [%s] %-28s %s" % (tid, name, "PASS" if ok else "FAIL"))
    print("  ---")
    print("  %d / %d 通过" % (npass, len(RESULTS)))
    return 0 if npass == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
