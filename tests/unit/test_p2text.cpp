// Tests for tools/p2text: LZ codec, control-code tokenizer, bank rebuild with offset fix-ups. Synthetic data only.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "p2text.h"

using namespace personarm::tools::p2text;

static int g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static void RoundTrip(const std::vector<uint8_t>& d, bool overlap) {
    const auto c = LzCompress(d.data(), d.size(), overlap);
    std::vector<uint8_t> back;
    size_t used = 0;
    CHECK(LzDecompress(c.data(), c.size(), d.size(), back, &used));
    CHECK(back == d && used == c.size());
}

static void TestLz() {
    // game semantics: literal run then backward copy (distance = byte + 1)
    const uint8_t v[] = {0x02, 'A', 'B', 'C', 0x80, 0x02};
    std::vector<uint8_t> out;
    CHECK(LzDecompress(v, sizeof v, 6, out) && std::string(out.begin(), out.end()) == "ABCABC");
    // overlapping copy (RLE style) is decoded byte by byte
    const uint8_t r[] = {0x00, 'x', 0x82, 0x00};
    CHECK(LzDecompress(r, sizeof r, 6, out) && std::string(out.begin(), out.end()) == "xxxxxx");
    CHECK(!LzDecompress(v, 5, 6, out));                               // truncated copy
    const uint8_t far[] = {0x00, 'x', 0x80, 0x05};
    CHECK(!LzDecompress(far, sizeof far, 4, out));                    // distance before start
    CHECK(!LzDecompress(v, sizeof v, 5, out));                        // copy overruns declared size

    std::mt19937 rng(7);
    std::vector<std::vector<uint8_t>> cases{{}, {1}, {1, 2}, {1, 2, 3, 1, 2, 3}};
    cases.push_back(std::vector<uint8_t>(1000, 0x55));                // long run
    std::vector<uint8_t> lit(129); for (auto& b : lit) b = uint8_t(rng());
    cases.push_back(lit);                                             // literal run crossing 128
    std::vector<uint8_t> rnd(5000); for (auto& b : rnd) b = uint8_t(rng());
    cases.push_back(rnd);
    std::vector<uint8_t> rep(4000); for (size_t i = 0; i < rep.size(); ++i) rep[i] = uint8_t((i * i / 7) % 13);
    cases.push_back(rep);
    std::vector<uint8_t> far2(600); for (auto& b : far2) b = uint8_t(rng() & 3);   // matches at the 256 window edge
    cases.push_back(far2);
    for (const auto& c : cases) { RoundTrip(c, false); RoundTrip(c, true); }
    // default mode never emits an overlapping copy
    const auto c = LzCompress(cases[4].data(), cases[4].size(), false);
    for (size_t i = 0; i < c.size();) {
        if (c[i] < 0x80) { i += size_t(c[i]) + 2; continue; }
        CHECK(size_t(c[i] & 0x7F) + 3 <= size_t(c[i + 1]) + 1);
        i += 2;
    }
}

static void TestText() {
    GlyphMap m;
    m.Set(0x0013, "あ");
    std::vector<uint16_t> codes;
    std::vector<std::string> err;
    CHECK(Encode("<color=11>Hi あ<br><indent>x<page><c:1331 0001 0002><g0200><end>", m, codes, err));
    const std::vector<uint16_t> want{0x122E, 11, 'H', 'i', ' ', 0x13, 0x1101, 0x1131, 'x', 0x1102, 0x1331, 1, 2, 200, 0x1103};
    CHECK(codes == want);
    CHECK(Render(codes, m) == "<color=11>Hi あ<br><indent>x<page><c:1331 0001 0002><g0200><end>");
    // '<' glyph (0x3C) must not render as a raw '<'
    CHECK(Render({0x3C, 0x1103}, m) == "<g0060><end>");
    // errors are reported, with the offending character
    codes.clear(); err.clear();
    CHECK(!Encode("ab\xE4\xB8\x80<end>", m, codes, err) && err.size() == 1 && err[0].find("U+4E00") != std::string::npos);
    codes.clear(); err.clear();
    CHECK(!Encode("ab", m, codes, err));                              // no <end>
    codes.clear(); err.clear();
    CHECK(!Encode("a<end>b<end>", m, codes, err));                    // two terminators
    codes.clear(); err.clear();
    CHECK(!Encode("<bogus><c:1331 0001><end>", m, codes, err) && err.size() == 2);   // unknown tag, wrong length
    // control length: 0x1Lxx consumes L u16
    CHECK(CodeLength(0x122E) == 2 && CodeLength(0x1101) == 1 && CodeLength(0x0041) == 1);

    const auto tmp = (std::filesystem::temp_directory_path() / "p2text_test_map.txt").string();
    { std::ofstream f(tmp, std::ios::binary); f << "# c\n300\t\xE6\x97\xA5\n0x12C\t\xE6\x9C\x88\n"; }   // 0x12C == 300: later line wins
    GlyphMap m2; std::string e;
    CHECK(m2.Load(tmp, e) && m2.to_text.at(300) == "\xE6\x9C\x88" && m2.to_glyph.count("\xE6\x9C\x88") && !m2.to_glyph.count("\xE6\x97\xA5"));
    { std::ofstream f(tmp, std::ios::binary); f << "5\tab\n"; }
    CHECK(!m2.Load(tmp, e));
    std::filesystem::remove(tmp);
}

