"""
Bootloader ROM footprint measurement for LUMOS-bootloader.

直接编译 + 链接整个固件（Bootloader 源码 + HAL + CubeMX Core + startup），
按给定的区域大小做链接，从而在不打开 uVision 的情况下量出真实 ROM 占用。
改完代码后跑一次，就能立刻知道还装不装得下 16KB。

依赖：Keil MDK 的 AC6 工具链（armclang / armasm / armlink）。

Compiles the whole firmware (Bootloader sources + HAL + CubeMX Core + startup)
with the Keil AC6 toolchain and links it against a given region size,
so the real ROM footprint can be measured without opening uVision.

Usage:
    python bl_measure.py            # default: 16KB region, several opt levels
"""
import subprocess
import os
import tempfile
import re

KEIL = r'C:\Users\11846\AppData\Local\Keil_v5\ARM\ARMCLANG\bin'
CC = os.path.join(KEIL, 'armclang.exe')
AS = os.path.join(KEIL, 'armasm.exe')
LD = os.path.join(KEIL, 'armlink.exe')

ROOT = r'C:\Users\11846\Desktop\Git_Code\MyProjects_Develop\LUMOS-bootloader'
PROJ = os.path.join(ROOT, 'Bootloader')

INCLUDES = ['-I', os.path.join(PROJ, 'config'),
            '-I', os.path.join(PROJ, 'core'),
            '-I', os.path.join(PROJ, 'port'),
            '-I', os.path.join(PROJ, 'target', 'stm32f4'),
            '-I', os.path.join(ROOT, 'Core', 'Inc'),
            '-I', os.path.join(ROOT, 'UserApp'),
            '-I', os.path.join(ROOT, 'Bsp', 'usart'),
            '-I', os.path.join(ROOT, 'Drivers', 'STM32F4xx_HAL_Driver', 'Inc'),
            '-I', os.path.join(ROOT, 'Drivers', 'STM32F4xx_HAL_Driver', 'Inc', 'Legacy'),
            '-I', os.path.join(ROOT, 'Drivers', 'CMSIS', 'Device', 'ST', 'STM32F4xx', 'Include'),
            '-I', os.path.join(ROOT, 'Drivers', 'CMSIS', 'Include')]

# only the HAL modules this bootloader actually needs
HAL_NEED = ['stm32f4xx_hal.c', 'stm32f4xx_hal_cortex.c', 'stm32f4xx_hal_rcc.c',
            'stm32f4xx_hal_rcc_ex.c', 'stm32f4xx_hal_gpio.c', 'stm32f4xx_hal_uart.c',
            'stm32f4xx_hal_flash.c', 'stm32f4xx_hal_flash_ex.c',
            'stm32f4xx_hal_flash_ramfunc.c', 'stm32f4xx_hal_pwr.c',
            'stm32f4xx_hal_pwr_ex.c', 'stm32f4xx_hal_dma.c']

CORE_CPP = ['stm32f4xx_hal_msp.c']  # from Core/Src, plus the ones below
CORE_SRC = ['main.c', 'gpio.c', 'usart.c', 'stm32f4xx_it.c', 'system_stm32f4xx.c',
            'stm32f4xx_hal_msp.c']

BOOT_CPP = [('core', 'bl_log.cpp'), ('core', 'bl_crc.cpp'), ('core', 'bl_meta.cpp'),
            ('core', 'bl_ymodem.cpp'), ('core', 'bl_verify.cpp'),
            ('core', 'bl_session.cpp'), ('core', 'bl_boot.cpp'),
            ('target/stm32f4', 'bl_port_flash_stm32f4.cpp'),
            ('target/stm32f4', 'bl_port_uart_stm32f4.cpp'),
            ('target/stm32f4', 'bl_port_system_stm32f4.cpp'),
            ('app', 'bl_main.cpp')]


