#include "rendering/bs_decoder.h"

#include <cstring>

namespace personarm::bs {

namespace {

// psx-spx AC table: prefix, number of index bits 'x', values in index order (sign bit follows).
struct Group {
    const char* prefix;
    int xbits;
    uint16_t values[16];
};
const Group kGroups[] = {
    {"11", 0, {0x0001}},
    {"011", 0, {0x0401}},
    {"010", 1, {0x0002, 0x0801}},
    {"0011", 1, {0x1001, 0x0C01}},
    {"00101", 0, {0x0003}},
    {"00100", 3, {0x3401, 0x0006, 0x3001, 0x2C01, 0x0C02, 0x0403, 0x0005, 0x2801}},
    {"0001", 2, {0x1C01, 0x1801, 0x0402, 0x1401}},
    {"00001", 2, {0x0802, 0x2401, 0x0004, 0x2001}},
    {"0000001", 3, {0x4001, 0x1402, 0x0007, 0x0803, 0x0404, 0x3C01, 0x3801, 0x1002}},
    {"00000001", 4, {0x000B, 0x2002, 0x1003, 0x000A, 0x0804, 0x1C02, 0x5401, 0x5001,
                     0x0009, 0x4C01, 0x4801, 0x0405, 0x0C03, 0x0008, 0x1802, 0x4401}},
    {"000000001", 4, {0x2802, 0x2402, 0x1403, 0x0C04, 0x0805, 0x0407, 0x0406, 0x000F,
                      0x000E, 0x000D, 0x000C, 0x6801, 0x6401, 0x6001, 0x5C01, 0x5801}},
    {"0000000001", 4, {0x001F, 0x001E, 0x001D, 0x001C, 0x001B, 0x001A, 0x0019, 0x0018,
                       0x0017, 0x0016, 0x0015, 0x0014, 0x0013, 0x0012, 0x0011, 0x0010}},
    {"00000000001", 4, {0x0028, 0x0027, 0x0026, 0x0025, 0x0024, 0x0023, 0x0022, 0x0021,
                        0x0020, 0x040E, 0x040D, 0x040C, 0x040B, 0x040A, 0x0409, 0x0408}},
    {"000000000001", 4, {0x0412, 0x0411, 0x0410, 0x040F, 0x1803, 0x4002, 0x3C02, 0x3802,
                         0x3402, 0x3002, 0x2C02, 0x7C01, 0x7801, 0x7401, 0x7001, 0x6C01}},
};

// v3 DC size codes (psx-spx v3 DC table): code -> number of 'snnn' bits.
struct DcCode {
    const char* code;
    int size;
};
const DcCode kDcLuma[] = {{"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4},
                          {"1110", 5}, {"11110", 6}, {"111110", 7}, {"1111110", 8}};
const DcCode kDcChroma[] = {{"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4},
                            {"11110", 5}, {"111110", 6}, {"1111110", 7}, {"11111110", 8}};

// 16-bit peek -> (len << 16 | mdec), 0 = not an AC code.
const std::vector<uint32_t>& Lut() {
    static const std::vector<uint32_t> lut = [] {
        std::vector<uint32_t> t(65536, 0);
        for (const AcCode& c : AcCodes()) {
            const uint32_t lo = static_cast<uint32_t>(c.bits) << (16 - c.len);
            for (uint32_t i = 0; i < (1u << (16 - c.len)); ++i) t[lo | i] = (static_cast<uint32_t>(c.len) << 16) | c.mdec;
        }
        return t;
    }();
    return lut;
}

// Bitstream of little-endian halfwords, bit 15 first (psx-spx BS v1/v2/v3 bit order).
struct Bits {
    const uint8_t* d;
    size_t nbits;
    size_t pos = 0;
    uint32_t Peek(int n) const {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const size_t p = pos + i;
            uint32_t bit = 0;
            if (p < nbits) {
                const size_t h = (p >> 4) * 2;
                bit = ((d[h] | (d[h + 1] << 8)) >> (15 - (p & 15))) & 1;
            }
            v = (v << 1) | bit;
        }
        return v;
    }
    uint32_t Read(int n) {
        const uint32_t v = Peek(n);
        pos += n;
        return v;
    }
    bool Match(const char* code) {
        const int n = static_cast<int>(std::strlen(code));
        uint32_t want = 0;
        for (int i = 0; i < n; ++i) want = (want << 1) | (code[i] == '1');
        if (Peek(n) != want) return false;
        pos += n;
        return true;
    }
    bool Overrun() const { return pos > nbits; }
};

} // namespace

const std::vector<AcCode>& AcCodes() {
    static const std::vector<AcCode> codes = [] {
        std::vector<AcCode> v;
        for (const Group& g : kGroups) {
            uint16_t prefix = 0;
            const int plen = static_cast<int>(std::strlen(g.prefix));
            for (int i = 0; i < plen; ++i) prefix = static_cast<uint16_t>((prefix << 1) | (g.prefix[i] == '1'));
            for (int x = 0; x < (1 << g.xbits); ++x)
                v.push_back({static_cast<uint16_t>((prefix << g.xbits) | x), static_cast<uint8_t>(plen + g.xbits), g.values[x]});
        }
        return v;
    }();
    return codes;
}

bool ReadHeader(const uint8_t* bs, size_t size, Header& out) {
    if (!bs || size < 8) return false;
    auto u16 = [&](int o) { return static_cast<uint16_t>(bs[o] | (bs[o + 1] << 8)); };
    out = {u16(0), u16(2), u16(4), u16(6)};
    return out.magic == 0x3800 && out.version >= 1 && out.version <= 3 && out.qscale <= 0x3F;
}

bool ToMdec(const uint8_t* bs, size_t size, uint16_t width, uint16_t height, std::vector<uint16_t>& out) {
    Header h;
    if (!ReadHeader(bs, size, h)) return false;
    out.clear();
    Bits b{bs + 8, (size - 8) / 2 * 16};
    const std::vector<uint32_t>& lut = Lut();
    const int mbs = ((width + 15) / 16) * ((height + 15) / 16);
    const uint16_t q = static_cast<uint16_t>(h.qscale << 10);
    int old_dc[3] = {0, 0, 0};  // v3 predictors: Cr, Cb, Y1..Y4 (reset per frame)

    for (int mb = 0; mb < mbs; ++mb) {
        for (int blk = 0; blk < 6; ++blk) {
            // DC
            if (h.version == 3) {
                if (b.Peek(10) == 0x3FF) goto done;  // end of frame
                const DcCode* tab = blk < 2 ? kDcChroma : kDcLuma;
                int size_bits = -1;
                for (int i = 0; i < 9 && size_bits < 0; ++i)
                    if (b.Match(tab[i].code)) size_bits = tab[i].size;
                if (size_bits < 0) return false;
                int diff = 0;
                if (size_bits > 0) {
                    diff = static_cast<int>(b.Read(size_bits));
                    if (!(diff >> (size_bits - 1))) diff -= (1 << size_bits) - 1;  // s=0: negative
                }
                int& old = old_dc[blk < 2 ? blk : 2];
                old += diff * 4;
                out.push_back(static_cast<uint16_t>(q | (old & 0x3FF)));  // newer v3 decoders: 10-bit wrap
            } else {
                const uint32_t dc = b.Read(10);
                if (blk == 0 && dc == 0x1FF) goto done;  // end of frame in place of the Cr DC
                out.push_back(static_cast<uint16_t>(q | dc));
            }
            // AC
            for (;;) {
                if (b.Overrun()) return false;
                if (b.Peek(2) == 2) {  // "10" EOB
                    b.pos += 2;
                    out.push_back(0xFE00);
                    break;
                }
                if (b.Peek(6) == 1) {  // "000001" escape + literal MDEC halfword
                    b.pos += 6;
                    out.push_back(static_cast<uint16_t>(b.Read(16)));
                    continue;
                }
                const uint32_t e = lut[b.Peek(16)];
                if (e == 0) return false;
                b.pos += e >> 16;
                uint16_t v = static_cast<uint16_t>(e);
                if (b.Read(1)) v = static_cast<uint16_t>((v & 0xFC00) | ((-(v & 0x3FF)) & 0x3FF));
                out.push_back(v);
            }
        }
    }
done:
    if (b.Overrun()) return false;
    while (out.size() % 64) out.push_back(0xFE00);
    return true;
}

} // namespace personarm::bs
