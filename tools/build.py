"""
LUMOS-bootloader 命令行构建 / ROM 测量 / 固件导出

不打开 uVision 就能：
  - 编译 + 链接整份 Bootloader，量出真实 ROM 占用（是否装得进 16KB）
  - 导出可烧写的 .bin / .hex

依赖：Keil MDK 的 AC6 工具链（armclang / armasm / armlink / fromelf）。
工具链路径可用环境变量 KEIL_ARMCLANG_BIN 覆盖。

用法：
    python tools/build.py                    # 默认芯片，-Oz + 调试日志
    python tools/build.py -c stm32f405xx     # 换芯片
    python tools/build.py -c stm32f401xe --bin build/bl.bin
    python tools/build.py --no-log --opt -Oz # 发布版（关日志、压体积）
    python tools/build.py --all              # 逐个芯片跑一遍，看 ROM 占用对比
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# 路径推导：脚本位于 <repo>/Bootloader/tools/ 下
# ---------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # .../LUMOS-bootloader —— 宿主工程根
PROJ = os.path.join(ROOT, 'Bootloader')            # .../Bootloader —— 库根目录

KEIL = os.environ.get(
    'KEIL_ARMCLANG_BIN',
    r'C:\Users\11846\AppData\Local\Keil_v5\ARM\ARMCLANG\bin')
CC = os.path.join(KEIL, 'armclang.exe')
AS = os.path.join(KEIL, 'armasm.exe')
LD = os.path.join(KEIL, 'armlink.exe')
FE = os.path.join(KEIL, 'fromelf.exe')

# ---------------------------------------------------------------------------
# 芯片档案：只描述「这颗芯片的 Flash / SRAM 怎么切」
#
#   device  —— 传给编译器的 CMSIS 器件宏
#   startup —— 启动文件
#   flash   —— Flash 容量，用来算链接区域与分区
#   ram     —— SRAM 容量
#   hse     —— 只用于 -DHSE_VALUE（宿主 system_stm32f4xx.c 需要）。
#              库自己不配时钟，量体积时用空实现代替。
# ---------------------------------------------------------------------------
CHIPS = {
    'stm32f401xe': dict(device='STM32F401xE', startup='startup_stm32f401xe.s',
                        hse=25000000, flash=512 * 1024, ram=96 * 1024),
    'stm32f405xx': dict(device='STM32F405xx', startup='startup_stm32f405xx.s',
                        hse=8000000, flash=1024 * 1024, ram=128 * 1024),
    'stm32f407xx': dict(device='STM32F407xx', startup='startup_stm32f407xx.s',
                        hse=8000000, flash=1024 * 1024, ram=128 * 1024),
}

# ---------------------------------------------------------------------------
# 源码清单
# ---------------------------------------------------------------------------
LIB_SRCS = ['bl.cpp', 'bl_port_stm32f4.cpp']   # 库本体 + STM32F4 移植实现

# 只编译真正用到的 HAL 模块。全量编译会把没用到的模块也拖进来占 ROM。
HAL_NEED = ['stm32f4xx_hal.c', 'stm32f4xx_hal_cortex.c', 'stm32f4xx_hal_rcc.c',
            'stm32f4xx_hal_rcc_ex.c', 'stm32f4xx_hal_gpio.c', 'stm32f4xx_hal_flash.c',
            'stm32f4xx_hal_flash_ex.c', 'stm32f4xx_hal_flash_ramfunc.c',
            'stm32f4xx_hal_pwr.c', 'stm32f4xx_hal_pwr_ex.c']

# 宿主工程里必须保留的 CMSIS 文件（提供 SystemInit / SystemCoreClockUpdate）
HOST_CMSIS = ['Core/Src/system_stm32f4xx.c']

BOOT_REGION = 16 * 1024        # Bootloader 区 16KB（与 bl_config.h 一致）


def includes() -> list:
    """include 路径。库自身只需要 Bootloader/ 一个根。"""
    return [
        '-I', PROJ,
        '-I', os.path.join(ROOT, 'Core', 'Inc'),
        '-I', os.path.join(ROOT, 'Drivers', 'STM32F4xx_HAL_Driver', 'Inc'),
        '-I', os.path.join(ROOT, 'Drivers', 'STM32F4xx_HAL_Driver', 'Inc', 'Legacy'),
        '-I', os.path.join(ROOT, 'Drivers', 'CMSIS', 'Device', 'ST', 'STM32F4xx', 'Include'),
        '-I', os.path.join(ROOT, 'Drivers', 'CMSIS', 'Include'),
    ]


def compile_all(chip: str, opt: str, defines: list, out: str, verbose=False):
    """编译全部源文件，返回 (对象文件列表, 错误列表)"""
    prof = CHIPS[chip]
    defs = ['-DUSE_HAL_DRIVER', '-D' + prof['device'],
            '-DHSE_VALUE=%d' % prof['hse']] + defines
    common = ['--target=arm-arm-none-eabi', '-mcpu=cortex-m4',
              '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard',
              '-c', opt, '-Wall', '-Wextra'] + defs + includes()

    objs, errs = [], []

    def build(src, kind, tag):
        o = os.path.join(out, tag + '.o')
        if kind == 'cpp':
            args = [CC] + common + ['-std=c++14', '-fno-exceptions', '-fno-rtti']
        elif kind == 'c':
            args = [CC] + common + ['-std=c99']
        else:  # asm (armasm 传统语法)
            r = subprocess.run([AS, '--cpu=Cortex-M4', '--apcs=interwork', '-g',
                                src, '-o', o], capture_output=True, text=True)
            (objs if r.returncode == 0 else errs).append(
                o if r.returncode == 0 else (tag, (r.stderr or r.stdout)[:400]))
            return
        r = subprocess.run(args + [src, '-o', o], capture_output=True, text=True)
        if r.returncode == 0:
            objs.append(o)
        else:
            errs.append((tag, (r.stderr or r.stdout)[:600]))
            if verbose:
                print(r.stderr or r.stdout)

    for rel in LIB_SRCS:
        build(os.path.join(PROJ, rel.replace('/', os.sep)), 'cpp',
              os.path.splitext(os.path.basename(rel))[0])

    for h in HAL_NEED:
        build(os.path.join(ROOT, 'Drivers', 'STM32F4xx_HAL_Driver', 'Src', h),
              'c', os.path.splitext(h)[0])

    for rel in HOST_CMSIS:
        build(os.path.join(ROOT, rel.replace('/', os.sep)), 'c',
              os.path.splitext(os.path.basename(rel))[0])

    build(os.path.join(ROOT, 'MDK-ARM', prof['startup']), 'asm', 'startup')

    # 库不初始化芯片，也不带 main()。量「库本体」体积时补一个最小宿主：
    # 这三样在真实工程里都由 CubeMX 生成（见 Core/Src）。
    os.makedirs(out, exist_ok=True)
    stub = os.path.join(out, 'host_stub.c')
    with open(stub, 'w', encoding='utf-8', newline='\n') as f:
        f.write('/* 量库体积用的最小宿主，见 build.py 注释 */\n'
                '#include "bl.h"\n'
                'void SystemClock_Config(void) { }\n'
                'void HAL_MspInit(void) { }\n'
                'int main(void) { blRun(); return 0; }\n')
    build(stub, 'c', 'host_stub')

    return objs, errs


def link(chip: str, objs: list, out: str, region: int = BOOT_REGION,
         want_bin=None, want_hex=None):
    prof = CHIPS[chip]

    sct = os.path.join(out, 'scatter.sct')
    with open(sct, 'w') as fp:
        fp.write('LR_IROM1 0x08000000 0x%08X {\n'
                 '  ER_IROM1 0x08000000 0x%08X {\n'
                 '   startup.o (RESET, +First)\n'
                 '   *(InRoot$$Sections)\n'
                 '   .ANY (+RO)\n'
                 '  }\n'
                 '  RW_IRAM1 0x20000000 0x%08X {\n'
                 '   .ANY (+RW +ZI)\n'
                 '  }\n'
                 '}\n' % (region, region, prof['ram']))

    elf = os.path.join(out, 'bl.elf')
    r = subprocess.run([LD, '--scatter', sct, '--entry', 'Reset_Handler',
                        '--info', 'sizes', '-o', elf] + objs,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr or r.stdout)

    # 导出发烧文件
    if want_bin:
        os.makedirs(os.path.dirname(os.path.abspath(want_bin)), exist_ok=True)
        subprocess.run([FE, '--bin', '--output', want_bin, elf],
                       capture_output=True)
    if want_hex:
        os.makedirs(os.path.dirname(os.path.abspath(want_hex)), exist_ok=True)
        subprocess.run([FE, '--i32', '--output', want_hex, elf],
                       capture_output=True)

    m = re.search(r'Total RO\s+Size \(Code \+ RO Data\)\s+(\d+)', r.stdout)
    rw = re.search(r'Total RW\s+Size \(RW Data \+ ZI Data\)\s+(\d+)', r.stdout)
    zi = re.search(r'Total ZI\s+Size \(ZI Data\)\s+(\d+)', r.stdout)
    return (int(m.group(1)) if m else None,
            None, elf,
            int(rw.group(1)) if rw else None,
            int(zi.group(1)) if zi else None)


def build_one(chip: str, opt: str, defines: list, region=BOOT_REGION,
              want_bin=None, want_hex=None, keep=False, label=''):
    out = tempfile.mkdtemp(prefix='bl_%s_' % chip)
    objs, errs = compile_all(chip, opt, defines, out)
    if errs:
        print('  ✘ 编译失败 %d 个文件：' % len(errs))
        for name, e in errs:
            # 报错信息里第一条 error: 行才是真正有用的，前面往往是一串
            # "In file included from ..." 噪音
            msg = next((l.strip() for l in e.splitlines() if 'error:' in l), None)
            if msg is None:
                msg = e.strip().splitlines()[0].strip() if e.strip() else '(无输出)'
            print('    %-30s %s' % (name, msg[:170]))
        return None

    res = link(chip, objs, out, region, want_bin, want_hex)
    if len(res) == 2:
        ro, err = res
        print('  ✘ 链接失败（超出 %d KB 区域）：' % (region // 1024))
        for line in (err or '').splitlines()[:4]:
            print('    ', line.strip()[:150])
        return None

    ro, _, elf, rw, zi = res
    free = region - ro
    print('  %-28s RO=%6d B (%5.2f KB)  余量=%5d B  RW+ZI=%s B  %s'
          % (label or ('%s %s' % (chip, opt)), ro, ro / 1024, free,
             rw if rw is not None else '?',
             '✔ 装得下' if free > 0 else '✘ 超出'))
    if want_bin:
        print('      bin → %s' % want_bin)
    if want_hex:
        print('      hex → %s' % want_hex)
    if keep:
        print('      中间产物 → %s' % out)
    return ro


def main():
    ap = argparse.ArgumentParser(description='LUMOS-bootloader 构建 / ROM 测量')
    ap.add_argument('-c', '--chip', default='stm32f401xe', choices=sorted(CHIPS),
                    help='目标芯片档案（默认 stm32f401xe）')
    ap.add_argument('--opt', default='-Oz',
                    help='优化等级。默认 -Oz（实测 -Os 开日志时只剩 268B 余量，太紧）')
    ap.add_argument('--no-log', action='store_true', help='关闭调试日志（发布版）')
    ap.add_argument('--bin', help='导出 .bin 到该路径')
    ap.add_argument('--hex', help='导出 .hex(Intel) 到该路径')
    ap.add_argument('--keep', action='store_true', help='保留中间产物目录')
    ap.add_argument('--all', action='store_true', help='所有芯片 × 常用优化等级全跑一遍')
    args = ap.parse_args()

    if not os.path.exists(CC):
        print('找不到 armclang：%s' % CC)
        print('请设置环境变量 KEIL_ARMCLANG_BIN 指向 Keil 的 ARMCLANG\\bin')
        return 1

    defines = ['-DBL_DEBUG_LOG=0'] if args.no_log else []

    if args.all:
        print('=' * 74)
        print('ROM 占用对比（链接区域 = Bootloader 区 %d KB）' % (BOOT_REGION // 1024))
        print('提示：-O2 预期会超出 16KB —— 特意列出来说明优化等级的影响。')
        print('=' * 74)
        for chip in sorted(CHIPS):
            print()
            print('--- %s ---' % chip)
            for opt in ['-O2', '-Os', '-Oz']:
                build_one(chip, opt, [], label='%s 日志开' % opt)
            build_one(chip, '-Os', ['-DBL_DEBUG_LOG=0'], label='-Os 日志关')
        return 0

    print('=' * 74)
    print('LUMOS-bootloader 构建  chip=%s  opt=%s  log=%s'
          % (args.chip, args.opt, 'off' if args.no_log else 'on'))
    print('=' * 74)
    ro = build_one(args.chip, args.opt, defines,
                   want_bin=args.bin, want_hex=args.hex, keep=args.keep)
    return 0 if ro is not None else 2


if __name__ == '__main__':
    sys.exit(main())
