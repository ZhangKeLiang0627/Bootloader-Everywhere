"""
LUMOS-bootloader 板端调试工具（DAPLink / CMSIS-DAP + pyocd + pyserial）

把「备份、擦除、烧写、读回、看串口」这一串常用动作收在一处，
方便按 docs/TEST_PLAN.md 反复跑（那个方案里有 18 次断电暴力测试，
手工点鼠标是跑不完的）。

用法示例：
    python tools/board.py info                       # 芯片信息 + 分区占用 + 配置区状态
    python tools/board.py backup                     # 整片备份到桌面 board_flash_backup/
    python tools/board.py restore <backup.bin>       # 还原
    python tools/board.py erase 0x08060000-0x08080000
    python tools/board.py flash build/bl_f401.bin
    python tools/board.py monitor                    # 读 8 秒串口
    python tools/board.py monitor --seconds 20 --port COM3

依赖：
    pyocd（含 CMSIS-DAP 支持）与 pyserial，装在 WorkBuddy 的隔离 Python 环境里。
"""
import argparse
import hashlib
import os
import sys
import time

DEFAULT_TARGET = 'stm32f401retx'

# F4 单 bank 的扇区布局：S0-S3=16KB、S4=64KB、S5+=128KB
SECTOR_LAYOUT = [(16, 4), (64, 1), (128, 16)]


# ---------------------------------------------------------------------------
# 连接
# ---------------------------------------------------------------------------
def connect(target, freq=500000, under_reset=True, quiet=False):
    """连接目标；先试 under-reset（目标固件不跑，最稳），再退到普通模式。"""
    from pyocd.core.helpers import ConnectHelper

    attempts = []
    if under_reset:
        attempts.append(({'connect_mode': 'under-reset'}, 100000))
        attempts.append(({'connect_mode': 'under-reset'}, 500000))
    attempts.append(({}, 500000))
    attempts.append(({}, 1000000))

    last = None
    for opts, f in attempts:
        try:
            s = ConnectHelper.session_with_chosen_probe(
                target_override=target, frequency=f,
                options=dict(opts, resume_on_disconnect=False))
            s.open()
            if not quiet:
                mode = opts.get('connect_mode', 'normal')
                print('已连接 %s @ %d Hz (%s)' % (target, f, mode))
            return s
        except Exception as e:                                  # noqa: BLE001
            last = e
            try:
                s.close()
            except Exception:                                   # noqa: BLE001
                pass
    print('★ 连接失败：%s' % str(last)[:200])
    print('  检查：DAPLink 的 SWD 线（SWCLK/SWDIO/GND）是否接好、目标是否供电。')
    return None



def region_start(r):
    """pyocd 各版本对 MemoryRegion 起始地址的属性名不一致，兼容一下。"""
    for attr in ('start', 'start_address'):
        v = getattr(r, attr, None)
        if v is not None:
            return v
    raise AttributeError('无法取得区域起始地址：%r' % (r,))


def chip_sectors(flash_kb):
    """按容量算出扇区表 [(base_offset, size)]，只覆盖到容量为止。"""
    out, off = [], 0
    for size_kb, count in SECTOR_LAYOUT:
        for _ in range(count):
            if off >= flash_kb * 1024:
                return out
            out.append((off, size_kb * 1024))
            off += size_kb * 1024
    return out


def read_flash(t, base, size, chunk=4096, progress=True):
    data = bytearray()
    for addr in range(base, base + size, chunk):
        n = min(chunk, base + size - addr)
        data.extend(bytearray(t.read_memory_block8(addr, n)))
        if progress and (addr - base) % (64 * 1024) == 0:
            sys.stdout.write('.')
            sys.stdout.flush()
    if progress:
        print()
    return data


