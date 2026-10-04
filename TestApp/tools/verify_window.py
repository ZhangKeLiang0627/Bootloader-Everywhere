"""
验证「软件复位唤回 + 15 秒窗口」机制（bootloader 已用 Keil 烧好）。

链路：
  1) 复位观察当前状态
  2) 确保进 IAP（若跳 APP 则软复位唤回进窗口）
  3) YMODEM 升级 app_test.bin
  4) 观察升级完成 → 状态 Testing 第一次运行，decision 应为 JUMP（不误进窗口）
  5) 引脚复位 → clean boot 转 Valid
  6) 探针写 AIRCR 软复位 → 期望 decision=IAP_TIMED（软件复位 + Valid）
  7) 等 15s 无上位机 → 期望跳回 APP（upgrade window timeout）

用法：python verify_window.py
"""
import sys, os, time, serial

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from pyocd.core.helpers import ConnectHelper
from ymodem_send import YmodemSender

TARGET = 'stm32f401retx'
PORT   = 'COM3'
BAUD   = 115200

ROOT = os.path.dirname(os.path.dirname(TOOLS))
APP  = os.path.join(ROOT, 'build', 'app_test.bin')


def open_probe(freq=1000000):
    return ConnectHelper.session_with_chosen_probe(
        target_override=TARGET, frequency=freq,
        options={'resume_on_disconnect': True})


def pin_reset():
    s = open_probe()
    try:
        s.open()
        s.target.reset_and_halt()
        s.target.resume()
    finally:
        s.close()


def soft_reset():
    s = open_probe()
    try:
        s.open()
        t = s.target
        t.halt()
        t.write32(0xE000ED0C, 0x05FA0004)   # SCB->AIRCR = VECTKEY | SYSRESETREQ
    finally:
        s.close()


def read_until(ser, seconds):
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
    return buf


def dump(tag, buf):
    txt = buf.decode('utf-8', 'replace')
    print('  --- %s ---' % tag)
    for line in txt.splitlines():
        s = line.strip()
        if s and s != 'C':
            print('    |', s[:120])
    return txt


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.05)

    print('=== 1. 复位观察当前状态 ===')
    ser.reset_input_buffer()
    pin_reset()
    time.sleep(1.5)
    buf = read_until(ser, 0.5)
    txt = dump('复位后', buf)
    in_iap = ('waiting for YMODEM' in txt or 'decision: IAP' in txt)

    if not in_iap:
        print()
        print('=== 1b. 跳了 APP，软复位唤回进窗口 ===')
        soft_reset()
        time.sleep(1.0)
        buf = read_until(ser, 0.5)
        txt = dump('软复位后', buf)
        in_iap = ('waiting for YMODEM' in txt or 'IAP_TIMED' in txt)

    print()
    print('=== 2. 升级 app_test.bin（in_iap=%s）===' % in_iap)
    sender = YmodemSender(ser, verbose=False)
    ok = sender.send_file(APP)
    print('  升级结果:', 'OK' if ok else 'FAIL')
    if not ok:
        print('★ 升级失败'); ser.close(); return

    print()
    print('=== 3. 升级完成后，观察是否误进窗口 ===')
    time.sleep(2.5)
    buf = read_until(ser, 1.5)
    txt = dump('升级完成后', buf)
    a_pass = ('decision: JUMP' in txt and 'IAP_TIMED' not in txt)
    print('  判定A（升级完成不误进窗口）:', 'PASS' if a_pass else 'CHECK')

    print()
    print('=== 4. 引脚复位让 APP 转 Valid ===')
    pin_reset()
    time.sleep(1.5)
    buf = read_until(ser, 1.0)
    txt = dump('引脚复位后', buf)
    b_pass = ('self-confirmed' in txt or 'VALID' in txt)
    print('  判定B（clean boot 转 Valid）:', 'PASS' if b_pass else 'CHECK')

    print()
    print('=== 5. 探针软复位，期望进限时窗口 IAP_TIMED ===')
    soft_reset()
    time.sleep(1.0)
    buf = read_until(ser, 1.0)
    txt = dump('软复位后', buf)
    got_window = ('IAP_TIMED' in txt)
    got_sft = ('reset cause = 3' in txt)
    print('  判定C（软复位进窗口）:',
          'PASS' if (got_window and got_sft) else 'CHECK')

    print()
    print('=== 6. 等 15s 无上位机，期望超时跳回 APP ===')
    buf = read_until(ser, 17.0)
    txt = dump('窗口期', buf)
    got_timeout = 'upgrade window timeout' in txt
    got_jump = 'jumping to app' in txt
    d_pass = got_timeout and got_jump
    print('  判定D（15s 超时跳回 APP）:', 'PASS' if d_pass else 'CHECK')

    ser.close()
    print()
    print('=== 汇总 ===')
    print('  A 升级完成不误进窗口 :', a_pass)
    print('  B clean boot 转 Valid:', b_pass)
    print('  C 软复位进窗口      :', got_window and got_sft)
    print('  D 15s 超时跳回 APP  :', d_pass)


if __name__ == '__main__':
    main()