static void P32(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); }
static void P16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }

// bank: 3 sections; sec1 = (op,ptr) pairs; sec2 = two op-19 operands {msg offset, filler}; sec3 = 3 messages (#1 orphan)
static std::vector<uint8_t> MakeBankDec(const std::vector<std::vector<uint16_t>>& msgs, const std::vector<uint32_t>& offs) {
    std::vector<uint8_t> d;
    P32(d, 0x18); P32(d, 0x18); P32(d, 3); P32(d, 24); P32(d, 48); P32(d, 64);
    P32(d, 5); P32(d, 0);                                              // other op
    P32(d, 19); P32(d, 48);
    P32(d, 19); P32(d, 56);
    P32(d, offs[0]); P32(d, 0x3C);
    P32(d, offs[1]); P32(d, 0x3C);
    for (const auto& m : msgs) for (uint16_t c : m) P16(d, c);
    return d;
}

static std::vector<uint8_t> MakeFile(const std::vector<uint8_t>& dec) {
    const auto comp = LzCompress(dec.data(), dec.size());
    std::vector<uint8_t> f;
    P32(f, 0x201); P32(f, uint32_t(12 + comp.size())); P32(f, uint32_t(dec.size()));
    f.insert(f.end(), comp.begin(), comp.end());
    while (f.size() % 4) f.push_back(0);
    f.resize(0x800, 0);                                                // group padding
    P32(f, 0x001); P32(f, 16); P32(f, 0xAABBCCDD); P32(f, 0x11223344); // next chunk at the sector boundary
    f.resize(0x1000, 0);
    return f;
}

static void TestPacked() {
    GlyphMap m; m.packed = true;
    std::vector<uint16_t> codes; std::vector<std::string> err;
    // odd length: last word has a zero high byte; even length: extra 0x0000 word; '<' escaped
    CHECK(Encode("<color=1>Abc<br>Hi<lt>!<end>", m, codes, err));
    const std::vector<uint16_t> want{0x122E, 1, 0x1136, 0x6241, 0x0063, 0x1101, 0x1136, 0x6948, 0x213C, 0x0000, 0x1103};
    CHECK(codes == want);
    CHECK(Render(codes, m) == "<color=1>Abc<br>Hi<lt>!<end>");
    // non-canonical runs and bare glyphs stay raw
    CHECK(Render({0x1136, 0x4100, 0x1103}, m) == "<c:1136><c:4100><end>");
    CHECK(Render({0x1136, 0x0000, 0x1103}, m) == "<c:1136><g0000><end>");
    CHECK(Render({0x41, 0x1103}, m) == "<g0065><end>");
    CHECK(UsesPackedText(Messages{{Message{0, 0, {0x1136, 0x0041, 0x1103}, 0}}, {}, 0, 0}));
    CHECK(!UsesPackedText(Messages{{Message{0, 0, {0x41, 0x1103}, 0}}, {}, 0, 0}));
    codes.clear(); err.clear();
    CHECK(!Encode("ã<end>", m, codes, err));              // non-ASCII has no packed form
}

