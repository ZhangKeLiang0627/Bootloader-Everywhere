#!/usr/bin/env python3
"""板端基础设施：烧写 / 探针 / 观察串口。

协议相关的东西在 proto.py（上位机）与 test_proto.py（回归测试）里。

用法：
    python board_test.py flash              # 烧 Bootloader 的 hex
    python board_test.py reset              # 复位一次
    python board_test.py observe 20         # 观察串口 20 秒
    python board_test.py info               # 读 APP 区向量表：上电会跳 APP 还是停在 IAP
"""

import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    serial = None

try:
    from pyocd.core.helpers import ConnectHelper
    from pyocd.flash.file_programmer import FileProgrammer
except ImportError:
    ConnectHelper = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

DEFAULT_HEX = os.path.join(ROOT, 'MDK-ARM', 'Bootloader-Everywhere',
                           'Bootloader-Everywhere.hex')
DEFAULT_BAUD = 115200
APP_BASE = 0x08004000


def open_probe(target, freq=1000000):
    return ConnectHelper.session_with_chosen_probe(
        target_override=target, frequency=freq, options={'resume_on_disconnect': True})


def flash_bootloader(target, hex_path):
    s = open_probe(target)
    try:
        s.open()
        print('烧写 %s ...' % os.path.basename(hex_path))
        FileProgrammer(s).program(hex_path)
        print('✔ 烧写完成')
    finally:
        s.close()


def probe_reset(target):
    """引脚复位。注意探针会连带置 SFTRSTF，Bootloader 会判为软件复位进唤回窗口。"""
    s = open_probe(target)
    try:
        s.open()
        s.target.reset_and_halt()
        s.target.resume()
    finally:
        s.close()


def cmd_flash(args):
    flash_bootloader(args.target, args.hex)
    return 0


def cmd_reset(args):
    probe_reset(args.target)
    print('✔ 已复位')
    return 0


def cmd_observe(args):
    ser = serial.Serial(args.port, DEFAULT_BAUD, timeout=0.05)
    buf = bytearray()
    t0 = time.time()
    try:
        while time.time() - t0 < args.seconds:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
            else:
                b = ser.read(1)
                if b:
                    buf += b
            time.sleep(0.01)
    finally:
        ser.close()
    for line in buf.decode('utf-8', 'replace').splitlines():
        s = line.strip()
        if s and s != 'C':
            print('  |', s)
    return 0


def cmd_info(args):
    """读 APP 区前两个字，判断上电会走哪条路。"""
    s = open_probe(args.target)
    try:
        s.open()
        t = s.target
        t.halt()
        sp = t.read32(APP_BASE)
        pc = t.read32(APP_BASE + 4)
        core = t.read_core_registers_raw([15])[0]
        t.resume()
    finally:
        s.close()

    sp_ok = 0x20000000 <= sp < 0x20100000 and (sp & 7) == 0
    pc_ok = (APP_BASE <= pc < 0x08100000) and (pc & 1) != 0
    print('APP 区向量表： SP=0x%08X  Reset=0x%08X' % (sp, pc))
    print('  上电行为：%s' % ('跳转 APP' if (sp_ok and pc_ok) else
                             '停在 IAP（向量表非法：空片 / 上次传输没提交）'))
    print('  当前 PC = 0x%08X → %s' % (core, 'APP 区' if core >= APP_BASE else 'Bootloader 区'))
    return 0


def main():
    ap = argparse.ArgumentParser(description='板端基础设施')
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--target', default='stm32f401retx')
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('flash')
    p.add_argument('--hex', default=DEFAULT_HEX)
    p.set_defaults(func=cmd_flash)

    sub.add_parser('reset').set_defaults(func=cmd_reset)

    p = sub.add_parser('observe')
    p.add_argument('seconds', type=float, nargs='?', default=10.0)
    p.set_defaults(func=cmd_observe)

    sub.add_parser('info').set_defaults(func=cmd_info)

    args = ap.parse_args()
    if ConnectHelper is None and args.func in (cmd_flash, cmd_reset, cmd_info):
        print('★ 需要 pyocd')
        return 1
    if serial is None and args.func is cmd_observe:
        print('★ 需要 pyserial')
        return 1
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
