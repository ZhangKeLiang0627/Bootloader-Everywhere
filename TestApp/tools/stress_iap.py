#!/usr/bin/env python3
"""IAP 全链路压测（真板）。

在 test_proto.py（正常路径 T1-T5）与 test_proto_edge.py（边界 E1-E15）之上，
再压一层「持续性与对抗性」：

    S1  连续升级 xN            耐久：反复走完整流程，配置区/Flash 不能有累积错误
    S2  尺寸扫描               2K / 8K / 64K / 200K 的完整升级（含 END 提交）
    S3  每帧错误注入（采样）    某帧被改坏 -> 必须回 0x08 且偏移不推进 -> 重发正确帧 -> 仍能完成
    S4  应答丢失（幂等重发）    主机丢掉一次应答后重发同一帧 -> 从机必须原样重放上次应答
    S5  ★ 重放缓冲污染探测      成功应答后先插一条别的命令，再重发同一帧 ->
                               重放出来的必须仍是 DATA 应答（而不是那条别的命令的应答）
    S6  跳号                   跳过一帧 -> 0x07 + 期望地址 -> 从该地址续传 -> 完成
    S7  倒退帧                 发一个更早的地址 -> 幂等重放 -> 状态不变
    S8  未 START 直接 DATA      -> 0x06，且不得动 Flash
    S9  会话中途重启 START      START -> 几帧 -> 再 START -> 完成（无残留状态）
    S10 背靠背会话 xN          连续 START -> 3 帧 -> 放弃（不发 END），最后完整做一次
    S11 突发帧（不等应答）      一次灌 3 帧，观察从机能否全部处理（环形缓冲 512B < 一帧 533B）
    S12 RECEIVING 空闲超时      停 35 秒不发任何帧 -> 从机应回到 Idle（--slow 才跑）

用法：
    python stress_iap.py                    # 跑全部（含 12 项）
    python stress_iap.py --only S1,S3,S5
    python stress_iap.py --rounds 20        # S1 的轮数
    python stress_iap.py --slow             # 额外跑 S12（约 40 秒）
"""

import argparse
import os
import random
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from proto import (  # noqa: E402
    DIR_REPLY, CMD_START, CMD_DATA, CMD_END, CMD_STATUS, BLOCK_SIZE,
    DATA_HEAD, START_LEN, CODE_NAME, crc32_iso, encode, Parser, Link,
)

ROOT = os.path.dirname(os.path.dirname(HERE))
REAL_BIN = os.path.join(ROOT, 'build', 'app_test.bin')

PORT = 'COM3'
BAUD = 115200
DEV = 1
APP_BASE = 0x08004000
APP_SIZE = 496 * 1024

RESULTS = []


def record(tag, name, ok, detail=''):
    RESULTS.append((tag, name, bool(ok), detail))
    print('  [%s] %-4s %s%s' % (tag, 'PASS' if ok else 'FAIL', name,
                                ('  —— ' + detail) if detail else ''))
    sys.stdout.flush()


# ---------------------------------------------------------------- 链路小工具

def tx(link, cmd, data=b''):
    link.ser.reset_input_buffer()
    link.parser.reset()
    link.ser.write(encode(DEV, cmd, data))
    link.ser.flush()


def tx_raw(link, raw):
    link.ser.reset_input_buffer()
    link.parser.reset()
    link.ser.write(raw)
    link.ser.flush()


