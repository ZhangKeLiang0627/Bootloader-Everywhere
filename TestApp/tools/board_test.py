#!/usr/bin/env python3
"""
板端测试驱动 —— LUMOS-bootloader 的自动化测试基础设施

把「烧写 → 进 IAP → 升级 → 观察」这一串动作封成命令，避免每次测试都
临时拼脚本。

用法：
    python board_test.py flash                       # 烧 Bootloader 的 hex
    python board_test.py iap                         # 软件复位进入限时升级窗口
    python board_test.py upgrade build/app_test.bin  # 传固件（自动等 'C'）
    python board_test.py observe 20                  # 单纯观察串口 20 秒
    python board_test.py run --app build/app_test.bin
                                                     # 烧 Bootloader + 进 IAP + 升级 + 观察

串口与探针参数可用 --port / --target 覆盖。
"""

import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    print("★ 需要 pyserial：pip install pyserial")
    sys.exit(1)

try:
    from pyocd.core.helpers import ConnectHelper
    from pyocd.flash.file_programmer import FileProgrammer
except ImportError:
    print("★ 需要 pyocd：python -m pip install pyocd")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ymodem_send import YmodemSender        # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))          # .../LUMOS-bootloader
DEFAULT_HEX = os.path.join(ROOT, "MDK-ARM", "LUMOS-bootloader",
                           "LUMOS-bootloader.hex")
DEFAULT_BAUD = 115200

# SCB->AIRCR：VECTKEY | SYSRESETREQ
AIRCR_SYSRESETREQ = 0x05FA0004


# ---------------------------------------------------------------- 基础设施

def open_probe(target, freq=1000000):
    """连接探针。resume_on_disconnect=True 保证断开后目标继续跑。"""
    return ConnectHelper.session_with_chosen_probe(
        target_override=target, frequency=freq,
        options={'resume_on_disconnect': True})


def flash_bootloader(target, hex_path):
    s = open_probe(target)
    try:
        s.open()
        print("烧写 %s ..." % os.path.basename(hex_path))
        FileProgrammer(s).program(hex_path)
        print("✔ 烧写完成")
    finally:
        s.close()


def software_reset(target, retries=3):
    """用探针触发一次软件复位（SYSRESETREQ）。

    Bootloader 靠复位原因识别唤回：软件复位 + 固件 Valid 态 → 进入限时
    升级窗口。这是网页端「点开始升级自动唤回」在测试侧的等价手段。

    pyocd 连接偶发失败（线缆抖动），重试几次以免测试被硬件噪声打断。
    """
    last = None
    for _ in range(retries):
        s = open_probe(target)
        try:
            s.open()
            t = s.target
            t.halt()
            t.write32(0xE000ED0C, AIRCR_SYSRESETREQ)
            return True
        except Exception as e:            # noqa: BLE001
            last = e
        finally:
            try:
                s.close()
            except Exception:             # noqa: BLE001
                pass
        time.sleep(0.4)
    print("  ★ software_reset 失败:", last)
    return False


def enter_iap(target, port, wait=8.0):
    """软件复位进入限时升级窗口，并确认已进 IAP。"""
    ser = serial.Serial(port, DEFAULT_BAUD, timeout=0.05)
    try:
        ser.reset_input_buffer()
        software_reset(target)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < wait:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
            else:
                b = ser.read(1)
                if b:
                    buf += b
            if b'waiting for YMODEM' in buf:
                break
            time.sleep(0.01)
        txt = buf.decode('utf-8', 'replace')
        ok = ('waiting for YMODEM' in txt) or ('IAP' in txt)
        print("  %s" % ("✔ 已进入 IAP" if ok else "★ 未确认进入 IAP"))
        for line in txt.splitlines():
            if line.strip() and not line.strip().startswith('C'):
                print("    |", line.replace('\r', ''))
        return ok
    finally:
        ser.close()


def upgrade(port, app_path, timeout=120.0):
    """把固件通过 YMODEM 发过去，并一直读到出现结果日志为止。"""
    if not os.path.exists(app_path):
        print("★ 文件不存在:", app_path)
        return False

    print("升级 %s ..." % os.path.basename(app_path))
    ser = serial.Serial(port, DEFAULT_BAUD, timeout=0.05)
    ok = False
    try:
        ser.reset_input_buffer()
        sender = YmodemSender(ser, verbose=False)
        ok = sender.send_file(app_path)
    finally:
        ser.close()
    return ok