# ---------------------------------------------------------------------------
# 命令
# ---------------------------------------------------------------------------
def cmd_info(args):
    s = connect(args.target)
    if s is None:
        return 1
    t = s.target
    try:
        t.halt()
        idcode = t.read32(0xE0042000)
        dev = idcode & 0xFFF
        print('DBGMCU_IDCODE = 0x%08X   DEV_ID = 0x%03X' % (idcode, dev))

        for r in t.memory_map:
            print('  %-6s 0x%08X + %d KB' % (r.name, region_start(r), r.length // 1024))
            if r.name == 'Flash':
                flash_base, flash_size = region_start(r), r.length

        sp = t.read32(flash_base)
        pc = t.read32(flash_base + 4)
        print()
        print('Flash 起始向量： SP=0x%08X  Reset=0x%08X  %s'
              % (sp, pc,
                 '空白' if (sp == 0xFFFFFFFF and pc == 0xFFFFFFFF)
                 else ('看起来有固件' if 0x20000000 <= sp < 0x20030000 else '向量可疑')))

        print()
        print('扇区占用：')
        for i, (off, sz) in enumerate(chip_sectors(flash_size // 1024)):
            probe = min(sz, 16 * 1024)
            seg = bytearray(t.read_memory_block8(flash_base + off, probe))
            nz = sum(1 for b in seg if b != 0xFF)
            note = '' if probe == sz else ' (仅前 16KB)'
            print('  S%-2d 0x%08X %4dKB : %s%s'
                  % (i, flash_base + off, sz // 1024,
                     '空' if nz == 0 else '%d 字节' % nz, note))

        # 配置区（最后一个扇区）的槽位扫描：本库的槽魔数是 "LUMS"
        meta_base = flash_base + flash_size - 128 * 1024
        print()
        print('配置区扫描 0x%08X（槽魔数 "LUMS" = 0x4C554D53）:' % meta_base)
        found = 0
        for i in range(0, 128 * 1024, 64):
            head = bytearray(t.read_memory_block8(meta_base + i, 8))
            if head[:4] == b'\xff\xff\xff\xff':
                break
            magic = int.from_bytes(head[4:8], 'little')
            if magic == 0x4C554D53:
                found += 1
        print('  有效槽位数：%d %s' % (found, '(空配置区)' if found == 0 else ''))
    finally:
        try:
            t.resume()
        except Exception:                                       # noqa: BLE001
            pass
        s.close()
    return 0


def cmd_backup(args):
    s = connect(args.target)
    if s is None:
        return 1
    t = s.target
    try:
        t.halt()
        flash = next(r for r in t.memory_map if r.name == 'Flash')
        print('读取 %d KB ...' % (flash.length // 1024))
        data = read_flash(t, region_start(flash), flash.length)
    finally:
        try:
            t.resume()
        except Exception:                                       # noqa: BLE001
            pass
        s.close()

    out_dir = args.out or os.path.join(os.path.expanduser('~'), 'Desktop',
                                       'board_flash_backup')
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, '%s_%s.bin'
                        % (args.target, time.strftime('%Y%m%d_%H%M%S')))
    with open(path, 'wb') as fp:
        fp.write(data)
    used = sum(1 for b in data if b != 0xFF)
    print('已备份 %d 字节，非 0xFF %d 字节 (%.1f%%)' % (len(data), used, used * 100.0 / len(data)))
    print('MD5   :', hashlib.md5(bytes(data)).hexdigest())
    print('文件  :', path)
    return 0


def cmd_restore(args):
    data = open(args.file, 'rb').read()
    print('待写入 %d 字节，MD5 %s' % (len(data), hashlib.md5(data).hexdigest()))
    s = connect(args.target)
    if s is None:
        return 1
    try:
        from pyocd.flash.file_programmer import FileProgrammer
        FileProgrammer(s).program(args.file, base_address=args.addr)
    finally:
        try:
            s.target.resume()
        except Exception:                                       # noqa: BLE001
            pass
        s.close()
    print('★ 还原完成')
    return 0


def cmd_flash(args):
    if not os.path.exists(args.file):
        print('文件不存在：%s' % args.file)
        return 1
    size = os.path.getsize(args.file)
    print('待烧写 %s (%d 字节, %d KB)' % (args.file, size, (size + 1023) // 1024))

    s = connect(args.target)
    if s is None:
        return 1
    t = s.target
    try:
        t.halt()
        if args.erase_first:
            from pyocd.flash.eraser import FlashEraser
            print('先擦除 %s ...' % args.erase_first)
            FlashEraser(s, FlashEraser.Mode.SECTOR).erase([args.erase_first])

        from pyocd.flash.file_programmer import FileProgrammer
        FileProgrammer(s).program(args.file, base_address=args.addr)
        print('写入完成，回读校验 ...')

        # 回读比对：这是「烧写是否真的成功」唯一可靠的判据
        img = open(args.file, 'rb').read()
        back = bytearray(t.read_memory_block8(args.addr, len(img)))
        same = (bytes(back) == img)
        print('回读比对：%s' % ('一致 ✔' if same else '不一致 ✘'))
        if not same:
            for i, (a, b) in enumerate(zip(img, back)):
                if a != b:
                    print('  首个差异在偏移 %d (0x%08X): 期望 0x%02X 实际 0x%02X'
                          % (i, args.addr + i, a, b))
                    break
    finally:
        try:
            t.resume()
        except Exception:                                       # noqa: BLE001
            pass
        s.close()
    print('★ 烧写结束（目标已放开运行）')
    return 0


def cmd_erase(args):
    s = connect(args.target)
    if s is None:
        return 1
    try:
        from pyocd.flash.eraser import FlashEraser
        print('擦除 %s ...' % args.range)
        FlashEraser(s, FlashEraser.Mode.SECTOR).erase([args.range])
        print('★ 擦除完成')
    finally:
        try:
            s.target.resume()
        except Exception:                                       # noqa: BLE001
            pass
        s.close()
    return 0


def cmd_read(args):
    s = connect(args.target)
    if s is None:
        return 1
    t = s.target
    try:
        t.halt()
        data = bytearray(t.read_memory_block8(args.addr, args.len))
    finally:
        try:
            t.resume()
        except Exception:                                       # noqa: BLE001
            pass
        s.close()
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        hexs = ' '.join('%02X' % b for b in row)
        txt = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print('%08X  %-47s  %s' % (args.addr + i, hexs, txt))
    return 0


def cmd_monitor(args):
    import serial                                                # 延迟导入
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as e:                                       # noqa: BLE001
        print('打开 %s 失败：%s' % (args.port, e))
        return 1

    print('监听 %s @ %d 8N1，%d 秒（Ctrl+C 提前结束）'
          % (args.port, args.baud, args.seconds))
    print('-' * 68)
    buf = b''
    end = time.time() + args.seconds
    try:
        while time.time() < end:
            chunk = ser.read(ser.in_waiting or 1)
            if chunk:
                buf += chunk
                sys.stdout.write(chunk.decode('utf-8', 'replace'))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    print()
    print('-' * 68)
    print('共收到 %d 字节' % len(buf))
    return 0


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='LUMOS-bootloader 板端工具')
    ap.add_argument('--target', default=DEFAULT_TARGET,
                    help='pyocd 目标名（默认 %s）' % DEFAULT_TARGET)
    sub = ap.add_subparsers(dest='cmd', required=True)

    sub.add_parser('info', help='芯片信息 / 扇区占用 / 配置区状态').set_defaults(func=cmd_info)

    p = sub.add_parser('backup', help='整片备份')
    p.add_argument('--out', help='输出目录（默认桌面 board_flash_backup）')
    p.set_defaults(func=cmd_backup)

    p = sub.add_parser('restore', help='把备份写回 Flash')
    p.add_argument('file')
    p.add_argument('--addr', type=lambda x: int(x, 0), default=0x08000000)
    p.set_defaults(func=cmd_restore)

    p = sub.add_parser('flash', help='烧写 bin/hex')
    p.add_argument('file')
    p.add_argument('--addr', type=lambda x: int(x, 0), default=0x08000000)
    p.add_argument('--erase-first', help='先擦除该地址范围，例如 0x08060000-0x08080000')
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser('erase', help='按扇区擦除，格式 起-止 或 起+长度')
    p.add_argument('range')
    p.set_defaults(func=cmd_erase)

    p = sub.add_parser('read', help='读一段内存并打印十六进制')
    p.add_argument('addr', type=lambda x: int(x, 0))
    p.add_argument('len', type=lambda x: int(x, 0))
    p.set_defaults(func=cmd_read)

    p = sub.add_parser('monitor', help='读串口')
    p.add_argument('--port', default='COM3')
    p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--seconds', type=int, default=8)
    p.set_defaults(func=cmd_monitor)

    args = ap.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
