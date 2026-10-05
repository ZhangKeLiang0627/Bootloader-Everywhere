// 载体层 PC 侧单测。与固件里的 frameSelftest() 共用同一组向量：
// 两端都过，才算「CRC8 变体与字节序两端一致」（设计文档 §0.6.6）。
//
// 编译运行：
//   g++ -std=c++11 -Wall -I Bootloader tools/test_protocol.cpp Bootloader/protocol.cpp -o t && ./t

#include "protocol.h"

#include <stdio.h>
#include <string.h>

static int gFail = 0;
static int gCase = 0;

static void check(bool ok, const char* what)
{
    ++gCase;
    if (!ok) {
        ++gFail;
        printf("  ✘ %s\n", what);
    }
}

static void checkEq(uint32_t got, uint32_t want, const char* what)
{
    ++gCase;
    if (got != want) {
        ++gFail;
        printf("  ✘ %s: got 0x%08X want 0x%08X\n", what, got, want);
    }
}

// 参考用 CRC32 / ISO-HDLC（逐位，LSB-first），用来独立复算文档里的期望值
static uint32_t crc32Ref(const uint8_t* p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFU;
    while (n-- > 0U) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i) {
            c = (c & 1U) ? ((c >> 1) ^ 0xEDB88320U) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFU;
}