static void TestBank() {
    const std::vector<std::vector<uint16_t>> msgs{
        {'A', 0x1101, 'B', 0x1102, 0x1103},
        {'o', 'r', 'p', 0x1103},                                       // not referenced by any op 19
        {0x122E, 3, 'C', 0x1103}};
    const std::vector<uint32_t> offs{0, 18};                           // msg 0 = 10 B, msg 1 = 8 B -> msg 2 at 18
    const auto dec = MakeBankDec(msgs, offs);
    auto file = MakeFile(dec);

    const auto banks = FindBanks(file);
    CHECK(banks.size() == 1 && banks[0].pos == 0 && banks[0].usize == dec.size());
    std::vector<uint8_t> d2;
    Bank b;
    CHECK(DecodeBankChunk(file, banks[0], d2) && d2 == dec && ParseBank(d2, b));
    const Messages m = SplitMessages(b);
    CHECK(m.list.size() == 3 && m.orphans == 1 && m.bad_refs == 0 && m.list[1].refs == 0 && m.list[2].offset == 18);
    CHECK(m.list[0].codes == msgs[0] && m.list[2].codes == msgs[2]);

    // no edits: identical bank, original chunk bytes kept
    std::vector<std::vector<uint16_t>> codes;
    for (const auto& x : m.list) codes.push_back(x.codes);
    RebuildReport rep;
    std::string err;
    std::vector<uint8_t> out;
    CHECK(RebuildBank(b, m, codes, out, rep, err) && out == dec && rep.changed == 0);
    std::vector<uint8_t> patched;
    CHECK(PatchFile(file, banks[0], out, true, patched, err) && patched == file);

    // grow message 0 by 2 glyphs: later offsets and the op-19 operand of msg 2 follow; the orphan moves (warning)
    codes[0].insert(codes[0].end() - 1, {'x', 'y'});
    CHECK(RebuildBank(b, m, codes, out, rep, err) && rep.changed == 1 && rep.moved_orphans == 1 && !rep.warnings.empty());
    Bank b2;
    CHECK(ParseBank(out, b2));
    const Messages m2 = SplitMessages(b2);
    CHECK(m2.list.size() == 3 && m2.list[0].codes == codes[0] && m2.list[2].offset == 22 && m2.list[2].refs == 1 && m2.list[0].refs == 1);
    uint32_t ref2;
    std::memcpy(&ref2, out.data() + 56, 4);
    CHECK(ref2 == 22);
    CHECK(b2.msg_off == b.msg_off && out.size() % 4 == 0);

    // small growth keeps the next chunk at 0x800
    CHECK(PatchFile(file, banks[0], out, true, patched, err));
    CHECK(patched.size() == file.size() && std::memcmp(patched.data() + 0x800, file.data() + 0x800, 16) == 0);
    auto again = FindBanks(patched);
    CHECK(again.size() == 1 && DecodeBankChunk(patched, again[0], d2) && d2 == out);

    // big incompressible growth pushes the next chunk to 0x1000 and the file to a sector multiple
    std::mt19937 rng(3);
    codes[1].clear();
    for (int i = 0; i < 1400; ++i) codes[1].push_back(uint16_t(0x80 + rng() % 0x700));
    codes[1].push_back(0x1103);
    CHECK(RebuildBank(b, m, codes, out, rep, err) && PatchFile(file, banks[0], out, true, patched, err));
    CHECK(patched.size() == 0x1800 && std::memcmp(patched.data() + 0x1000, file.data() + 0x800, 16) == 0);
    again = FindBanks(patched);
    CHECK(again.size() == 1 && DecodeBankChunk(patched, again[0], d2) && d2 == out);
    CHECK(SplitMessages([&] { Bank x; ParseBank(d2, x); return x; }()).list[1].codes == codes[1]);

    CHECK(!RebuildBank(b, m, {codes[0]}, out, rep, err));              // wrong message count
}

static void TestFont() {
    std::vector<uint8_t> font(0x480 + 18 * 4, 0);
    font[0x480 + 18] = 1;                                              // glyph 1, pixel (0,0)
    const auto bmp = FontSheetBmp(font, 0, 4);
    CHECK(bmp.size() > 1078 && bmp[0] == 'B' && bmp[1] == 'M');
}

int main() {
    TestLz();
    TestText();
    TestPacked();
    TestBank();
    TestFont();
    std::printf("unit_test_p2text: %d checks OK\n", g_checks);
    return 0;
}