def build(tag, opt, extra_def, region=0x4000, quiet=False):
    out = tempfile.mkdtemp(prefix='bl_%s_' % tag)
    defs = ['-DUSE_HAL_DRIVER', '-DSTM32F405xx'] + extra_def
    common = ['--target=arm-arm-none-eabi', '-mcpu=cortex-m4', '-mfpu=fpv4-sp-d16',
              '-mfloat-abi=hard', '-c', opt, '-Wall', '-Wextra'] + defs + INCLUDES
    objs, errs = [], []

    for d, f in BOOT_CPP:
        src = os.path.join(PROJ, d.replace('/', os.sep), f)
        o = os.path.join(out, os.path.splitext(f)[0] + '.o')
        r = subprocess.run([CC] + common + ['-std=c++14', '-fno-exceptions', '-fno-rtti',
                                            src, '-o', o], capture_output=True, text=True)
        (objs if r.returncode == 0 else errs).append(o if r.returncode == 0 else
                                                     (f, (r.stderr or '')[:300]))

    for h in HAL_NEED:
        src = os.path.join(ROOT, 'Drivers', 'STM32F4xx_HAL_Driver', 'Src', h)
        o = os.path.join(out, os.path.splitext(h)[0] + '.o')
        r = subprocess.run([CC] + common + ['-std=c99', src, '-o', o],
                           capture_output=True, text=True)
        if r.returncode == 0:
            objs.append(o)
        else:
            errs.append((h, (r.stderr or '')[:200]))

    for f in CORE_SRC:
        src = os.path.join(ROOT, 'Core', 'Src', f)
        o = os.path.join(out, os.path.splitext(f)[0] + '.o')
        r = subprocess.run([CC] + common + ['-std=c99', src, '-o', o],
                           capture_output=True, text=True)
        if r.returncode == 0:
            objs.append(o)
        else:
            errs.append((f, (r.stderr or '')[:200]))

    so = os.path.join(out, 'startup.o')
    subprocess.run([AS, '--cpu=Cortex-M4', '--apcs=interwork', '-g',
                    os.path.join(ROOT, 'MDK-ARM', 'startup_stm32f405xx.s'), '-o', so],
                   capture_output=True)
    objs.append(so)

    if errs:
        print('   编译错误:')
        for f, e in errs:
            print('     ', f, '->', e.strip()[:250])
        return None

    sct = os.path.join(out, 's.sct')
    with open(sct, 'w') as fp:
        fp.write('LR_IROM1 0x08000000 0x%08X {\n  ER_IROM1 0x08000000 0x%08X {\n'
                 '   startup.o (RESET, +First)\n   *(InRoot$$Sections)\n   .ANY (+RO)\n  }\n'
                 '  RW_IRAM1 0x20000000 0x00020000 {\n   .ANY (+RW +ZI)\n  }\n}\n'
                 % (region, region))

    elf = os.path.join(out, 'bl.elf')
    r = subprocess.run([LD, '--scatter', sct, '--entry', 'Reset_Handler',
                        '--info', 'sizes', '-o', elf] + objs,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('   链接失败（超出 %d KB 区域）:' % (region // 1024))
        for l in (r.stderr or '').splitlines()[:3]:
            print('     ', l.strip()[:160])
        return None

    m = re.search(r'Total RO\s+Size \(Code \+ RO Data\)\s+(\d+)', r.stdout)
    rom = re.search(r'Total ROM Size.*?(\d+)\s*\(', r.stdout)
    if m:
        ro = int(m.group(1))
        free = region - ro
        print('   RO=%6d B (%5.2f KB)  ROM=%5s B  余量=%6d B  %s'
              % (ro, ro / 1024, rom.group(1) if rom else '?', free,
                 '✔ 放得下' if free > 0 else '✘ 超出'))
        return ro
    return None


if __name__ == '__main__':
    print('=' * 68)
    print('装入 16KB 区域 (0x08000000 / 0x4000) 的真实结果')
    print('=' * 68)
    for label, opt, d in [
            ('① -Os  + 轻量日志（推荐）', '-Os', []),
            ('② -O2  + 轻量日志', '-O2', []),
            ('③ -Oz  + 轻量日志', '-Oz', []),
            ('④ -Os  + 关日志（发布版）', '-Os', ['-DBL_DEBUG_LOG=0']),
    ]:
        print()
        print(label)
        build('opt', opt, d)
