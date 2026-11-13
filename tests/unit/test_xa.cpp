// XA-ADPCM decoder + 44.1 kHz resampler tests. Hand-built sound groups only.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "audio/xa_adpcm.h"

using namespace personarm;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// 4-bit group: unit u gets `param`, its first three nibbles are n0,n1,n2, everything else 0.
static std::vector<uint8_t> Group4(int u, uint8_t param, int n0, int n1, int n2) {
    std::vector<uint8_t> g(128, 0);
    g[4 + u] = param;  // header bytes 4..11 = units 0..7
    const int nib[3] = {n0, n1, n2};
    for (int j = 0; j < 3; ++j) g[16 + (u >> 1) + j * 4] |= static_cast<uint8_t>((nib[j] & 0xF) << ((u & 1) * 4));
    return g;
}

static std::vector<int16_t> Mono4(const std::vector<uint8_t>& g) {
    XaAdpcmDecoder d;
    std::vector<int16_t> out;
    d.DecodeGroup(g.data(), XaFormat::FromCodingInfo(0x00), out);
    CHECK(out.size() == 8 * 28);
    return out;
}

static void TestFilters() {
    // Filter 0, range 0: nibble << 12; negative nibble.
    auto o = Mono4(Group4(0, 0x00, 1, 0xF, 0));
    CHECK(o[0] == 4096 && o[1] == -4096 && o[2] == 0);
    // Range 12: nibble unshifted; range 13..15 behaves as 9.
    o = Mono4(Group4(0, 0x0C, 7, 0, 0));
    CHECK(o[0] == 7);
    o = Mono4(Group4(0, 0x0D, 1, 0, 0));
    CHECK(o[0] == 8);
    // Filter 1 (60,0): s = old*60/64.
    o = Mono4(Group4(0, 0x10, 1, 0, 0));
    CHECK(o[0] == 4096 && o[1] == 3840 && o[2] == 3600);
    // Filter 2 (115,-52).
    o = Mono4(Group4(0, 0x20, 1, 0, 0));
    CHECK(o[0] == 4096 && o[1] == 7360 && o[2] == 9897);
    // Filter 3 (98,-55).
    o = Mono4(Group4(0, 0x30, 1, 0, 0));
    CHECK(o[0] == 4096 && o[1] == 6272 && o[2] == 6084);
    // History carries into the next unit of the same channel (unit 1, filter 1, zero data).
    std::vector<uint8_t> g = Group4(0, 0x00, 0, 0, 0);
    g[16 + 27 * 4] = 0x01;  // unit 0 last sample = 4096
    g[5] = 0x10;            // unit 1: filter 1
    o = Mono4(g);
    CHECK(o[27] == 4096 && o[28] == 3840);
    // Clamp to 16 bit: filter 2 grows past 7FFFh.
    o = Mono4(Group4(0, 0x20, 7, 7, 7));
    CHECK(o[2] == 0x7FFF);
}

static void TestEightBit() {
    std::vector<uint8_t> g(128, 0);
    g[4] = 0x00;
    g[16] = 0x40;       // unit 0 sample 0: 64 << 8
    g[5] = 0x08;        // unit 1 range 8
    g[17] = 0xC0;       // -64 << 8 >> 8
    XaAdpcmDecoder d;
    std::vector<int16_t> out;
    d.DecodeGroup(g.data(), XaFormat::FromCodingInfo(0x10), out);
    CHECK(out.size() == 4 * 28);
    CHECK(out[0] == 16384 && out[28] == -64);
}

static void TestStereoInterleave() {
    std::vector<uint8_t> g(128, 0);
    g[16] = 0x21;  // unit0 (L) nibble 1, unit1 (R) nibble 2
    g[17] = 0x43;  // unit2 (L) 3, unit3 (R) 4
    XaAdpcmDecoder d;
    std::vector<int16_t> out;
    d.DecodeGroup(g.data(), XaFormat::FromCodingInfo(0x01), out);
    CHECK(out.size() == 8 * 28);
    CHECK(out[0] == 4096 && out[1] == 8192);
    CHECK(out[56] == 12288 && out[57] == 16384);  // after 28 L/R pairs: units 2/3
}

static void TestSectorAndResampleCounts() {
    std::vector<uint8_t> sector(0x900, 0);
    XaAdpcmDecoder d;
    std::vector<int16_t> pcm;
    d.DecodeSector(sector.data(), 0x01, pcm);
    CHECK(pcm.size() == 4032);  // 18 groups x 8 units x 28, stereo -> 2016 frames
    pcm.clear();
    d.DecodeSector(sector.data(), 0x10, pcm);
    CHECK(pcm.size() == 2016);  // 8-bit mono: 18 x 4 x 28

    std::vector<int16_t> st(4032, 1000), out;
    XaResampler r;
    r.Process(st.data(), 2016, 2, 37800, out);
    CHECK(out.size() == 2352 * 2);  // 37.8 kHz: 2016 * 7/6
    out.clear();
    r.Reset();
    r.Process(st.data(), 2016, 2, 18900, out);
    CHECK(out.size() == 4704 * 2);  // 18.9 kHz: 2016 * 7/3
    CHECK(out.back() == 1000);

    // Linear interpolation midpoints at 18.9 kHz (7 outputs per 3 inputs).
    r.Reset();
    const int16_t ramp[3] = {0, 2100, 4200};
    out.clear();
    r.Process(ramp, 3, 1, 18900, out);
    CHECK(out.size() == 7);
    CHECK(out[3] == 600 && out[4] == 1500 && out[6] == 3300);  // one-input delay, 3/7 steps

    std::vector<uint8_t> raw(2352, 0);
    raw[0x12] = 0x48;  // data sector
    CHECK(!d.DecodeRawSector(raw.data(), pcm));
    raw[0x12] = 0x64;
    raw[0x13] = 0x05;
    pcm.clear();
    CHECK(d.DecodeRawSector(raw.data(), pcm) && pcm.size() == 4032);
    CHECK(XaFormat::FromCodingInfo(0x05).rate == 18900 && XaFormat::FromCodingInfo(0x01).rate == 37800);
}

int main() {
    TestFilters();
    TestEightBit();
    TestStereoInterleave();
    TestSectorAndResampleCounts();
    std::printf("unit_test_xa: all passed\n");
    return 0;
}
