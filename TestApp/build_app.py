#!/usr/bin/env python3
"""
构建 Bootloader-Everywhere 的测试 APP。

产物链接到 0x08004000（BL_APP_BASE），供 Bootloader 通过串口升级。

用法：
    python build_app.py                 # 编译并生成 build/app_test.bin
    python build_app.py --map           # 额外打印符号表摘要

依赖 Keil MDK 的 AC6 工具链（armclang / armasm / armlink / fromelf）。
安装路径不同时用环境变量 KEIL_ARMCLANG_BIN 指定。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# ---- 路径 ------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # .../Bootloader-Everywhere

KEIL = os.environ.get(
    "KEIL_ARMCLANG_BIN",
    r"C:\Users\11846\AppData\Local\Keil_v5\ARM\ARMCLANG\bin")

CC = os.path.join(KEIL, "armclang.exe")
AS = os.path.join(KEIL, "armasm.exe")
LD = os.path.join(KEIL, "armlink.exe")
FE = os.path.join(KEIL, "fromelf.exe")

# ---- 目标镜像参数（必须与 Bootloader 的 bl_config.h 一致）------------------
APP_BASE = 0x08004000
APP_SIZE = 0x0005C000                             # 368 KB（S1-S6）

CFLAGS = [
    "--target=arm-arm-none-eabi",
    "-mcpu=cortex-m4",
    "-mfpu=fpv4-sp-d16",
    "-mfloat-abi=hard",
    "-c",
    "-Oz",
    "-std=c99",
    "-Wall",
    "-Wextra",
]


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("  ✘ %s 失败 (rc=%d)" % (what, r.returncode))
        out = (r.stderr or r.stdout or "").strip()
        for line in out.splitlines()[:14]:
            print("     ", line[:160])
        return None
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None,
                    help="输出的 .bin 路径。默认 app_test.bin，"
                         "故障固件为 app_fail<N>.bin")
    ap.add_argument("--fail", type=int, default=0, choices=(0, 1, 2),
                    help="故障注入（构造坏固件用）："
                         "1 = 挂死；2 = 启动即 HardFault")
    ap.add_argument("--map", action="store_true", help="打印符号表摘要")
    args = ap.parse_args()

    if args.out is None:
        name = "app_test.bin" if args.fail == 0 else "app_fail%d.bin" % args.fail
        args.out = os.path.join(ROOT, "build", name)

    for tool in (CC, AS, LD, FE):
        if not os.path.exists(tool):
            print("★ 找不到工具:", tool)
            sys.exit(1)

    work = os.path.join(ROOT, "build", "testapp")
    os.makedirs(work, exist_ok=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    print("=" * 66)
    print("构建测试 APP  →  0x%08X + %d KB" % (APP_BASE, APP_SIZE // 1024))
    if args.fail:
        print("⚠ 故障注入模式 APP_FAIL_MODE=%d （%s）"
              % (args.fail,
                 "挂死" if args.fail == 1 else "启动即 HardFault"))
    print("=" * 66)

    # 1) 汇编启动文件
    obj_startup = os.path.join(work, "startup_app.o")
    r = run([AS, "--cpu=Cortex-M4", "--apcs=interwork", "-g",
             os.path.join(HERE, "startup_app.s"), "-o", obj_startup],
            "armasm startup_app.s")
    if r is None:
        return 1
    print("  ✔ startup_app.s  汇编完成")

    # 2) 编译 C 源码
    obj_main = os.path.join(work, "app_main.o")
    cflags = CFLAGS + ["-DAPP_FAIL_MODE=%d" % args.fail]
    r = run([CC] + cflags + [os.path.join(HERE, "app_main.c"), "-o", obj_main],
            "armclang app_main.c")
    if r is None:
        return 1
    if r.stderr.strip():
        for line in r.stderr.strip().splitlines()[:8]:
            print("     W", line[:150])
    print("  ✔ app_main.c    编译完成")

    # 3) 链接
    import tempfile
    sct = os.path.join(work, "link_app.sct")
    # 生成一份把 APP 尺寸写死的 scatter，便于单独校验边界
    with open(os.path.join(HERE, "link_app.sct"), encoding="utf-8") as f:
        sct_txt = f.read()
    with open(sct, "w", encoding="utf-8", newline="\n") as f:
        f.write(sct_txt)

    axf = os.path.join(work, "app_test.axf")
    r = run([LD, "--scatter", sct, "--entry", "Reset_Handler",
             "--info", "sizes", "--info", "totals",
             "-o", axf, obj_startup, obj_main], "armlink")
    if r is None:
        return 1

    print()
    print("--- 链接结果 ---")
    for line in (r.stdout or "").splitlines():
        if "Total RO" in line or "Total RW" in line or "Total ROM" in line:
            print("  " + line.strip())

    m = re.search(r"Total RO\s+Size \(Code \+ RO Data\)\s+(\d+)", r.stdout or "")
    ro = int(m.group(1)) if m else 0

    # 4) 生成 bin / hex
    bin_path = args.out
    hex_path = os.path.splitext(bin_path)[0] + ".hex"
    run([FE, "--bin", "--output", bin_path, axf], "fromelf --bin")
    run([FE, "--i32", "--output", hex_path, axf], "fromelf --i32")

    print()
    print("--- 体积 ---")
    print("  RO 占用      : %d B (%.2f KB)" % (ro, ro / 1024.0))
    print("  APP 区上限   : %d B (%d KB)" % (APP_SIZE, APP_SIZE // 1024))
    print("  余量         : %d B (%.2f KB)" % (APP_SIZE - ro, (APP_SIZE - ro) / 1024.0))
    print("  镜像起点     : 0x%08X   镜像末尾 ≈ 0x%08X"
          % (APP_BASE, APP_BASE + ro))
    if os.path.exists(bin_path):
        print("  bin          : %s (%d B)" % (bin_path, os.path.getsize(bin_path)))
    if os.path.exists(hex_path):
        print("  hex          : %s (%d B)" % (hex_path, os.path.getsize(hex_path)))

    if args.map:
        print()
        print("--- 关键符号 ---")
        r2 = run([FE, "--text", "-s", axf], "fromelf -s")
        if r2:
            for line in (r2.stdout or "").splitlines():
                if re.search(r"\b(Reset_Handler|main|SystemInit|__Vectors|__initial_sp)\b", line):
                    print("  " + line.strip()[:120])

    print()
    print("✔ 完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
