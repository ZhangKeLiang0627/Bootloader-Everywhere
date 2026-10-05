#!/usr/bin/env python3
"""
YMODEM-1K 发送端 —— Bootloader-Everywhere 的串口升级上位机（命令行版）

用法：
    python ymodem_send.py COM3 build/app_test.bin
    python ymodem_send.py COM3 build/app_test.bin -b 115200 -v

协议要点（与 bootloader 的 core/bl_ymodem 对齐）：
    * 接收方先发 'C'（0x43）表示「用 CRC 模式，请开始」
    * 首包用 SOH(128B)：文件名\\0 + 十进制大小 + 填充
    * 数据包用 STX(1024B)，序号从 1 开始，逐包等 ACK
    * 传输结束发 EOT，接收方 ACK 后再发 'C' 请批次结束包
    * 批次结束包 = SOH(128B) 全 0，接收方 ACK 后结束
    * 每包附 CRC16/XMODEM（高字节在前）

为什么 CRC 由板子自己算：本脚本**不需要**传 CRC32 过去。
    YMODEM 的帧级 CRC16 + ACK/NAK 已经保证了「PC → 板子」的传输正确性，
    整镜像 CRC32 是板子用来防 Flash 位翻转的，由它自己算并存下来。
    所以标准工具（SecureCRT / lrzsz）无需改造也能用。
"""

import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    print("★ 需要 pyserial：pip install pyserial")
    sys.exit(1)

# ---- 协议常量 --------------------------------------------------------------
SOH = 0x01          # 128 字节包
STX = 0x02          # 1024 字节包
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC_REQ = 0x43      # 'C'

PKT_128 = 128
PKT_1024 = 1024

# ---- 超时与重试 ------------------------------------------------------------
WAIT_C_TIMEOUT = 20.0       # 等接收方的 'C'（bootloader 约每 3 秒发一次）
ACK_TIMEOUT = 3.0           # 等单包 ACK
MAX_RETRY = 10


def crc16_xmodem(data: bytes) -> int:
    """CRC-16/XMODEM：poly=0x1021, init=0x0000（与板端 Crc16 一致）"""
    crc = 0
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return crc


