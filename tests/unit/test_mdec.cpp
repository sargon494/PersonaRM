// MDEC device, decode core and BS bitstream tests (psx-spx "Macroblock Decoder (MDEC)",
// "CDROM File Video BS Picture Compression"). Synthetic data only.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hw/mdec.h"
#include "rendering/bs_decoder.h"
#include "rendering/mdec_decoder.h"

using namespace personarm;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static uint16_t Dc(int q, int v) { return static_cast<uint16_t>((q << 10) | (v & 0x3FF)); }

static void TestZigzag() {
    // psx-spx zigzag[raster] table; zagzig must be its inverse.
    const int spx[64] = {0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
                         3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                         10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
                         21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
    for (int i = 0; i < 64; ++i) CHECK(mdec::kZagZig[spx[i]] == i);
}

static void TestRlDecodeQuant() {
    mdec::BlockDecoder d;
    const uint8_t* qt = mdec::kDefaultQuant;  // qt[0]=2, qt[1]=16, qt[3]=19
    CHECK(!d.Push(0xFE00, qt));               // padding before DC is skipped
    CHECK(!d.Push(Dc(1, 64), qt));            // DC: 64*qt[0], no q_scale
    CHECK(!d.Push(0x0003, qt));               // run 0, +3 at k=1: (3*16*1+4)>>3 = 6
    CHECK(!d.Push(static_cast<uint16_t>((1 << 10) | (-2 & 0x3FF)), qt));  // run 1, -2 at k=3: (-38+4)>>3 = -5
    CHECK(d.Push(0xFE00, qt));
    CHECK(d.blk[0] == 128);
    CHECK(d.blk[1] == 6);            // zagzig[1] = 1
    CHECK(d.blk[16] == -5);          // zagzig[3] = 16
    int nonzero = 0;
    for (int16_t v : d.blk) nonzero += v != 0;
    CHECK(nonzero == 3);

    // q_scale = 0: special mode, val*2 stored at k (no zigzag); clamp to -400h..3FFh.
    CHECK(!d.Push(Dc(0, 5), qt));
    CHECK(!d.Push(0x0C07, qt));      // run 3 -> k=4, level 7 -> blk[4] = 14
    CHECK(d.Push(0xFE00, qt));
    CHECK(d.blk[0] == 10 && d.blk[4] == 14 && d.blk[2] == 0);
    CHECK(!d.Push(Dc(1, 0x1FF), qt));  // 511*2 = 1022 fits; with qt 83 it would clamp
    const uint8_t big[64] = {83};
    CHECK(d.Push(0xFE00, qt) && d.blk[0] == 1022);
    CHECK(!d.Push(Dc(1, 0x1FF), big));
    CHECK(d.Push(0xFE00, big) && d.blk[0] == 0x3FF);
}

static void TestIdctFlat() {
    for (int dc : {128, -128, 0, 400}) {
        int16_t blk[64] = {};
        blk[0] = static_cast<int16_t>(dc);
        mdec::Idct(blk, mdec::kDefaultScale);
        for (int i = 1; i < 64; ++i) CHECK(blk[i] == blk[0]);
        if (dc == 128) CHECK(blk[0] == 16);  // 128/8, hardware rounding path
        if (dc == 0) CHECK(blk[0] == 0);
    }
}

static void UploadDefaultTables(hw::Mdec& m) {
    m.Write32(0, 0x40000001);
    for (int t = 0; t < 2; ++t)
        for (int i = 0; i < 64; i += 4)
            m.Write32(0, mdec::kDefaultQuant[i] | (mdec::kDefaultQuant[i + 1] << 8) | (mdec::kDefaultQuant[i + 2] << 16) |
                             (static_cast<uint32_t>(mdec::kDefaultQuant[i + 3]) << 24));
    m.Write32(0, 0x60000000);
    for (int i = 0; i < 64; i += 2)
        m.Write32(0, static_cast<uint16_t>(mdec::kDefaultScale[i]) | (static_cast<uint32_t>(static_cast<uint16_t>(mdec::kDefaultScale[i + 1])) << 16));
}

static uint32_t DcBlock(int dc) { return Dc(1, dc) | (0xFE00u << 16); }

static void TestDeviceColorMacroblock() {
    hw::Mdec m;
    CHECK(m.Read32(4) == 0x80040000);
    UploadDefaultTables(m);
    m.Write32(4, 0x60000000);  // enable DREQ0/DREQ1

    // 24-bit unsigned decode, 6 parameter words: Cr=0, Cb=0, Y1=+16, Y2=-16, Y3=0, Y4=+16 (after IDCT).
    m.Write32(0, 0x30000006);
    CHECK(m.Read32(4) == 0xB4040005);  // empty, busy, DREQ0, depth 2, block Cr, 5 left (minus 1)
    m.Write32(0, DcBlock(0));
    CHECK(m.Read32(4) == 0xB4050004);  // block Cb
    m.Write32(0, DcBlock(0));
    CHECK(((m.Read32(4) >> 16) & 7) == 0);  // block Y1
    for (int dc : {64, -64, 0}) m.Write32(0, DcBlock(dc));
    CHECK(((m.Read32(4) >> 16) & 7) == 3);  // block Y4
    m.Write32(0, DcBlock(64));
    // Decode takes 6 blocks of kCyclesPerBlock: busy, out FIFO still empty, no DREQ1.
    CHECK(m.Read32(4) == 0xE404FFFF && !m.DataOutRequest());
    m.Tick(6 * hw::Mdec::kCyclesPerBlock - 1);
    CHECK(!m.DataOutRequest() && m.Read32(0) == 0);
    m.Tick(1);
    CHECK(m.Read32(4) == 0x6C04FFFF);  // last word, busy (output pending), DREQ1, none left
    CHECK(m.DataOutRequest() && !m.DataInRequest());

    std::vector<uint8_t> px;
    for (int i = 0; i < 192; ++i) {
        const uint32_t w = m.DmaOut();
        for (int b = 0; b < 4; ++b) px.push_back(static_cast<uint8_t>(w >> (8 * b)));
    }
    CHECK(m.Read32(4) == 0xC404FFFF);
    auto at = [&](int x, int y, int c) { return px[(x + y * 16) * 3 + c]; };
    for (int c = 0; c < 3; ++c) {
        CHECK(at(0, 0, c) == 144 && at(7, 7, c) == 144);   // Y1
        CHECK(at(8, 0, c) == 112 && at(15, 7, c) == 112);  // Y2
        CHECK(at(0, 8, c) == 128 && at(7, 15, c) == 128);  // Y3
        CHECK(at(8, 8, c) == 144 && at(15, 15, c) == 144); // Y4
    }

    // 15-bit with bit15 set, Cr = +16: R = Y+22, G = Y-12, B = Y.
    m.Write32(0, 0x3A000006);
    m.Write32(0, DcBlock(64));
    for (int i = 0; i < 5; ++i) m.Write32(0, DcBlock(0));
    m.Tick(6 * hw::Mdec::kCyclesPerBlock);
    const uint32_t w = m.Read32(0);
    CHECK((w & 0xFFFF) == 0xC1D2);  // stp | (128>>3)<<10 | (116>>3)<<5 | (150>>3)

    m.Write32(4, 0x80000000);
    CHECK(m.Read32(4) == 0x80040000);
}

static void TestDeviceMono() {
    hw::Mdec m;
    UploadDefaultTables(m);
    m.Write32(0, 0x28000001);  // 8-bit unsigned, 1 word
    m.Write32(0, DcBlock(64));
    m.Tick(hw::Mdec::kCyclesPerBlock);
    for (int i = 0; i < 16; ++i) CHECK(m.Read32(0) == 0x90909090u);  // 144
    CHECK(m.Read32(4) & 0x80000000u);
    m.Write32(0, 0x24000001);  // 4-bit signed: +16 -> 1 per nibble
    m.Write32(0, DcBlock(64));
    m.Tick(hw::Mdec::kCyclesPerBlock);
    for (int i = 0; i < 8; ++i) CHECK(m.Read32(0) == 0x11111111u);
    m.Write32(0, 0x00001234);  // MDEC(0): no function, bits 15-0 to status
    CHECK((m.Read32(4) & 0xFFFF) == 0x1234);
}

static void TestDecodeFrameColumnOrder() {
    // 16x32 frame = 2 macroblocks stacked in one column: MB0 Y=+16, MB1 Y=-16.
    std::vector<uint16_t> rl;
    for (int mb = 0; mb < 2; ++mb)
        for (int b = 0; b < 6; ++b) {
            rl.push_back(Dc(1, b < 2 ? 0 : (mb == 0 ? 64 : -64)));
            rl.push_back(0xFE00);
        }
    MdecDecoder d;
    MdecFrame f;
    CHECK(d.DecodeFrame(reinterpret_cast<const uint8_t*>(rl.data()), rl.size() * 2, 16, 32, f));
    CHECK(f.pixels_rgba[0] == 0xFF909090u);
    CHECK(f.pixels_rgba[31 * 16 + 15] == 0xFF707070u);
}

// --- BS v2 encoder (test-only), the inverse of bs::ToMdec -------------------------------
struct BitWriter {
    std::vector<uint16_t> words;
    int n = 0;
    void Put(uint32_t v, int len) {
        for (int i = len - 1; i >= 0; --i) {
            if (n % 16 == 0) words.push_back(0);
            if ((v >> i) & 1) words.back() |= static_cast<uint16_t>(1u << (15 - n % 16));
            ++n;
        }
    }
    void Put(const char* s) {
        for (; *s; ++s) Put(*s == '1', 1);
    }
};

static std::vector<uint8_t> EncodeV2(const std::vector<std::vector<uint16_t>>& blocks, int q) {
    BitWriter w;
    for (const auto& blk : blocks) {
        w.Put(blk[0] & 0x3FF, 10);
        for (size_t i = 1; i < blk.size(); ++i) {
            const uint16_t v = blk[i];
            const int level = static_cast<int16_t>(static_cast<uint16_t>(v << 6)) >> 6;
            const uint16_t key = static_cast<uint16_t>((v & 0xFC00) | (level < 0 ? -level : level));
            bool done = false;
            for (const bs::AcCode& c : bs::AcCodes())
                if (c.mdec == key && level != 0) {
                    w.Put(c.bits, c.len);
                    w.Put(level < 0, 1);
                    done = true;
                    break;
                }
            if (!done) {
                w.Put("000001");
                w.Put(v, 16);
            }
        }
        w.Put("10");
    }
    std::vector<uint8_t> out = {0, 0, 0x00, 0x38, static_cast<uint8_t>(q), 0, 2, 0};
    for (uint16_t h : w.words) {
        out.push_back(static_cast<uint8_t>(h));
        out.push_back(static_cast<uint8_t>(h >> 8));
    }
    return out;
}

static void TestAcTablePrefixFree() {
    // Kraft sum of codes (+sign bit) + EOB "10" + escape "000001" + unused "000000000000" == 1.
    double sum = 0.25 + 1.0 / 64 + 1.0 / 4096;
    for (const bs::AcCode& c : bs::AcCodes()) sum += 1.0 / (1u << c.len);  // both sign variants
    CHECK(sum > 0.999999 && sum < 1.000001);
    CHECK(bs::AcCodes().size() == 111);
}

static void TestBsRoundTrip() {
    // 2 macroblocks (16x32): table codes (short, long, negative), an escape, and a long run.
    std::vector<std::vector<uint16_t>> blocks;
    const std::vector<uint16_t> acs[] = {
        {0x0001, 0x0401, 0x7C01, 0x0412},
        {static_cast<uint16_t>(0x0000 | (-3 & 0x3FF)), 0x0064 /*escape: 0,100*/, 0xA001 /*escape: run 40*/},
        {},
        {0x0028, static_cast<uint16_t>(0x1800 | (-3 & 0x3FF))},
        {0x0002},
        {0x6C01, 0x0010},
    };
    std::vector<uint16_t> expect;
    for (int mb = 0; mb < 2; ++mb)
        for (int b = 0; b < 6; ++b) {
            std::vector<uint16_t> blk = {static_cast<uint16_t>((b * 37 + mb * 100 - 200) & 0x3FF)};
            blk.insert(blk.end(), acs[b].begin(), acs[b].end());
            blocks.push_back(blk);
            expect.push_back(static_cast<uint16_t>((5 << 10) | blk[0]));
            expect.insert(expect.end(), acs[b].begin(), acs[b].end());
            expect.push_back(0xFE00);
        }
    const std::vector<uint8_t> bs_data = EncodeV2(blocks, 5);
    std::vector<uint16_t> rl;
    CHECK(bs::ToMdec(bs_data.data(), bs_data.size(), 16, 32, rl));
    CHECK(rl.size() % 64 == 0);
    CHECK(std::vector<uint16_t>(rl.begin(), rl.begin() + expect.size()) == expect);
    for (size_t i = expect.size(); i < rl.size(); ++i) CHECK(rl[i] == 0xFE00);

    // The converted stream decodes through the MDEC without desync (2 macroblocks).
    MdecDecoder d;
    MdecFrame f;
    CHECK(d.DecodeFrame(reinterpret_cast<const uint8_t*>(rl.data()), rl.size() * 2, 16, 32, f));

    // Bad header / magic.
    std::vector<uint8_t> bad = bs_data;
    bad[3] = 0x37;
    CHECK(!bs::ToMdec(bad.data(), bad.size(), 16, 32, rl));
}

static void TestBsV3Dc() {
    // One macroblock, v3: Cr "01"+"1" (+1*4), Cb "00" (0), Y1 "01"+"10" (+2*4=8),
    // Y2 "00"+"0" (-1*4 -> 4), Y3 "100", Y4 "100"; each block ends with EOB "10".
    BitWriter w;
    for (const char* s : {"011", "00", "0110", "000", "100", "100"}) {
        w.Put(s);
        w.Put("10");
    }
    std::vector<uint8_t> data = {0, 0, 0x00, 0x38, 3, 0, 3, 0};
    for (uint16_t h : w.words) {
        data.push_back(static_cast<uint8_t>(h));
        data.push_back(static_cast<uint8_t>(h >> 8));
    }
    std::vector<uint16_t> rl;
    CHECK(bs::ToMdec(data.data(), data.size(), 16, 16, rl));
    const int dcs[6] = {4, 0, 8, 4, 4, 4};
    for (int b = 0; b < 6; ++b) {
        CHECK(rl[b * 2] == Dc(3, dcs[b]));
        CHECK(rl[b * 2 + 1] == 0xFE00);
    }
}

int main() {
    TestZigzag();
    TestRlDecodeQuant();
    TestIdctFlat();
    TestDeviceColorMacroblock();
    TestDeviceMono();
    TestDecodeFrameColumnOrder();
    TestAcTablePrefixFree();
    TestBsRoundTrip();
    TestBsV3Dc();
    std::printf("unit_test_mdec: all passed\n");
    return 0;
}
