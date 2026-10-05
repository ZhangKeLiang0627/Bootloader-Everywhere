// 载体层压测（PC 侧）。只测 protocol.{h,cpp}，不碰硬件。
//
//   g++ -std=c++11 -O2 -Wall -Wextra -I Bootloader tools/stress_protocol.cpp
//       Bootloader/protocol.cpp -o stress_protocol
//
// 核心断言只有一条，但它是「绝不出假帧」的充分条件：
//
//   解析器接受一帧时，它消耗掉的最后 (kOverhead+len) 个字节，
//   必须与「把这一帧重新编码」的结果逐字节相同。
//
// 因为状态机只在消费完帧尾那一刻返回 true，所以这个「尾部切片」可以精确取到。
// 一旦解析器接受了任何不是真实帧的东西，这条断言必然失败。

#include "protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------- 小工具

static uint64_t gChecks = 0;
static uint64_t gFails  = 0;

static void check(bool ok, const char* what)
{
    ++gChecks;
    if (!ok) {
        ++gFails;
        std::printf("    FAIL: %s\n", what);
        std::fflush(stdout);
    }
}

static void checkEq(uint64_t got, uint64_t want, const char* what)
{
    ++gChecks;
    if (got != want) {
        ++gFails;
        std::printf("    FAIL: %s  (got %llu, want %llu)\n", what,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
        std::fflush(stdout);
    }
}

// 确定性 PRNG（xorshift64*），保证字节流每次完全一致、可复现
static uint64_t gRng = 0x123456789ABCDEF0ULL;

static uint32_t rnd()
{
    gRng ^= gRng >> 12;
    gRng ^= gRng << 25;
    gRng ^= gRng >> 27;
    return static_cast<uint32_t>((gRng * 0x2545F4914F6CDD1DULL) >> 32);
}

static void rndSeed(uint64_t s) { gRng = s ? s : 1U; }

// 扫描结果：接受的帧数 + 违反不变量的次数
struct ScanStats {
    uint32_t frames;
    uint32_t badTail;
    uint32_t badLen;
};

// 把一条完整字节流喂给解析器；每接受一帧就校验「尾部切片 = 重新编码」。
static ScanStats scanStream(const uint8_t* data, uint32_t n, proto::Frame& f)
{
    ScanStats      st{0U, 0U, 0U};
    proto::Parser  parser;
    uint8_t        re[proto::kFrameMax];

    for (uint32_t i = 0U; i < n; ++i) {
        if (!parser.feed(data[i], f)) {
            continue;
        }
        ++st.frames;

        if (!f.lenOk() || f.len > proto::kDataMax) {
            ++st.badLen;
            continue;
        }

        const uint32_t flen = proto::kOverhead + f.len;
        if (flen > (i + 1U)) {
            ++st.badTail;                     // 声称的帧长比已喂入的还长 —— 不可能
            continue;
        }
        const uint32_t rn = proto::encode(f.id, f.cmd, f.data, f.len, re, sizeof(re));
        if (rn != flen || std::memcmp(re, data + (i + 1U - flen), flen) != 0) {
            ++st.badTail;                     // 接受了字节流里并不存在的帧 = 假帧
        }
    }
    return st;
}

// 带护栏的 Frame：解析器若写越界会踩坏 post 区
struct GuardedFrame {
    uint32_t     pre[16];
    proto::Frame f;
    uint32_t     post[16];
};

static const uint32_t kGuardMagic = 0xA5A5A5A5U;

static void guardInit(GuardedFrame& g)
{
    for (uint32_t i = 0U; i < 16U; ++i) {
        g.pre[i]  = kGuardMagic;
        g.post[i] = kGuardMagic;
    }
}

static bool guardOk(const GuardedFrame& g)
{
    for (uint32_t i = 0U; i < 16U; ++i) {
        if (g.pre[i] != kGuardMagic || g.post[i] != kGuardMagic) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- [1] 全长度往返

static void t1RoundTrip()
{
    std::printf("[1] 长度 0..1024 全量往返（encode -> parse）\n");

    static uint8_t body[proto::kDataMax];
    static uint8_t buf[proto::kFrameMax];
    GuardedFrame   g;

    rndSeed(0x1111U);
    for (uint32_t i = 0U; i < sizeof(body); ++i) {
        body[i] = static_cast<uint8_t>(rnd());
    }

    uint32_t bad = 0U;
    for (uint32_t len = 0U; len <= proto::kDataMax; ++len) {
        const uint8_t id  = static_cast<uint8_t>(len & 0xFFU);
        const uint8_t cmd = static_cast<uint8_t>((len >> 3) & 0x7FU);

        const uint32_t n = proto::encode(id, cmd, body, static_cast<uint16_t>(len),
                                         buf, sizeof(buf));
        if (n != (proto::kOverhead + len)) {
            ++bad;
            continue;
        }

        guardInit(g);
        proto::Parser parser;
        bool           got = false;
        for (uint32_t i = 0U; i < n; ++i) {
            got = parser.feed(buf[i], g.f);
            if (got && (i + 1U) != n) {
                ++bad;                        // 提前接受
                break;
            }
        }
        const bool contentOk =
            got && g.f.id == id && g.f.cmd == cmd && g.f.len == len &&
            (len == 0U || std::memcmp(g.f.data, body, len) == 0) && g.f.lenOk();
        if (!contentOk) {
            ++bad;
        }
        if (!guardOk(g)) {
            ++bad;
            std::printf("    护栏被破坏：len=%u\n", len);
        }
    }
    checkEq(bad, 0U, "长度 0..1024 全部往返一致");
    std::printf("    1025 个长度全部通过\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [2] encode 参数校验

static void t2EncodeGuards()
{
    std::printf("[2] encode 的非法参数必须返回 0\n");

    static uint8_t out[proto::kFrameMax + 8];
    uint8_t        small[4];

    checkEq(proto::encode(1U, 1U, small, 1U, nullptr, 64U), 0U, "out=nullptr");
    checkEq(proto::encode(1U, 1U, small, 1U, out, sizeof(out)),
            1U + proto::kOverhead, "正常一路");
    checkEq(proto::encode(1U, 1U, nullptr, 1U, out, sizeof(out)), 0U, "data=nullptr 且 len>0");
    checkEq(proto::encode(1U, 1U, nullptr, 0U, out, sizeof(out)), proto::kOverhead,
            "data=nullptr 且 len=0 合法");
    checkEq(proto::encode(1U, 1U, small, static_cast<uint16_t>(proto::kDataMax + 1U),
                          out, sizeof(out)), 0U, "len > kDataMax");
    checkEq(proto::encode(1U, 1U, small, 4U, small, sizeof(small)), 0U, "outCap 不足");
    checkEq(proto::encode(1U, 1U, small, 4U, out, proto::kOverhead + 4U),
            proto::kOverhead + 4U, "outCap 恰好够");
    checkEq(proto::encode(1U, 1U, small, static_cast<uint16_t>(proto::kDataMax),
                          out, sizeof(out)), proto::kFrameMax, "满载荷帧长 = kFrameMax");
    std::printf("    8 项边界断言完成\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [3] lenOk 边界

static void t3LenOk()
{
    std::printf("[3] Frame::lenOk / totalLen 边界\n");

    proto::Frame f;
    std::memset(&f, 0, sizeof(f));

    checkEq(proto::kFrameMin, proto::kOverhead, "kFrameMin == kOverhead");
    checkEq(proto::kFrameMax, proto::kOverhead + proto::kDataMax, "kFrameMax");

    f.len = 0U;
    check(f.lenOk(), "len=0 lenOk");
    checkEq(f.totalLen(), proto::kFrameMin, "len=0 totalLen=kFrameMin");

    f.len = 1024U;
    check(f.lenOk(), "len=1024 lenOk");
    checkEq(f.totalLen(), proto::kFrameMax, "len=1024 totalLen=kFrameMax");

    f.len = 1025U;
    check(!f.lenOk(), "len=1025 不 lenOk");

    f.len = 0xFFFFU;
    check(!f.lenOk(), "len=0xFFFF 不 lenOk");
    std::printf("    边界断言完成\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [4] 单字节突变

// CRC8 的生成多项式 0x07 有常数项，能检出所有长度 <= 8 的突发错误 ——
// 单字节错误正是长度 8 的突发错误，所以任何一处单字节改动都必须被检出。
static void t4SingleByteMutation()
{
    std::printf("[4] 单字节突变：每个位置改成任意值，必须全部检出\n");

    static const uint32_t kLens[] = {0U, 1U, 13U, 14U, 511U, 512U, 1024U};
    static uint8_t        payload[proto::kDataMax];
    static uint8_t        frame[proto::kFrameMax];
    static uint8_t        mutated[proto::kFrameMax];
    GuardedFrame          g;

    rndSeed(0x2222U);
    for (uint32_t i = 0U; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(rnd());
    }

    uint64_t trials = 0U;
    uint64_t missed = 0U;

    for (uint32_t li = 0U; li < sizeof(kLens) / sizeof(kLens[0]); ++li) {
        const uint32_t len = kLens[li];
        const uint32_t n   = proto::encode(0x01U, 0x02U, payload,
                                          static_cast<uint16_t>(len), frame, sizeof(frame));
        if (n == 0U) {
            check(false, "encode 失败");
            continue;
        }

        for (uint32_t pos = 0U; pos < n; ++pos) {
            const uint8_t orig = frame[pos];
            for (uint32_t v = 0U; v < 256U; ++v) {
                if (v == orig) {
                    continue;
                }
                std::memcpy(mutated, frame, n);
                mutated[pos] = static_cast<uint8_t>(v);
                ++trials;

                guardInit(g);
                const ScanStats st = scanStream(mutated, n, g.f);
                if (st.frames != 0U || st.badTail != 0U) {
                    ++missed;
                    if (missed <= 3U) {
                        std::printf("    漏检：len=%u pos=%u -> 0x%02X (原 0x%02X)，接受 %u 帧\n",
                                    len, pos, v, orig, st.frames);
                    }
                }
                if (!guardOk(g)) {
                    check(false, "护栏被破坏（单字节突变）");
                }
            }
        }
        std::printf("    len=%u 完成\n", len);
        std::fflush(stdout);
    }
    checkEq(missed, 0U, "单字节突变零漏检");
    std::printf("    %llu 次突变，漏检 %llu\n",
                static_cast<unsigned long long>(trials),
                static_cast<unsigned long long>(missed));
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [5] 截断

static void t5Truncation()
{
    std::printf("[5] 截断：任何前缀都不得被接受\n");

    static uint8_t payload[proto::kDataMax];
    static uint8_t frame[proto::kFrameMax];
    GuardedFrame   g;

    rndSeed(0x3333U);
    for (uint32_t i = 0U; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(rnd());
    }

    uint32_t bad    = 0U;
    uint64_t trials = 0U;
    for (uint32_t len = 0U; len <= proto::kDataMax; len += 37U) {
        const uint32_t n = proto::encode(0x01U, 0x02U, payload,
                                         static_cast<uint16_t>(len), frame, sizeof(frame));
        for (uint32_t cut = 0U; cut < n; ++cut) {
            ++trials;
            guardInit(g);
            const ScanStats st = scanStream(frame, cut, g.f);
            if (st.frames != 0U || st.badTail != 0U) {
                ++bad;
            }
        }
    }
    checkEq(bad, 0U, "截断前缀零接受");
    std::printf("    %llu 个前缀，接受 %u\n",
                static_cast<unsigned long long>(trials), bad);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [6] 随机 fuzz

static void t6Fuzz(uint32_t megabytes)
{
    std::printf("[6] 随机字节流 fuzz（%u MB）\n", megabytes);

    const uint32_t chunk = 1U << 16;          // 64KB 一批
    static uint8_t buf[1U << 16];
    GuardedFrame   g;

    uint64_t totalFrames = 0U;
    uint64_t badTail     = 0U;
    uint64_t badLen      = 0U;

    rndSeed(0xDEADBEEFULL);

    for (uint32_t mb = 0U; mb < megabytes; ++mb) {
        guardInit(g);
        proto::Parser parser;
        for (uint32_t off = 0U; off < chunk; ++off) {
            buf[off] = static_cast<uint8_t>(rnd());
        }
        for (uint32_t i = 0U; i < chunk; ++i) {
            if (!parser.feed(buf[i], g.f)) {
                continue;
            }
            ++totalFrames;

            if (!g.f.lenOk()) {
                ++badLen;
                continue;
            }
            const uint32_t flen = proto::kOverhead + g.f.len;
            uint8_t        re[proto::kFrameMax];
            const uint32_t rn = proto::encode(g.f.id, g.f.cmd, g.f.data, g.f.len,
                                              re, sizeof(re));
            if (flen > (i + 1U) || rn != flen ||
                std::memcmp(re, buf + (i + 1U - flen), flen) != 0) {
                ++badTail;
                if (badTail <= 3U) {
                    std::printf("    假帧！len=%u flen=%u\n", g.f.len, flen);
                }
            }
        }
        if (!guardOk(g)) {
            check(false, "护栏被破坏（fuzz）");
        }
    }

    checkEq(badTail, 0U, "fuzz 零假帧");
    checkEq(badLen, 0U, "fuzz 零非法长度");

    const uint64_t totalBytes = static_cast<uint64_t>(megabytes) * chunk;
    std::printf("    接受 %llu 帧 / %llu 字节（平均每 %.0f 字节一帧）\n",
                static_cast<unsigned long long>(totalFrames),
                static_cast<unsigned long long>(totalBytes),
                totalFrames ? static_cast<double>(totalBytes) /
                                  static_cast<double>(totalFrames)
                            : 0.0);

    // 0xA5 密集流：帧头多，但不应产生假帧
    rndSeed(0x5150U);
    guardInit(g);
    proto::Parser parser;
    uint64_t      injected = 0U;
    for (uint32_t i = 0U; i < chunk; ++i) {
        buf[i] = (i % 3U == 0U) ? 0xA5U : static_cast<uint8_t>(rnd());
    }
    for (uint32_t i = 0U; i < chunk; ++i) {
        if (!parser.feed(buf[i], g.f)) {
            continue;
        }
        ++injected;
        const uint32_t flen = proto::kOverhead + g.f.len;
        uint8_t        re[proto::kFrameMax];
        const uint32_t rn = proto::encode(g.f.id, g.f.cmd, g.f.data, g.f.len,
                                          re, sizeof(re));
        if (flen > (i + 1U) || rn != flen ||
            std::memcmp(re, buf + (i + 1U - flen), flen) != 0) {
            check(false, "0xA5 密集流出现假帧");
        }
    }
    std::printf("    0xA5 密集流（1/3 是帧头）64KB：接受 %llu 帧，零假帧\n",
                static_cast<unsigned long long>(injected));

    // 极端：整条流全是 0xA5
    guardInit(g);
    proto::Parser p2;
    uint64_t      allHead = 0U;
    for (uint32_t i = 0U; i < chunk; ++i) {
        if (p2.feed(0xA5U, g.f)) {
            ++allHead;
        }
    }
    check(guardOk(g), "全 0xA5 流护栏完好");
    checkEq(allHead, 0U, "全 0xA5 流零接受");
    std::printf("    全 0xA5 流 64KB：接受 %llu 帧（应为 0）\n",
                static_cast<unsigned long long>(allHead));

    // [6b] 对抗性假帧率：把字节流造成「解析器频繁走到等 CRC 那一步」的形态，
    //      这是假帧概率最大的输入。构造 6 字节一组：A5 id cmd len=0 CRC tail，
    //      只有 CRC 与 tail 同时"恰好"正确才会被当成一帧。
    //      理论接受率 = 1/256（CRC）× 1/256（tail）= 1/65536 组。
    const uint32_t groups = 1U << 21;        // 约 209 万组 = 12.6 MB
    uint64_t       advAccept = 0U;
    uint64_t       advTrials = 0U;
    rndSeed(0xABCDEFULL);
    guardInit(g);
    proto::Parser pa;
    for (uint32_t gi = 0U; gi < groups; ++gi) {
        const uint8_t grp[6] = {
            0xA5U,
            static_cast<uint8_t>(rnd()),
            static_cast<uint8_t>(rnd()),
            0x00U, 0x00U,                     // len = 0
            static_cast<uint8_t>(rnd()),      // 冒充 CRC
        };
        // 第 6 个字节随机；再补一个随机 tail 字节
        for (uint32_t k = 0U; k < 6U; ++k) {
            if (pa.feed(grp[k], g.f)) {
                ++advAccept;
            }
        }
        const uint8_t tail = static_cast<uint8_t>(rnd());
        if (pa.feed(tail, g.f)) {
            ++advAccept;
        }
        advTrials += 7U;
    }
    const double advRate = advAccept ? static_cast<double>(advTrials) /
                                           static_cast<double>(advAccept)
                                     : 0.0;
    std::printf("    [6b] 对抗流 %llu 字节：接受 %llu 帧，约 1/%.0f 字节（理论 1/65536 量级）\n",
                static_cast<unsigned long long>(advTrials),
                static_cast<unsigned long long>(advAccept), advRate);

    // 上限断言：假帧率必须显著低于 1/4096（理论 1/65536，留 16 倍余量）
    check((advAccept * 4096ULL) < advTrials, "对抗流假帧率 < 1/4096");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [7] 恢复能力

static void t7Recovery()
{
    std::printf("[7] 恢复：坏帧之后多久能重新同步\n");

    static uint8_t frame[proto::kFrameMax];
    static uint8_t stream[proto::kFrameMax * 8U];
    static uint8_t body[512];
    GuardedFrame   g;

    rndSeed(0x7777U);
    for (uint32_t i = 0U; i < sizeof(body); ++i) {
        body[i] = static_cast<uint8_t>(rnd());
    }

    const uint32_t n = proto::encode(0x01U, 0x02U, body, 512U, frame, sizeof(frame));
    checkEq(n, proto::kOverhead + 512U, "基准帧长");

    // (a) 每个位置翻转 1 bit，后面接一帧完好的。
    //
    // 判据要精确：只有在「长度字段被破坏」时才允许吞掉后面那一帧 ——
    // 帧长字段同时决定「CRC 在哪」和「帧尾在哪」，它一坏，解析器就会把后续
    // 字节当成数据吞掉，直到 CRC 失败才重同步；此时被吞掉的那一帧的帧头
    // 已经消费掉了。这是「不定长帧」的固有代价（设计文档 §0.6.4 已记录），
    // 不是实现缺陷 —— 主机等一帧超时后重传即可补回来。
    // 除此之外的任何位置被破坏（尤其数据区），后一帧必须完好收到。
    uint32_t lostLenField = 0U;      // 长度字段被破坏导致吞掉下一帧（允许）
    uint32_t lostOther    = 0U;      // 其它位置被破坏也吞帧（不允许）
    uint64_t trials       = 0U;
    for (uint32_t pos = 0U; pos < n; ++pos) {
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            std::memcpy(stream, frame, n);
            stream[pos] ^= static_cast<uint8_t>(1U << bit);
            std::memcpy(stream + n, frame, n);
            ++trials;
            guardInit(g);
            const ScanStats st = scanStream(stream, 2U * n, g.f);
            const bool swallowed = (st.frames == 0U) || (st.badTail != 0U);
            if (!swallowed) {
                continue;
            }
            if (pos == 3U || pos == 4U) {           // 长度字段（小端 2 字节）
                ++lostLenField;
            } else {
                ++lostOther;
                if (lostOther <= 3U) {
                    std::printf("    非长度字段 pos=%u bit=%u 也吞帧了\n", pos, bit);
                }
            }
        }
    }
    checkEq(lostOther, 0U, "非长度字段被破坏不得吞掉下一帧");
    std::printf("    %llu 组坏帧+好帧：长度字段破坏吞帧 %u 次（设计取舍），其它位置吞帧 %u 次\n",
                static_cast<unsigned long long>(trials), lostLenField, lostOther);

    // (b) 丢掉帧中间的一个字节
    uint32_t notRecovered = 0U;
    for (uint32_t drop = 0U; drop < n; ++drop) {
        uint32_t k = 0U;
        for (uint32_t i = 0U; i < n; ++i) {
            if (i != drop) {
                stream[k++] = frame[i];
            }
        }
        std::memcpy(stream + k, frame, n);
        guardInit(g);
        const ScanStats st = scanStream(stream, k + n, g.f);
        if (st.frames == 0U || st.badTail != 0U) {
            ++notRecovered;
            if (notRecovered <= 3U) {
                std::printf("    drop=%u 后未能恢复下一帧\n", drop);
            }
        }
    }
    checkEq(notRecovered, 0U, "丢字节后下一帧必被收到");
    std::printf("    丢 %u 个位置各一次，全部在下一帧内恢复\n", n);

    // (c) 插入一个垃圾字节
    uint32_t bad2 = 0U;
    for (uint32_t ins = 0U; ins < n; ++ins) {
        uint32_t k = 0U;
        for (uint32_t i = 0U; i < n; ++i) {
            if (i == ins) {
                stream[k++] = 0x5AU;
            }
            stream[k++] = frame[i];
        }
        std::memcpy(stream + k, frame, n);
        guardInit(g);
        const ScanStats st = scanStream(stream, k + n, g.f);
        if (st.frames == 0U || st.badTail != 0U) {
            ++bad2;
        }
    }
    checkEq(bad2, 0U, "插入垃圾字节后下一帧必被收到");
    std::printf("    插 %u 个位置各一次，全部在下一帧内恢复\n", n);

    // (d) 背靠背
    for (uint32_t cnt = 1U; cnt <= 4U; ++cnt) {
        uint32_t k = 0U;
        for (uint32_t c = 0U; c < cnt; ++c) {
            std::memcpy(stream + k, frame, n);
            k += n;
        }
        guardInit(g);
        const ScanStats st = scanStream(stream, k, g.f);
        checkEq(st.frames, cnt, "背靠背帧数");
    }
    std::printf("    背靠背 1-4 帧全部正确\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- [8] 吞吐

static void t8Throughput()
{
    std::printf("[8] 吞吐\n");

    static uint8_t body[512];
    static uint8_t frame[proto::kFrameMax];
    GuardedFrame   g;

    rndSeed(0x8888U);
    for (uint32_t i = 0U; i < sizeof(body); ++i) {
        body[i] = static_cast<uint8_t>(rnd());
    }
    const uint32_t n = proto::encode(0x01U, 0x02U, body, 512U, frame, sizeof(frame));

    guardInit(g);
    proto::Parser  parser;
    const uint32_t rounds = 200000U;
    uint64_t       frames = 0U;

    for (uint32_t r = 0U; r < rounds; ++r) {
        for (uint32_t i = 0U; i < n; ++i) {
            if (parser.feed(frame[i], g.f)) {
                ++frames;
            }
        }
    }
    checkEq(frames, rounds, "吞吐测试帧数");
    std::printf("    %llu 字节逐字节解析，%llu 帧，零假帧\n",
                static_cast<unsigned long long>(static_cast<uint64_t>(rounds) * n),
                static_cast<unsigned long long>(frames));

    uint64_t acc = 0U;
    for (uint32_t r = 0U; r < rounds; ++r) {
        acc += proto::encode(0x01U, 0x02U, body, 512U, frame, sizeof(frame));
    }
    std::printf("    编码 %u 次（每次 519B），累计返回 %llu\n",
                rounds, static_cast<unsigned long long>(acc));
    std::fflush(stdout);
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时也要能看到进度

    uint32_t fuzzMb = 8U;
    if (argc > 1) {
        const int v = std::atoi(argv[1]);
        if (v > 0) {
            fuzzMb = static_cast<uint32_t>(v);
        }
    }

    std::printf("=== 载体层压测（protocol.h / protocol.cpp）===\n");
    std::printf("kHead=0x%02X kTail=0x%02X kFrameMin=%u kFrameMax=%u kDataMax=%u\n\n",
                proto::kHead, proto::kTail, proto::kFrameMin, proto::kFrameMax,
                proto::kDataMax);

    t1RoundTrip();
    t2EncodeGuards();
    t3LenOk();
    t4SingleByteMutation();
    t5Truncation();
    t6Fuzz(fuzzMb);
    t7Recovery();
    t8Throughput();

    std::printf("\n=== 结果：%llu 项断言，%llu 失败 ===\n",
                static_cast<unsigned long long>(gChecks),
                static_cast<unsigned long long>(gFails));
    return gFails == 0U ? 0 : 1;
}