def observe(port, seconds, quiet=False):
    """观察串口，返回 (原始字节, 逐行列表)。"""
    ser = serial.Serial(port, DEFAULT_BAUD, timeout=0.05)
    buf = bytearray()
    t0 = time.time()
    deadline = t0 + seconds
    try:
        while time.time() < deadline:
            n = ser.in_waiting
            if n:
                chunk = ser.read(n)
                buf += chunk
                if not quiet:
                    for line in chunk.decode('utf-8', 'replace').replace('\r', '').split('\n'):
                        s = line.strip()
                        if s:
                            print("  [%5.1fs] %s" % (time.time() - t0, s[:110]))
            else:
                b = ser.read(1)
                if b:
                    buf += b
            time.sleep(0.01)
    finally:
        ser.close()
    return bytes(buf)


# ------------------------------------------------------------------- 命令

def cmd_flash(args):
    flash_bootloader(args.target, args.hex)
    return 0


def cmd_iap(args):
    return 0 if enter_iap(args.target, args.port) else 1


def cmd_upgrade(args):
    return 0 if upgrade(args.port, args.app) else 1


def cmd_observe(args):
    observe(args.port, args.seconds)
    return 0


def cmd_run(args):
    """一体化：烧 Bootloader（可选）→ 进 IAP → 升级 → **无缝观察**。

    ⚠️ 升级与观察必须复用同一个串口连接。中间一旦断开再重开，
    就会错过「升级完成后复位 → Bootloader 重新决策」那几百毫秒的关键日志
    —— 而复位原因正是在那里打印的，是最重要的证据。
    """
    if args.bootloader:
        flash_bootloader(args.target, args.bootloader)
        time.sleep(0.3)
    if not enter_iap(args.target, args.port):
        print("★ 未能进入 IAP，终止")
        return 1

    ser = serial.Serial(args.port, DEFAULT_BAUD, timeout=0.05)
    try:
        print("升级 %s ..." % os.path.basename(args.app))
        ser.reset_input_buffer()
        sender = YmodemSender(ser, verbose=False)
        if not sender.send_file(args.app):
            print("★ 升级失败")
            return 1

        print("\n--- 无缝观察 %d 秒 ---" % args.observe_s)
        raw = observe_on(ser, args.observe_s)
    finally:
        ser.close()

    txt = raw.decode('utf-8', 'replace')
    print("\n--- 摘要 ---")
    print("  bootloader 启动     : %d 次" % txt.count('LUMOS-bootloader'))
    print("  APP 启动            : %d 次" % txt.count('LUMOS APP'))
    print("  进入窗口 (IAP_TIMED): %d 次" % txt.count('IAP_TIMED'))
    print("  窗口超时跳回 APP    : %d 次" % txt.count('upgrade window timeout'))
    causes = re.findall(r'reset cause = (\d)', txt)
    if causes:
        names = {'0': 'unk', '1': 'por', '2': 'pin', '3': 'sft', '4': 'wdg',
                 '5': 'bor', '6': 'lp'}
        print("  复位原因序列        : %s"
              % ', '.join('%s(%s)' % (c, names.get(c, '?')) for c in causes))
    return 0


def observe_on(ser, seconds):
    """在已打开的串口上观察，返回原始字节。"""
    buf = bytearray()
    t0 = time.time()
    while time.time() < t0 + seconds:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n)
            buf += chunk
            for line in chunk.decode('utf-8', 'replace').replace('\r', '').split('\n'):
                s = line.strip()
                if s and not s.startswith('C'):
                    print("  [%5.1fs] %s" % (time.time() - t0, s[:110]))
        else:
            b = ser.read(1)
            if b:
                buf += b
        time.sleep(0.01)
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser(description="LUMOS-bootloader 板端测试驱动")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--target", default="stm32f401retx")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("flash", help="烧 Bootloader")
    p.add_argument("--hex", default=DEFAULT_HEX)
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser("iap", help="软件复位进限时升级窗口")
    p.set_defaults(func=cmd_iap)

    p = sub.add_parser("upgrade", help="传固件")
    p.add_argument("app")
    p.set_defaults(func=cmd_upgrade)

    p = sub.add_parser("observe", help="观察串口")
    p.add_argument("seconds", type=float)
    p.set_defaults(func=cmd_observe)

    p = sub.add_parser("run", help="烧写+进IAP+升级+观察")
    p.add_argument("--app", required=True)
    p.add_argument("--bootloader", default=None, help="给了就顺便烧 Bootloader")
    p.add_argument("--observe-s", type=float, default=40.0)
    p.set_defaults(func=cmd_run)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