class YmodemSender:
    def __init__(self, ser: serial.Serial, verbose: bool = False):
        self.ser = ser
        self.verbose = verbose
        self.stats = {"packets": 0, "retries": 0, "bytes": 0}
        self.board_log = bytearray()        # 累积板端夹在协议里的日志文本

    # -- 基础收发 ----------------------------------------------------------
    def log(self, msg: str):
        print(msg, flush=True)

    def vlog(self, msg: str):
        if self.verbose:
            print("    " + msg, flush=True)

    def read_byte(self, timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            b = self.ser.read(1)
            if b:
                return b[0]
            time.sleep(0.001)
        return None

    def drain(self):
        n = self.ser.in_waiting
        if n:
            self.ser.read(n)

    def wait_for(self, wanted: int, timeout: float, label: str):
        """等一个特定字节，忽略中间的其它内容（并记下来）"""
        deadline = time.time() + timeout
        seen = []
        while time.time() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            v = b[0]
            if v == wanted:
                return True
            if v == CAN:
                self.log("  ✘ 接收方发送 CAN（取消）")
                return False
            seen.append(v)
            if self.verbose:
                self.vlog("忽略字节 0x%02X (%r)" % (v, chr(v) if 32 <= v < 127 else '.'))
        self.log("  ✘ 等待 %s 超时（%.1fs）" % (label, timeout))
        if seen:
            self.log("     期间收到: " + " ".join("%02X" % x for x in seen[:24]))
        return False

    # -- 发包 --------------------------------------------------------------
    def send_packet(self, seq: int, payload: bytes, expect_ack: bool = True,
                    label: str = "", ack_timeout: float = ACK_TIMEOUT) -> bool:
        """发一包并等 ACK，超时按 MAX_RETRY 重传

        ack_timeout 单独可调：首包要等板子把 APP 扇区擦完才回 ACK
        （单扇区典型 0.3-0.5s、最坏 4s），所以给它更长的窗口。
        """
        size = PKT_128 if len(payload) == PKT_128 else PKT_1024
        head = SOH if size == PKT_128 else STX
        crc = crc16_xmodem(payload)
        frame = bytes([head, seq & 0xFF, (~seq) & 0xFF]) + payload + \
                bytes([(crc >> 8) & 0xFF, crc & 0xFF])

        for attempt in range(1, MAX_RETRY + 1):
            self.ser.write(frame)
            self.ser.flush()

            if not expect_ack:
                return True

            # 板子的调试日志和 YMODEM 的 ACK 共用同一个串口，
            # 所以这里必须容忍中间夹着的文本字节，只挑协议字节处理，
            # 把其余内容原样打印出来（同时也是一份很好的现场记录）。
            got = None
            noise = bytearray()
            deadline = time.time() + ack_timeout
            while time.time() < deadline:
                b = self.read_byte(deadline - time.time())
                if b is None:
                    break
                if b in (ACK, NAK, CAN):
                    got = b
                    break
                noise.append(b)
                if len(noise) >= 8192:
                    break

            if noise:
                txt = noise.decode("utf-8", "replace").replace("\r", "")
                for line in txt.split("\n"):
                    if line.strip():
                        print("    [板端] %s" % line.strip()[:130], flush=True)
                self.board_log.extend(noise)

            if got == ACK:
                self.stats["packets"] += 1
                self.stats["bytes"] += len(payload)
                return True
            if got == NAK:
                self.stats["retries"] += 1
                self.log("  ! 第 %d 包收到 NAK，重传 (%d/%d)" % (seq, attempt, MAX_RETRY))
                self.drain()
                continue
            if got == CAN:
                self.log("  ✘ 第 %d 包收到 CAN" % seq)
                return False

            # 超时（没等到任何协议字节）
            self.stats["retries"] += 1
            self.log("  ! 第 %d 包%s 等 ACK 超时（%.1fs），重传 (%d/%d)"
                     % (seq, (" " + label) if label else "", ack_timeout,
                        attempt, MAX_RETRY))
            self.drain()
        return False
        return False

    def send_eot(self) -> bool:
        """EOT 可能需要发两次（部分实现第一遍回 NAK）"""
        for attempt in range(3):
            self.ser.write(bytes([EOT]))
            self.ser.flush()
            r = self.read_byte(ACK_TIMEOUT)
            if r == ACK:
                return True
            if r == NAK:
                self.vlog("EOT 收到 NAK，再发一次（正常流程）")
                continue
            if r is None:
                self.vlog("EOT 等 ACK 超时，重发")
                continue
            self.vlog("EOT 收到 0x%02X，重发" % r)
        return False

    # -- 主流程 ------------------------------------------------------------
    def send_file(self, path: str) -> bool:
        name = os.path.basename(path).encode("ascii", "replace")
        data = open(path, "rb").read()
        total = len(data)

        self.log("文件   : %s" % path)
        self.log("大小   : %d 字节 (%.2f KB)" % (total, total / 1024.0))
        self.log("分块   : %d 个 1024B 数据包 + 首包 + 结束包"
                 % ((total + PKT_1024 - 1) // PKT_1024))
        self.log("")

        self.log("[1] 等待接收方握手字符 'C' ...")
        if not self.wait_for(CRC_REQ, WAIT_C_TIMEOUT, "'C'"):
            return False
        self.log("    收到 'C' —— 接收方已就绪")

        # ---- 首包：文件名 + 大小 ----
        head_payload = name + b"\x00" + str(total).encode("ascii")
        head_payload = head_payload.ljust(PKT_128, b"\x00")[:PKT_128]
        self.log("[2] 发送首包（文件名 + 大小）...")
        if not self.send_packet(0, head_payload, expect_ack=True, label="首包",
                                ack_timeout=12.0):
            return False
        self.log("    首包已确认（此刻板子正在擦除 APP 扇区，可能停顿 1-2 秒）")

        # ---- 等接收方再发 'C'，表示可以发数据了 ----
        if not self.wait_for(CRC_REQ, WAIT_C_TIMEOUT, "数据阶段的 'C'"):
            return False
        self.log("    收到 'C' —— 开始传输数据")

        # ---- 数据包 ----
        self.log("[3] 传输数据 ...")
        seq = 1
        off = 0
        t0 = time.time()
        next_report = 0.0

        while off < total:
            chunk = data[off:off + PKT_1024]
            if len(chunk) < PKT_1024:
                chunk = chunk.ljust(PKT_1024, b"\x1A")     # Ctrl-Z 填充（不影响 CRC 口径）
            if not self.send_packet(seq, chunk):
                self.log("  ✘ 第 %d 包发送失败，中止" % seq)
                self.ser.write(bytes([CAN]) * 8)
                return False

            off += PKT_1024
            seq += 1

            done = min(off, total)
            pct = done * 100.0 / total
            now = time.time()
            if now >= next_report or done >= total:
                el = now - t0
                kbps = (done / 1024.0 / el) if el > 0 else 0
                print("    %6.1f%%  %7d / %d 字节  %.1f KB/s"
                      % (pct, done, total, kbps), flush=True)
                next_report = now + 0.5

        # ---- EOT ----
        self.log("[4] 发送 EOT ...")
        if not self.send_eot():
            self.log("  ✘ EOT 未被确认")
            return False
        self.log("    EOT 已确认")

        # ---- 批次结束包 ----
        self.log("[5] 发送批次结束包 ...")
        if not self.wait_for(CRC_REQ, WAIT_C_TIMEOUT, "结束阶段的 'C'"):
            # 结束帧缺失不该让升级失败（数据已收全），仅提示
            self.log("  ! 未收到结束阶段的 'C' —— 数据已传完，通常不影响结果")
            return True

        end_payload = b"\x00" * PKT_128
        if not self.send_packet(0, end_payload, expect_ack=True, label="结束包"):
            self.log("  ! 结束包未确认 —— 数据已传完，通常不影响结果")
            return True

        self.log("    结束包已确认")
        return True


def main():
    ap = argparse.ArgumentParser(description="YMODEM-1K 发送端（Bootloader-Everywhere 上位机）")
    ap.add_argument("port", nargs="?", help="串口，如 COM3")
    ap.add_argument("file", nargs="?", help="要发送的 .bin")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("-v", "--verbose", action="store_true", help="输出每包细节")
    ap.add_argument("--check", action="store_true", help="只做 CRC16 自检后退出")
    args = ap.parse_args()

    if args.check:
        v = b"123456789"
        got = crc16_xmodem(v)
        print("CRC16/XMODEM('123456789') = 0x%04X  期望 0x31C3  %s"
              % (got, "PASS" if got == 0x31C3 else "FAIL"))
        return 0 if got == 0x31C3 else 1

    if not args.port or not args.file:
        ap.error("需要指定串口与文件（或使用 --check）")

    if not os.path.exists(args.file):
        print("★ 文件不存在:", args.file)
        return 1

    print("=" * 62)
    print("YMODEM-1K 发送   %s @ %d 8N1" % (args.port, args.baud))
    print("=" * 62)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except Exception as e:
        print("★ 打开串口失败:", e)
        return 1

    try:
        ser.reset_input_buffer()
        sender = YmodemSender(ser, verbose=args.verbose)
        ok = sender.send_file(args.file)
    except KeyboardInterrupt:
        print("\n^C 中断")
        ok = False
    finally:
        ser.close()

    print()
    print("=" * 62)
    if ok:
        print("✔ 传输完成：%d 包，%d 字节，重传 %d 次"
              % (sender.stats["packets"], sender.stats["bytes"], sender.stats["retries"]))
        print("  板子随后会自行复位并跳转到新固件 —— 留意串口后续输出。")
    else:
        print("✘ 传输失败：%d 包已确认，重传 %d 次"
              % (sender.stats["packets"], sender.stats["retries"]))
    print("=" * 62)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
