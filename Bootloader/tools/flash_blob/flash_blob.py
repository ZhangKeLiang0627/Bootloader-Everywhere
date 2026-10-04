"""
在目标 RAM 里跑小程序来完成 Flash 编程 —— 绕过调试探针的总线限制。

什么时候需要它
--------------
某些 CMSIS-DAP 探针（固件实现不完整）在目标 Flash 编程期间无法处理
AHB 总线拉停，会返回非法 ACK；而标准 CMSIS-Pack 算法也可能返回错误码。
两者的共同点是「宿主一直在参与 Flash 访问」。
本工具把编程整个搬到目标侧：宿主只写 RAM、轮询 RAM，从不碰 Flash。

先用一次判断是否真的需要它：
    python tools/board.py flash app.bin
如果报 "flash program page failure" 或 "Unexpected ACK"，就用本工具。

用法
----
    python tools/flash_blob/flash_blob.py build/bl_f401.bin
    python tools/flash_blob/flash_blob.py bin --target stm32f401retx --addr 0x08000000
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))        # .../Bootloader
ROOT = os.path.dirname(PROJ)

KEIL = os.environ.get(
    'KEIL_ARMCLANG_BIN',
    r'C:\Users\11846\AppData\Local\Keil_v5\ARM\ARMCLANG\bin')
CC = os.path.join(KEIL, 'armclang.exe')
LD = os.path.join(KEIL, 'armlink.exe')
FE = os.path.join(KEIL, 'fromelf.exe')

# ---- RAM 布局（以 STM32F401 的 96KB SRAM 为例，按需调整） ----
# 注意各段不要重叠：状态字占 4 个字，spin 指令必须放在状态区之外，
# 否则写 spin 会把状态字覆盖掉（踩过一次）。
BLOB_BASE = 0x20008000          # 小程序代码
STATUS_ADDR = 0x20009000        # 状态字（4 个 word）
SPIN_ADDR = 0x20009020          # 这条 B . 指令用于接住「函数返回」
DATA_BASE = 0x2000A000          # 待写入的镜像
STACK_TOP = 0x20018000          # 96KB SRAM 的末尾

BLOB_LIMIT = 0x2000             # 小程序最大 8KB

ST_RUNNING = 0xE1
ST_NAMES = {0x00: 'OK', 0xE1: 'RUNNING', 0xE2: 'BUSY 卡住',
            0xE3: '编程出错', 0xE4: '超时'}


def build_blob(outdir):
    """编译出可在目标 RAM 里跑的编程小程序。"""
    src = os.path.join(HERE, 'prog_blob.c')
    obj = os.path.join(outdir, 'prog_blob.o')
    elf = os.path.join(outdir, 'prog_blob.elf')
    binf = os.path.join(outdir, 'prog_blob.bin')

    r = subprocess.run([CC, '--target=arm-arm-none-eabi', '-mcpu=cortex-m4',
                        '-mfloat-abi=soft', '-Os', '-c', '-ffreestanding',
                        '-Wall', src, '-o', obj], capture_output=True, text=True)
    if r.returncode != 0:
        print('编译小程序失败:\n' + (r.stderr or r.stdout)[:800])
        return None

    sct = os.path.join(outdir, 'blob.sct')
    with open(sct, 'w') as fp:
        fp.write('LR_BLOB 0x%08X 0x%08X {\n'
                 '  ER_BLOB 0x%08X 0x%08X {\n'
                 '   *(.text.bl_prog)\n'
                 '   *(.text*)\n'
                 '   *(.rodata*)\n'
                 '  }\n}\n' % (BLOB_BASE, BLOB_LIMIT, BLOB_BASE, BLOB_LIMIT))

    r = subprocess.run([LD, '--scatter', sct, '--entry', 'bl_prog',
                        '-o', elf, obj], capture_output=True, text=True)
    if r.returncode != 0:
        print('链接小程序失败:\n' + (r.stderr or r.stdout)[:800])
        return None

    r = subprocess.run([FE, '--bin', '--output', binf, elf],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(binf):
        print('导出小程序二进制失败:\n' + (r.stderr or r.stdout)[:500])
        return None

    data = open(binf, 'rb').read()
    # 去掉尾部填充，只写真正用到的部分
    end = len(data)
    while end > 0 and data[end - 1] == 0xFF:
        end -= 1
    data = data[:max(end, 4)]
    print('小程序 %d 字节（装入 0x%08X）' % (len(data), BLOB_BASE))
    return data


def main():
    ap = argparse.ArgumentParser(description='RAM 小程序方式烧写 Flash')
    ap.add_argument('file', help='待烧写的 .bin')
    ap.add_argument('--target', default='stm32f401retx')
    ap.add_argument('--addr', type=lambda x: int(x, 0), default=0x08000000)
    ap.add_argument('--freq', type=int, default=100000)
    ap.add_argument('--timeout', type=float, default=60.0, help='编程等待上限（秒）')
    args = ap.parse_args()

    import tempfile
    outdir = tempfile.mkdtemp(prefix='blob_')
    blob = build_blob(outdir)
    if blob is None:
        return 2

    img = open(args.file, 'rb').read()
    img += b'\xff' * ((4 - len(img) % 4) % 4)
    print('镜像 %d 字节 → %d 个字' % (len(img), len(img) // 4))

    if len(blob) > BLOB_LIMIT:
        print('小程序太大')
        return 2
    if DATA_BASE + len(img) >= STACK_TOP:
        print('镜像超出 RAM 布局，请调整 DATA_BASE / STACK_TOP')
        return 2

    from pyocd.core.helpers import ConnectHelper

    s = ConnectHelper.session_with_chosen_probe(
        target_override=args.target, frequency=args.freq,
        options={'connect_mode': 'under-reset', 'resume_on_disconnect': False})
    s.open()
    t = s.target
    t.halt()

    try:
        # 1) 小程序 + 数据 + 状态 + 返回落点，全部写到 RAM
        t.write_memory_block8(BLOB_BASE, blob)
        t.write_memory_block8(DATA_BASE, img)
        # 状态字先写成 RUNNING：这样"没跑起来"会表现为超时，而不是被误判成完成
        t.write32(STATUS_ADDR, ST_RUNNING)
        t.write32(STATUS_ADDR + 4, 0)
        t.write32(SPIN_ADDR, 0xE7FEE7FE)            # B .  —— 接住函数返回
        print('已写入小程序与镜像到 RAM')

        # 2) 设置寄存器：r0=src r1=dst r2=len r3=status, lr=spin, pc=blob
        regs = [0, 1, 2, 3, 13, 14, 15]
        vals = [DATA_BASE, args.addr, len(img), STATUS_ADDR,
                STACK_TOP, SPIN_ADDR | 1, BLOB_BASE | 1]
        t.write_core_registers_raw(regs, vals)
        print('寄存器就绪，放开核心运行...')

        # 3) 放开运行，然后只轮询 RAM（不碰 Flash）
        t.resume()

        end = time.time() + args.timeout
        state, done = ST_RUNNING, 0
        while time.time() < end:
            try:
                state = t.read32(STATUS_ADDR)
                done = t.read32(STATUS_ADDR + 4)
            except Exception as e:
                print('  轮询失败: %s' % str(e)[:80])
            if state != ST_RUNNING:
                break
            sys.stdout.write('\r  进度: %d / %d 字' % (done, len(img) // 4))
            sys.stdout.flush()
            time.sleep(0.05)
        print()
        print('状态字 = 0x%02X (%s)，已写入 %d / %d 字'
              % (state, ST_NAMES.get(state, '未知'), done, len(img) // 4))
        if state != 0:
            sr = t.read32(STATUS_ADDR + 8)
            cr = t.read32(STATUS_ADDR + 12)
            print('  FLASH->SR = 0x%08X  (BSY=%d OPERR=%d WRPERR=%d PGAERR=%d PGPERR=%d PGSERR=%d)'
                  % (sr, (sr >> 16) & 1, (sr >> 1) & 1, (sr >> 4) & 1,
                     (sr >> 5) & 1, (sr >> 6) & 1, (sr >> 7) & 1))
            print('  FLASH->CR = 0x%08X  (PG=%d PSIZE=%d LOCK=%d)'
                  % (cr, cr & 1, (cr >> 8) & 3, (cr >> 31) & 1))
            print('★ 编程未成功')
            return 3

        # 4) 回读校验
        t.halt()
        back = bytes(t.read_memory_block8(args.addr, len(img)))
        if back == img:
            print('★ 回读校验一致 —— 烧写成功')
            return 0
        bad = [i for i, (a, b) in enumerate(zip(img, back)) if a != b]
        print('★ 回读不一致：%d 字节不同，首个偏移 %d (0x%08X)'
              % (len(bad), bad[0], args.addr + bad[0]))
        return 4
    finally:
        try:
            t.halt()
        except Exception:
            pass
        try:
            t.resume()
        except Exception:
            pass
        s.close()


if __name__ == '__main__':
    sys.exit(main())