def rx(link, timeout=1.0, want=None):
    """收一条应答（任意命令或指定命令）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = link.ser.read(1)
        if not b:
            continue
        f = link.parser.feed(b[0])
        if f is None:
            continue
        if not f['reply'] or f['id'] != DEV:
            continue
        if want is not None and f['cmd'] != want:
            continue
        return f
    return None


def status(link, timeout=1.0):
    """STATUS 查询 -> (已写入偏移, 期望地址)；失败返回 None。"""
    tx(link, CMD_STATUS)
    f = rx(link, timeout, want=CMD_STATUS | DIR_REPLY)
    if f is None or len(f['data']) < 9 or f['data'][0] != 0x00:
        return None
    return struct.unpack('<II', f['data'][1:9])


def enter_iap(link, tries=24):
    """确保设备处在 IAP。已在则直接返回；否则发唤回关键字。"""
    if status(link) is not None:
        return True
    link.recall()
    time.sleep(0.3)
    link.flush()
    for _ in range(tries):
        if status(link) is not None:
            return True
        time.sleep(0.25)
    return False


def try_enter_iap(link, tries=24):
    """尝试进 IAP，但不要用唤回（用「APP 区是否可启动」之外的手段）。
    返回 (ok, method)：method 为 'iap' / 'recall' / 'fail'。"""
    if status(link) is not None:
        return True, 'iap'
    if enter_iap(link, tries):
        return True, 'recall'
    return False, 'fail'


# ---------------------------------------------------------------- 镜像构造

def real_image():
    raw = open(REAL_BIN, 'rb').read()
    if len(raw) < 16:
        raise SystemExit('★ build/app_test.bin 太小，先跑 TestApp/build_app.py')
    return raw


def sized_image(size, seed=0xC0FFEE):
    """在真固件后面补随机数据，凑成指定大小。

    尾部补的数据永远不会被执行（入口仍在向量表里的 PC），
    所以这仍是一份「能启动的真固件」，可以走完 END 提交并真的跳转。
    """
    base = real_image()
    if size < len(base):
        return base[:size] if size >= 16 else base
    rnd = random.Random(seed)
    pad = bytes(rnd.randrange(256) for _ in range(size - len(base)))
    return base + pad


def image_parts(img):
    sp, pc = struct.unpack('<II', img[:8])
    body = img[8:]
    return sp, pc, body, crc32_iso(body), (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE


def data_payload(body, offset, total_pkts):
    chunk = body[offset:offset + BLOCK_SIZE]
    idx = offset // BLOCK_SIZE
    addr = APP_BASE + 8 + offset
    cum = crc32_iso(body[:offset + len(chunk)])
    return addr, idx, chunk, cum, struct.pack('<IHHHI', addr, total_pkts, idx,
                                              len(chunk), cum) + chunk


# ---------------------------------------------------------------- 升级主流程

class Outcome(object):
    def __init__(self):
        self.ok = False
        self.frames = 0
        self.codes = {}            # {帧号: 首次应答码}
        self.dropped = 0           # 被主机故意丢掉的应答数
        self.dups = []             # 重复帧的 (idx, reply_cmd, code, nextAddr)
        self.clobber = []          # 污染实验的 (idx, reply_cmd, code)
        self.gaps = []             # 跳号时的 (idx, code, expect_addr)
        self.end_code = None
        self.end_written = 0
        self.stuck = False
        self.detail = ''


def upgrade(link, img, *, corrupt=None, drop_reply=None, dup_after=None,
            skip=None, clobber_after=None, do_end=True, progress=0,
            timeout=0.6, retries=6, verbose=False):
    """走完一次 START -> DATA(-> END)。

    corrupt      : {帧号}    第一次发该帧时把数据区改坏
    drop_reply   : {帧号}    第一次收到该帧的应答后丢掉（模拟回程丢失），然后重发
    dup_after    : {帧号}    该帧成功后，原样再发一次，记录应答（重复帧幂等）
    skip         : {帧号}    首次跳过该帧（发下一帧）以触发跳号
    clobber_after: {帧号}    该帧成功后，插一条 STATUS（污染重放缓冲），
                             然后原样重发该帧，记录应答命令码
    """
    corrupt = corrupt or set()
    drop_reply = drop_reply or set()
    dup_after = dup_after or set()
    skip = skip or set()
    clobber_after = clobber_after or set()

    sp, pc, body, crc_all, total_pkts = image_parts(img)
    out = Outcome()

    # ---- START ----
    tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
    f = rx(link, 12.0, want=CMD_START | DIR_REPLY)
    if f is None:
        out.detail = 'START 无应答'
        return out
    if f['data'][0] != 0x00:
        out.detail = 'START -> %s' % CODE_NAME.get(f['data'][0], f['data'][0])
        out.codes[-1] = f['data'][0]          # -1 = START 阶段的失败码
        return out

    # ---- DATA ----
    offset = 0
    last_off = -1
    stuck = 0
    skipped = set()

    while offset < len(body):
        if offset == last_off:
            stuck += 1
            if stuck > retries:
                out.stuck = True
                out.detail = '位置 %d 连续 %d 次无法推进' % (offset, stuck)
                return out
        else:
            stuck = 0
            last_off = offset

        idx = offset // BLOCK_SIZE

        # 首次遇到该帧时，若要求跳过，就发下一帧一次（制造跳号）
        if idx in skip and idx not in skipped:
            skipped.add(idx)
            nxt = offset + BLOCK_SIZE
            if nxt < len(body):
                _, _, _, _, bad = data_payload(body, nxt, total_pkts)
                tx(link, CMD_DATA, bad)
                g = rx(link, timeout, want=CMD_DATA | DIR_REPLY)
                code = g['data'][0] if g else None
                exp = struct.unpack('<I', g['data'][5:9])[0] if g and len(g['data']) >= 9 else 0
                out.gaps.append((idx, code, exp))
                if verbose:
                    print('    跳号：帧 %d 收到 %s，期望 0x%08X'
                          % (idx, CODE_NAME.get(code, code), exp))
            continue

        addr, _, chunk, cum, payload = data_payload(body, offset, total_pkts)
        reply = None

        for attempt in range(retries + 1):
            if attempt == 0 and idx in corrupt:
                corrupt.discard(idx)              # 只改坏一次：重传时必须发正确的内容
                bad = bytearray(payload)
                bad[DATA_HEAD + 1] ^= 0x40        # 改坏数据区一个 bit
                tx(link, CMD_DATA, bytes(bad))
            else:
                tx(link, CMD_DATA, payload)
            g = rx(link, timeout, want=CMD_DATA | DIR_REPLY)

            if attempt == 0 and idx in drop_reply:
                out.dropped += 1                  # 应答到了但主机不要（模拟回程丢失）
                continue
            reply = g
            break

        if reply is None:
            out.detail = '帧 %d 无应答' % idx
            return out

        code = reply['data'][0] if reply['data'] else None

        if code != 0x00:
            # codes 记的是「该帧第一次被拒的码」——被拒后我们会重传同一帧，
            # 若在这里按第一次应答写、后面又被成功应答覆盖，就看不出拒过了。
            if idx not in out.codes:
                out.codes[idx] = code
            exp = struct.unpack('<I', reply['data'][5:9])[0] if len(reply['data']) >= 9 else 0
            if exp >= APP_BASE + 8 and exp < APP_BASE + 8 + len(body):
                offset = exp - (APP_BASE + 8)     # 断点续传
                continue
            out.detail = '帧 %d -> %s（期望地址 0x%08X 无效）' % (
                idx, CODE_NAME.get(code, code), exp)
            return out

        out.frames += 1

        dev_crc, expect = struct.unpack('<II', reply['data'][1:9])
        # 只在从机确认的正是本帧时才比对它回读的累积 CRC32
        if expect == addr + len(chunk) and dev_crc != cum:
            out.detail = '帧 %d 从机回读 CRC32 0x%08X != 主机 0x%08X' % (idx, dev_crc, cum)
            return out

        offset = expect - (APP_BASE + 8)

        if progress and (idx % progress == 0):
            print('    %5.1f%%  %6d/%d' % (offset * 100.0 / len(body), offset, len(body)))

        # ---- 重复帧幂等 ----
        if idx in dup_after:
            tx(link, CMD_DATA, payload)
            d = rx(link, timeout, want=CMD_DATA | DIR_REPLY)
            dcode = d['data'][0] if d and d['data'] else None
            dexpect = (struct.unpack('<I', d['data'][5:9])[0]
                       if d and len(d['data']) >= 9 else 0)
            out.dups.append((idx, d['cmd'] if d else None, dcode, dexpect))
            if verbose:
                print('    重复帧 %d -> cmd=0x%02X %s 期望 0x%08X'
                      % (idx, d['cmd'] if d else 0, CODE_NAME.get(dcode, dcode), dexpect))

        # ---- 重放缓冲污染实验 ----
        if idx in clobber_after:
            tx(link, CMD_STATUS)                       # 一条会覆盖重放缓冲的命令
            c = rx(link, timeout, want=CMD_STATUS | DIR_REPLY)
            tx(link, CMD_DATA, payload)                # 再原样重发这一帧
            d = rx(link, timeout)                      # 收任意应答，看它到底是什么
            out.clobber.append((idx, d['cmd'] if d else None,
                                (d['data'][0] if d and d['data'] else None)))
            if verbose:
                print('    污染后再重发帧 %d -> 应答 cmd=0x%02X'
                      % (idx, d['cmd'] if d else 0))

    # ---- END ----
    if do_end:
        tx(link, CMD_END)
        f = rx(link, 6.0, want=CMD_END | DIR_REPLY)
        if f is None:
            out.detail = 'END 无应答'
            return out
        out.end_code = f['data'][0]
        out.end_written = (struct.unpack('<I', f['data'][1:5])[0]
                           if len(f['data']) >= 5 else 0)
        out.ok = (out.end_code == 0x00)
    else:
        out.ok = True
    return out


# ---------------------------------------------------------------- 用例

def S1_endurance(link, rounds):
    """连续升级 xN：每轮都完整提交并真的跳转，之后靠唤回关键字回来。"""
    img = real_image()
    bad = 0
    t_all = []
    for i in range(rounds):
        t0 = time.time()
        if not enter_iap(link):
            record('S1', '连续升级 x%d' % rounds, False, '第 %d 轮进不了 IAP' % (i + 1))
            return
        o = upgrade(link, img)
        t_all.append(time.time() - t0)
        if not o.ok:
            bad += 1
            print('    第 %d 轮失败：%s' % (i + 1, o.detail))
        # 跳转后应能收到 APP 的 alive
        time.sleep(1.2)
    record('S1', '连续升级 x%d 全部提交成功' % rounds, bad == 0,
           '失败 %d 轮，平均 %.2f s/轮' % (bad, sum(t_all) / len(t_all) if t_all else 0))


def S2_size_sweep(link, sizes):
    img = real_image()
    bad = []
    info = []
    for kb in sizes:
        size = kb * 1024
        if size < len(img):
            continue
        if not enter_iap(link):
            bad.append('%dK 进不了 IAP' % kb)
            continue
        im = sized_image(size, seed=0x1000 + kb)
        t0 = time.time()
        o = upgrade(link, im)
        dt = time.time() - t0
        info.append('%dK %.1fs' % (kb, dt))
        if not o.ok:
            bad.append('%dK: %s' % (kb, o.detail))
        time.sleep(0.4)
    record('S2', '尺寸扫描 %s 全部完成' % (','.join('%dK' % k for k in sizes)),
           not bad, ('; '.join(info) + (('  失败：' + '; '.join(bad)) if bad else '')))


def S3_corrupt_inject(link):
    """在若干个帧位置上注入「数据被改坏」，验证拒绝 + 恢复 + 最终仍成功。"""
    img = real_image()
    _, _, body, _, _ = image_parts(img)
    nframes = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    picks = sorted(set([0, 1, nframes // 2, nframes - 2, nframes - 1]))
    picks = [p for p in picks if 0 <= p < nframes]
    bad = []
    for idx in picks:
        if not enter_iap(link):
            bad.append('帧 %d：进不了 IAP' % idx)
            continue
        o = upgrade(link, img, corrupt={idx})
        code = o.codes.get(idx)
        # 期望：被改坏的那一帧回 0x08（累积 CRC32 不符），之后能续上并完成
        rejected = (code == 0x08)
        if not (rejected and o.ok):
            bad.append('帧 %d：出现码 %s，完成=%s（%s）'
                       % (idx, CODE_NAME.get(code, code), o.ok, o.detail))
        time.sleep(0.3)
    record('S3', '改坏帧被拒(0x08)且能续传完成（帧 %s）' % picks, not bad,
           '; '.join(bad) if bad else '每帧被拒后重发正确内容都能续上')


def S4_lost_reply(link):
    """主机丢一次应答后重发同一帧 -> 从机必须幂等重放，不得重复写。"""
    img = real_image()
    _, _, body, _, _ = image_parts(img)
    nframes = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    idx = min(1, nframes - 1)
    if not enter_iap(link):
        record('S4', '应答丢失后幂等重发', False, '进不了 IAP')
        return
    o = upgrade(link, img, drop_reply={idx})
    ok = o.ok and o.dropped >= 1
    record('S4', '主机丢应答后重发同一帧仍能完成', ok,
           '丢弃 %d 次应答，完成=%s（%s）' % (o.dropped, o.ok, o.detail))


def S5_replay_clobber(link):
    """★ 探测：成功应答后插一条别的命令，再重发同一帧 —— 重放出来的必须仍是 DATA 应答。

    若从机把「上次应答」当成全局缓冲、被任意一条命令覆盖，这里就会收到
    STATUS 应答（cmd=0x84）而不是 DATA 应答（cmd=0x82）。
    """
    img = real_image()
    _, _, body, _, _ = image_parts(img)
    nframes = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    probe = sorted(set([1, 2, nframes // 2]))
    probe = [p for p in probe if 0 <= p < nframes]
    if not enter_iap(link):
        record('S5', '重放缓冲不被其它命令污染', False, '进不了 IAP')
        return
    o = upgrade(link, img, clobber_after=set(probe))
    wrong = [c for c in o.clobber if c[1] != (CMD_DATA | DIR_REPLY)]
    detail = '; '.join('帧%d->cmd0x%02X code%s' % (i, cm or 0, c) for i, cm, c in o.clobber)
    record('S5', '重放缓冲不被其它命令污染', (not wrong) and o.ok,
           detail + ('  完成=%s' % o.ok))


def S6_gap(link):
    """跳号 -> 0x07 + 期望地址 -> 续传 -> 完成。"""
    img = real_image()
    _, _, body, _, _ = image_parts(img)
    nframes = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    idx = min(1, nframes - 1)
    if not enter_iap(link):
        record('S6', '跳号被拒且给出续传点', False, '进不了 IAP')
        return
    o = upgrade(link, img, skip={idx})
    got = o.gaps[0] if o.gaps else (None, None, 0)
    expect_addr = APP_BASE + 8 + idx * BLOCK_SIZE
    ok = (got[1] == 0x07) and (got[2] == expect_addr) and o.ok
    record('S6', '跳号 -> 0x07 + 期望地址，续传后完成', ok,
           '帧 %d：码 %s 期望 0x%08X（应为 0x%08X），完成=%s'
           % (idx, CODE_NAME.get(got[1], got[1]), got[2], expect_addr, o.ok))


def S7_backward(link):
    """倒退帧（更早的地址）应走幂等重放，不推进状态、不误写。"""
    img = real_image()
    _, _, body, _, _ = image_parts(img)
    nframes = (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE
    if nframes < 3:
        record('S7', '倒退帧幂等', True, '固件太小，跳过')
        return
    if not enter_iap(link):
        record('S7', '倒退帧幂等', False, '进不了 IAP')
        return

    sp, pc, body, crc_all, total_pkts = image_parts(img)
    tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
    if rx(link, 12.0, want=CMD_START | DIR_REPLY) is None:
        record('S7', '倒退帧幂等', False, 'START 无应答')
        return

    # 正常发前两帧
    for off in (0, BLOCK_SIZE):
        _, _, _, _, pl = data_payload(body, off, total_pkts)
        tx(link, CMD_DATA, pl)
        rx(link, 0.6, want=CMD_DATA | DIR_REPLY)
    st1 = status(link)

    # 倒退发第 0 帧
    _, _, _, _, pl0 = data_payload(body, 0, total_pkts)
    tx(link, CMD_DATA, pl0)
    f = rx(link, 0.6, want=CMD_DATA | DIR_REPLY)
    st2 = status(link)

    ok = (st1 is not None and st2 is not None and st1 == st2 and f is not None
          and f['data'][0] == 0x00)
    record('S7', '倒退帧被幂等处理，状态不倒退', ok,
           '发帧前三 %s / 发帧后 %s' % (st1, st2))


def S8_data_before_start(link):
    """未 START 直接 DATA -> 0x06，且不得动 Flash（STATUS 仍报未在接收）。"""
    img = real_image()
    _, _, body, _, total_pkts = image_parts(img)
    if not enter_iap(link):
        record('S8', '未 START 直接 DATA 被拒', False, '进不了 IAP')
        return
    _, _, _, _, pl = data_payload(body, 0, total_pkts)
    tx(link, CMD_DATA, pl)
    f = rx(link, 0.6, want=CMD_DATA | DIR_REPLY)
    code = f['data'][0] if f and f['data'] else None
    st = status(link)
    ok = (code == 0x06) and (st == (0, 0))
    record('S8', '未 START 直接 DATA -> 0x06 且不动 Flash', ok,
           '码 %s，STATUS %s' % (CODE_NAME.get(code, code), st))


def S9_restart_midway(link):
    """会话中途再发 START：应强制重置并接受，随后能完整完成。"""
    img = real_image()
    sp, pc, body, crc_all, total_pkts = image_parts(img)
    if not enter_iap(link):
        record('S9', '中途重启 START', False, '进不了 IAP')
        return

    # 第一次 START + 两帧
    tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
    rx(link, 12.0, want=CMD_START | DIR_REPLY)
    for off in (0, BLOCK_SIZE):
        _, _, _, _, pl = data_payload(body, off, total_pkts)
        tx(link, CMD_DATA, pl)
        rx(link, 0.6, want=CMD_DATA | DIR_REPLY)

    # 第二次 START（中途重启）—— 应该成功，不是 0x06
    tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
    f = rx(link, 12.0, want=CMD_START | DIR_REPLY)
    mid_ok = f is not None and f['data'][0] == 0x00

    # 走完剩下的
    offset = 0
    done = False
    while offset < len(body):
        _, _, _, _, pl = data_payload(body, offset, total_pkts)
        tx(link, CMD_DATA, pl)
        g = rx(link, 0.6, want=CMD_DATA | DIR_REPLY)
        if g is None or g['data'][0] != 0x00:
            break
        offset = struct.unpack('<I', g['data'][5:9])[0] - (APP_BASE + 8)
        if offset >= len(body):
            done = True
            break
    tx(link, CMD_END)
    e = rx(link, 6.0, want=CMD_END | DIR_REPLY)
    end_ok = e is not None and e['data'][0] == 0x00

    record('S9', '中途重启 START 被接受且最终完成', mid_ok and done and end_ok,
           '重启应答=%s 数据完成=%s END=%s' % (mid_ok, done, end_ok))
    time.sleep(0.5)


def S10_back_to_back(link, rounds):
    """背靠背会话：START -> 3 帧 -> 放弃（不发 END），重复 N 次，最后完整做一次。"""
    img = real_image()
    sp, pc, body, crc_all, total_pkts = image_parts(img)
    bad = 0
    for r in range(rounds):
        if not enter_iap(link):
            bad += 1
            continue
        tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
        f = rx(link, 12.0, want=CMD_START | DIR_REPLY)
        if f is None or f['data'][0] != 0x00:
            bad += 1
            continue
        for k in range(3):
            off = k * BLOCK_SIZE
            if off >= len(body):
                break
            _, _, _, _, pl = data_payload(body, off, total_pkts)
            tx(link, CMD_DATA, pl)
            rx(link, 0.6, want=CMD_DATA | DIR_REPLY)
        # 故意不发 END，直接下一轮（不必复位：下一轮 START 会强制重置）
    # 最后完整做一次
    o = upgrade(link, img)
    record('S10', '背靠背会话 x%d 后仍能完整升级' % rounds, bad == 0 and o.ok,
           '异常轮 %d，最终完成=%s（%s）' % (bad, o.ok, o.detail))


def S11_burst(link, burst):
    """不等应答一次灌 N 帧，量一下从机能收下多少。

    环形缓冲只有 512 B，而一帧最大 533 B —— 一旦主机流水线发送，
    从机在写 Flash 期间来不及搬字节，必然丢。这里量的是「停等模型下的余量」。
    """
    img = real_image()
    sp, pc, body, crc_all, total_pkts = image_parts(img)
    if not enter_iap(link):
        record('S11', '突发帧', False, '进不了 IAP')
        return
    tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
    if rx(link, 12.0, want=CMD_START | DIR_REPLY) is None:
        record('S11', '突发帧', False, 'START 无应答')
        return

    blobs = []
    for k in range(burst):
        off = k * BLOCK_SIZE
        if off >= len(body):
            break
        _, _, _, _, pl = data_payload(body, off, total_pkts)
        blobs.append(encode(DEV, CMD_DATA, pl))

    t0 = time.time()
    link.ser.reset_input_buffer()
    link.parser.reset()
    link.ser.write(b''.join(blobs))
    link.ser.flush()

    got = 0
    offered = len(blobs)
    while time.time() - t0 < 1.5:
        b = link.ser.read(1)
        if not b:
            if got >= offered:
                break
            continue
        f = link.parser.feed(b[0])
        if f and f['reply'] and f['id'] == DEV and f['cmd'] == (CMD_DATA | DIR_REPLY):
            got += 1
            if got >= offered:
                break
    # 期望：offered 帧全部被处理（停等余量足够）；掉帧也不算致命，靠重传补
    record('S11', '突发 %d 帧一次灌入' % offered, got == offered,
           '收到 %d/%d 个数据应答（环形缓冲 512B < 一帧 533B，掉帧靠重传补）'
           % (got, offered))


def S12_idle_timeout(link):
    """RECEIVING 空闲超时（BL_IDLE_TIMEOUT_MS = 30s）：应回到 Idle。"""
    img = real_image()
    _, _, body, _, total_pkts = image_parts(img)
    sp, pc, body, crc_all, total_pkts = image_parts(img)
    if not enter_iap(link):
        record('S12', '空闲超时回到 Idle', False, '进不了 IAP')
        return
    tx(link, CMD_START, struct.pack('<IIII', len(img), crc_all, sp, pc))
    if rx(link, 12.0, want=CMD_START | DIR_REPLY) is None:
        record('S12', '空闲超时回到 Idle', False, 'START 无应答')
        return
    _, _, _, _, pl = data_payload(body, 0, total_pkts)
    tx(link, CMD_DATA, pl)
    rx(link, 0.6, want=CMD_DATA | DIR_REPLY)
    st1 = status(link)

    print('    等待 35 秒（空闲超时 30 秒）...')
    time.sleep(35.0)
    st2 = status(link)
    ok = (st1 is not None and st1[0] != 0) and (st2 == (0, 0))
    record('S12', '空闲 35s 后回到 Idle（STATUS 归零）', ok,
           '超时前 %s / 超时后 %s' % (st1, st2))


# ---------------------------------------------------------------- main

def main():
    global PORT, BAUD, DEV

    ap = argparse.ArgumentParser(description='IAP 全链路压测（真板）')
    ap.add_argument('--port', default=PORT)
    ap.add_argument('--baud', type=int, default=BAUD)
    ap.add_argument('--id', type=int, default=DEV)
    ap.add_argument('--rounds', type=int, default=12, help='S1 连续升级轮数')
    ap.add_argument('--sizes', default='2,8,64,200', help='S2 尺寸（KB）')
    ap.add_argument('--burst', type=int, default=3, help='S11 一次灌几帧')
    ap.add_argument('--slow', action='store_true', help='额外跑 S12（约 40 秒）')
    ap.add_argument('--only', default='', help='只跑指定用例，如 S1,S3,S5')
    ap.add_argument('--skip-restore', action='store_true', help='结束不刷回真固件')
    args = ap.parse_args()

    PORT, BAUD, DEV = args.port, args.baud, args.id

    want = set(x.strip().upper() for x in args.only.split(',') if x.strip())

    def want_run(tag):
        return (not want) or (tag in want)

    sizes = [int(x) for x in args.sizes.split(',') if x.strip()]

    link = Link(args.port, args.baud, args.id, verbose=False)
    print('=== IAP 全链路压测（%s @%d，设备 ID %d）===' % (args.port, args.baud, args.id))
    print('真固件 %s：%d 字节' % (os.path.relpath(REAL_BIN, ROOT), os.path.getsize(REAL_BIN)))
    print()

    t0 = time.time()
    try:
        # 冒烟：先确认通路
        ok, how = try_enter_iap(link)
        if not ok:
            print('★ 无法进入 IAP —— 按住 PC0 上电，或确认 APP 在跑（会自动发唤回关键字）')
            return 2
        print('  冒烟：设备在 IAP（%s），STATUS=%s\n' % (how, status(link)))

        if want_run('S1'):
            S1_endurance(link, args.rounds)
        if want_run('S2'):
            S2_size_sweep(link, sizes)
        if want_run('S3'):
            S3_corrupt_inject(link)
        if want_run('S4'):
            S4_lost_reply(link)
        if want_run('S5'):
            S5_replay_clobber(link)
        if want_run('S6'):
            S6_gap(link)
        # 顺序有讲究：S8 要求从机处于 Idle（STATUS 归零），
        # 而 S7 会把从机留在 Receiving 态 —— 所以 S8 必须在 S7 之前。
        if want_run('S8'):
            S8_data_before_start(link)
        if want_run('S7'):
            S7_backward(link)
        if want_run('S9'):
            S9_restart_midway(link)
        if want_run('S10'):
            S10_back_to_back(link, args.rounds)
        if want_run('S11'):
            S11_burst(link, args.burst)
        if args.slow and want_run('S12'):
            S12_idle_timeout(link)

        # 收尾：刷回真固件，保证板子可用
        if not args.skip_restore:
            print()
            print('  收尾：刷回真固件 ...')
            if enter_iap(link):
                o = upgrade(link, real_image())
                print('  收尾结果：%s' % ('OK' if o.ok else ('失败 ' + o.detail)))
            else:
                print('  ★ 收尾失败：进不了 IAP')
    finally:
        link.close()

    print()
    print('=== 汇总（用时 %.1f s）===' % (time.time() - t0))
    passed = sum(1 for r in RESULTS if r[2])
    for tag, name, ok, detail in RESULTS:
        print('  %-4s %-4s %s' % (tag, 'PASS' if ok else 'FAIL', name))
    print('  %d/%d 通过' % (passed, len(RESULTS)))
    return 0 if passed == len(RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