static void testCrc8(void)
{
    printf("[1] CRC8 (SMBUS) 向量\n");
    const uint8_t v1[] = {0x01, 0x02, 0x02, 0x00, 0x34, 0x12};
    const uint8_t v2[] = {0x00};
    const uint8_t v3[] = {0xA5, 0x03};
    const uint8_t v4[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    const char*   v5   = "123456789";
    uint8_t       v6[256];
    for (uint32_t i = 0; i < 256U; ++i) {
        v6[i] = static_cast<uint8_t>(i);
    }

    checkEq(proto::crc8(nullptr, 0U), 0x00U, "空输入");
    checkEq(proto::crc8(v1, sizeof(v1)), 0x12U, "示例帧载荷 01 02 02 00 34 12");
    checkEq(proto::crc8(v2, sizeof(v2)), 0x00U, "单个 0x00");
    checkEq(proto::crc8(v3, sizeof(v3)), 0x50U, "A5 03");
    checkEq(proto::crc8(v6, sizeof(v6)), 0x14U, "0x00-0xFF 全字节");
    checkEq(proto::crc8(v5, 9U), 0xF4U, "\"123456789\"");
    // 增量与一次性必须一致
    uint8_t inc = 0U;
    for (uint32_t i = 0; i < sizeof(v1); ++i) {
        inc = proto::crc8Step(inc, v1[i]);
    }
    checkEq(inc, 0x12U, "增量 crc8Step 与一次性 crc8 一致");
    (void)v4;
}

static void testCrc32Vectors(void)
{
    printf("[2] CRC32 (ISO-HDLC) 文档向量复核\n");
    const char* s = "123456789";
    checkEq(crc32Ref(reinterpret_cast<const uint8_t*>(s), 9U), 0xCBF43926U, "\"123456789\"");
    checkEq(crc32Ref(nullptr, 0U), 0x00000000U, "空输入");
    const uint8_t t[] = {0x01, 0x02, 0x03};
    checkEq(crc32Ref(t, sizeof(t)), 0x55BC801DU, "01 02 03");
}

static void testEncode(void)
{
    printf("[3] 组帧\n");
    const uint8_t want[] = {0xA5, 0x01, 0x02, 0x02, 0x00, 0x34, 0x12, 0x12, 0x03};
    const uint8_t payload[2] = {0x34, 0x12};
    uint8_t       out[32];

    const uint32_t n = proto::encode(0x01U, 0x02U, payload, 2U, out, sizeof(out));
    checkEq(n, sizeof(want), "帧长");
    check(n == sizeof(want) && memcmp(out, want, n) == 0, "字节序列 A5 01 02 02 00 34 12 12 03");

    // 空载荷帧
    const uint32_t m = proto::encode(0x01U, 0x04U, nullptr, 0U, out, sizeof(out));
    checkEq(m, proto::kOverhead, "空载荷帧长 = 7");
    check(out[0] == 0xA5U && out[1] == 0x01U && out[2] == 0x04U &&
          out[3] == 0x00U && out[4] == 0x00U && out[6] == 0x03U, "空载荷帧结构");
    checkEq(out[5], proto::crc8(&out[1], 4U), "空载荷帧 CRC8 覆盖 [ID..len]");

    // 容量不足必须拒绝
    checkEq(proto::encode(0x01U, 0x02U, payload, 2U, out, 8U), 0U, "容量不足返回 0");
    // 超长必须拒绝
    uint8_t big[8];
    checkEq(proto::encode(0x01U, 0x02U, big, 2000U, out, sizeof(out)), 0U, "len>1024 返回 0");
}

static bool feedAll(proto::Parser& p, const uint8_t* b, uint32_t n, proto::Frame& f)
{
    bool got = false;
    for (uint32_t i = 0; i < n; ++i) {
        got = p.feed(b[i], f);
    }
    return got;
}

static void testParser(void)
{
    printf("[4] 解析\n");
    static proto::Frame f;
    uint8_t  buf[proto::kFrameMax];
    const uint8_t payload[2] = {0x34, 0x12};

    // 正常往返
    uint32_t n = proto::encode(0x01U, 0x02U, payload, 2U, buf, sizeof(buf));
    proto::Parser p;
    p.reset();
    check(feedAll(p, buf, n, f), "整帧喂入可解析");
    checkEq(f.id, 0x01U, "id");
    checkEq(f.code(), 0x02U, "cmd");
    checkEq(f.len, 2U, "len");
    check(f.data[0] == 0x34U && f.data[1] == 0x12U, "payload");

    // 前导垃圾（含伪帧头）后仍能解析
    proto::Parser p2;
    p2.reset();
    const uint8_t junk[] = {0x00, 0xA5, 0xFF, 0xA5, 0xA5};
    for (uint32_t i = 0; i < sizeof(junk); ++i) {
        (void)p2.feed(junk[i], f);
    }
    check(feedAll(p2, buf, n, f), "垃圾字节后仍能重同步");

    // CRC 被破坏 → 拒绝，且紧跟一帧好的仍能解析
    uint8_t bad[sizeof(buf)];
    memcpy(bad, buf, n);
    bad[3] ^= 0x01U;                        // 改 len 低字节 → CRC 不匹配
    proto::Parser p3;
    p3.reset();
    check(!feedAll(p3, bad, n, f), "CRC 不匹配的帧被拒绝");
    check(feedAll(p3, buf, n, f), "坏帧之后紧跟的好帧可解析");

    // 大载荷（1024）且数据里塞满 0xA5 / 0x03
    uint8_t big[1024];
    for (uint32_t i = 0; i < sizeof(big); i += 2) {
        big[i]     = 0xA5U;
        big[i + 1] = 0x03U;
    }
    uint8_t  buf2[proto::kFrameMax];
    const uint32_t n2 = proto::encode(0x01U, 0x03U, big, 1024U, buf2, sizeof(buf2));
    checkEq(n2, proto::kFrameMax, "满载荷帧长 = 1031");
    proto::Parser p4;
    p4.reset();
    check(feedAll(p4, buf2, n2, f), "载荷含 A5/03 也能解析（长度驱动，不用转义）");
    checkEq(f.len, 1024U, "满载荷 len");
    check(f.data[0] == 0xA5U && f.data[1023] == 0x03U, "满载荷首尾字节");

    // 长度非法（>1024）的帧被丢弃，随后正常帧可解析
    uint8_t over[] = {0xA5, 0x01, 0x02, 0x05, 0x00, 0xAA, 0xBB};   // len=5 但只有 2 字节数据
    proto::Parser p5;
    p5.reset();
    check(!feedAll(p5, over, sizeof(over), f), "长度越界的帧被拒绝");
}

static void testParserSplit(void)
{
    printf("[5] 逐字节喂入（模拟串口）\n");
    static proto::Frame f;
    uint8_t  payload[512];
    for (uint32_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7U);
    }
    uint8_t        buf[proto::kFrameMax];
    const uint32_t n = proto::encode(0x07U, 0x02U, payload, 512U, buf, sizeof(buf));

    proto::Parser p;
    p.reset();
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (p.feed(buf[i], f)) {
            ++hits;
        }
    }
    checkEq(hits, 1U, "整帧只触发一次");
    checkEq(f.id, 0x07U, "id");
    check(memcmp(f.data, payload, 512U) == 0, "512 字节载荷逐字节一致");
}

int main(void)
{
    printf("=== 载体层单测 ===\n");
    testCrc8();
    testCrc32Vectors();
    testEncode();
    testParser();
    testParserSplit();

    printf("\n%d 项断言，%d 项失败 → %s\n", gCase, gFail, gFail == 0 ? "PASS" : "FAIL");
    return gFail == 0 ? 0 : 1;
}
