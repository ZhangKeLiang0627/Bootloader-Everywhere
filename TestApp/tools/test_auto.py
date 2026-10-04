#!/usr/bin/env python3
"""
自动化暴力测试编排 —— LUMOS-bootloader

跑「不需要手动断电」的那批测试，逐项判定 PASS / FAIL 并输出汇总。
断电类测试（需 USB 继电器或手动拔插）不在本脚本范围内，见 TEST_PLAN.md。

用法：
    python test_auto.py            # 跑全部自动测试
    python test_auto.py --only T2  # 只跑某一项

测试项：
    T1  fail2(HardFault) 回滚     —— 已单独验证，这里作为回归
    T2  fail0(正常) 自确认转 VALID —— 正常固件不该被误回滚
    T3  fail1(不喂狗) 回滚        —— 另一种崩溃路径的回归
    T4  连续升级压力              —— 反复升级 fail0 若干次
"""

import os
import re
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from board_test import (DEFAULT_BAUD, DEFAULT_HEX, BACKDOOR_CHAR,
                        enter_iap, flash_bootloader, open_probe)  # noqa: E402
from ymodem_send import YmodemSender  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))          # .../LUMOS-bootloader
BUILD = os.path.join(ROOT, "build")
PORT = "COM3"
TARGET = "stm32f401retx"

APP_FAIL0 = os.path.join(BUILD, "app_test.bin")
APP_FAIL1 = os.path.join(BUILD, "app_fail1.bin")
APP_FAIL2 = os.path.join(BUILD, "app_fail2.bin")

RESULTS = []


def log(msg):
    print("  " + msg)


def record(tid, name, ok, detail=""):
    RESULTS.append((tid, name, ok, detail))
    mark = "PASS" if ok else "FAIL"
    print("\n[%s] %s  →  %s%s" % (tid, name, mark,
                                  ("  " + detail) if detail else ""))


def upgrade_then_observe(app_path, observe_s, mid_reset_at=None):
    """进 IAP → 升级 → 无缝观察。返回原始字节。

    mid_reset_at: 若给定，则在观察进行到该秒数时对目标做一次引脚复位
    （用于触发「用户按复位」→ clean boot 转 VALID 的路径）。
    """
    if not enter_iap(TARGET, PORT):
        return b""
    ser = serial.Serial(PORT, DEFAULT_BAUD, timeout=0.05)
    try:
        ser.reset_input_buffer()
        sender = YmodemSender(ser, verbose=False)
        if not sender.send_file(app_path):
            print("  ★ 升级失败")
            return b""
        buf = bytearray()
        t0 = time.time()
        reset_done = False
        while time.time() < t0 + observe_s:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
            else:
                b = ser.read(1)
                if b:
                    buf += b
            # 中途复位
            if mid_reset_at is not None and not reset_done \
               and (time.time() - t0) >= mid_reset_at:
                reset_done = True
                print("  [中途复位 @%.1fs]" % (time.time() - t0))
                try:
                    s = open_probe(TARGET)
                    s.open()
                    s.target.reset_and_halt()
                    s.target.resume()
                    s.close()
                except Exception as e:
                    print("  ★ 中途复位失败:", e)
            time.sleep(0.01)
        return bytes(buf)
    finally:
        ser.close()


def count(txt, *keys):
    return sum(txt.count(k) for k in keys)


# ------------------------------------------------------------------- 测试项

def T1_fail2_hardfault():
    """刷入上电即 HardFault 的固件，验证 3 次后自动回滚。"""
    raw = upgrade_then_observe(APP_FAIL2, 40)
    txt = raw.decode("utf-8", "replace")
    ok = ("rollback" in txt and "revoked" in txt
          and txt.count("watchdog reset") >= 3)
    record("T1", "fail2(HardFault) 回滚", ok,
           "watchdog=%d rollback=%d" % (txt.count("watchdog reset"),
                                        txt.count("rollback")))


def T2_fail0_self_confirm():
    """正常固件：升级后首次启动保持 TESTING，按复位后 clean boot 转 VALID。"""
    raw = upgrade_then_observe(APP_FAIL0, 30, mid_reset_at=8.0)
    txt = raw.decode("utf-8", "replace")
    first_run = "first run" in txt or "fresh upgrade" in txt
    self_confirmed = "self-confirmed" in txt or "self_confirmed" in txt
    alive = "alive" in txt
    ok = first_run and self_confirmed and alive and "rollback" not in txt
    record("T2", "fail0(正常) 自确认转 VALID", ok,
           "first_run=%s self_confirmed=%s alive=%s" % (first_run,
                                                        self_confirmed, alive))


def T3_fail1_no_watchdog():
    """不喂狗的固件：看门狗拉回 → 计数超限 → 回滚。"""
    raw = upgrade_then_observe(APP_FAIL1, 40)
    txt = raw.decode("utf-8", "replace")
    ok = "rollback" in txt and txt.count("watchdog reset") >= 3
    record("T3", "fail1(不喂狗) 回滚", ok,
           "watchdog=%d rollback=%d" % (txt.count("watchdog reset"),
                                        txt.count("rollback")))


def T4_stress_repeated_upgrade():
    """连续升级正常固件 N 次，每次都应成功且不误回滚。"""
    N = 5
    ok_all = True
    detail = []
    for i in range(N):
        raw = upgrade_then_observe(APP_FAIL0, 12, mid_reset_at=5.0)
        txt = raw.decode("utf-8", "replace")
        ok = "alive" in txt and "rollback" not in txt
        detail.append("%d:%s" % (i + 1, "✓" if ok else "✗"))
        ok_all = ok_all and ok
        if not ok:
            print("  ★ 第 %d 次异常" % (i + 1))
    record("T4", "连续升级压力 x%d" % N, ok_all, " ".join(detail))


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]

    print("=" * 64)
    print("LUMOS-bootloader 自动化暴力测试")
    print("=" * 64)
    print("固件路径: %s" % BUILD)
    print("串口: %s  目标: %s" % (PORT, TARGET))
    print()

    tests = [("T1", T1_fail2_hardfault),
             ("T2", T2_fail0_self_confirm),
             ("T3", T3_fail1_no_watchdog),
             ("T4", T4_stress_repeated_upgrade)]

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
        print("  [%s] %-32s %s" % (tid, name,
                                   "PASS" if ok else "FAIL"))
    print("  ---")
    print("  %d / %d 通过" % (npass, len(RESULTS)))
    return 0 if npass == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
