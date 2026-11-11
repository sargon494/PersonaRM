#include "png_codec.h"

#include "core/hash.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace personarm {

namespace {

// ── Inflate (puff-style canonical Huffman decoding) ────────────────────

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool error = false;

    uint32_t Bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= size) { error = true; return 0; }
            val |= static_cast<uint32_t>(data[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = need == 32 ? 0 : (val >> need);
        bitcnt -= need;
        return need == 32 ? val : (val & ((1u << need) - 1u));
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

// Returns <0 if over-subscribed, >0 if incomplete, 0 if complete.
int BuildHuffman(Huffman& h, const uint16_t* length, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; ++s) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s) {
        if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<uint16_t>(s);
    }
    return left;
}

int DecodeSymbol(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(br.Bits(1));
        if (br.error) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint16_t kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint16_t kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool InflateCodes(BitReader& br, std::vector<uint8_t>& out, size_t max_out,
                  const Huffman& lencode, const Huffman& distcode) {
    while (true) {
        int sym = DecodeSymbol(br, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= max_out) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + br.Bits(kLenExtra[sym]);
            int dsym = DecodeSymbol(br, distcode);
            if (dsym < 0 || dsym >= 30 || br.error) return false;
            size_t dist = kDistBase[dsym] + br.Bits(kDistExtra[dsym]);
            if (br.error || dist > out.size() || out.size() + len > max_out) return false;
            size_t from = out.size() - dist;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

bool InflateStored(BitReader& br, std::vector<uint8_t>& out, size_t max_out) {
    br.bitbuf = 0;
    br.bitcnt = 0;
    if (br.pos + 4 > br.size) return false;
    uint32_t len = br.data[br.pos] | (br.data[br.pos + 1] << 8);
    uint32_t nlen = br.data[br.pos + 2] | (br.data[br.pos + 3] << 8);
    br.pos += 4;
    if (len != (~nlen & 0xFFFFu)) return false;
    if (br.pos + len > br.size || out.size() + len > max_out) return false;
    out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
    br.pos += len;
    return true;
}

bool InflateFixed(BitReader& br, std::vector<uint8_t>& out, size_t max_out) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        uint16_t lengths[288];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        BuildHuffman(lencode, lengths, 288);
        for (s = 0; s < 30; ++s) lengths[s] = 5;
        BuildHuffman(distcode, lengths, 30);
        built = true;
    }
    return InflateCodes(br, out, max_out, lencode, distcode);
}

bool InflateDynamic(BitReader& br, std::vector<uint8_t>& out, size_t max_out) {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlen = static_cast<int>(br.Bits(5)) + 257;
    int ndist = static_cast<int>(br.Bits(5)) + 1;
    int ncode = static_cast<int>(br.Bits(4)) + 4;
    if (br.error || nlen > 286 || ndist > 30) return false;

    uint16_t lengths[320] = {0};
    for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint16_t>(br.Bits(3));
    if (br.error) return false;
    Huffman lencode, distcode;
    if (BuildHuffman(lencode, lengths, 19) != 0) return false;

    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = DecodeSymbol(br, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[idx++] = static_cast<uint16_t>(sym);
        } else {
            uint16_t len = 0;
            int rep;
            if (sym == 16) {
                if (idx == 0) return false;
                len = lengths[idx - 1];
                rep = 3 + static_cast<int>(br.Bits(2));
            } else if (sym == 17) {
                rep = 3 + static_cast<int>(br.Bits(3));
            } else {
                rep = 11 + static_cast<int>(br.Bits(7));
            }
            if (br.error || idx + rep > nlen + ndist) return false;
            while (rep--) lengths[idx++] = len;
        }
    }
    if (lengths[256] == 0) return false; // no end-of-block code
    int err = BuildHuffman(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = BuildHuffman(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return InflateCodes(br, out, max_out, lencode, distcode);
}

// ── PNG helpers ────────────────────────────────────────────────────────

uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void WriteBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void WriteChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& body) {
    WriteBE32(out, static_cast<uint32_t>(body.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    WriteBE32(out, Crc32(out.data() + start, out.size() - start));
}

int Paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

bool SetError(std::string* error, const char* msg) {
    if (error) *error = msg;
    return false;
}

} // namespace

bool PngCodec::Inflate(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out,
                       size_t max_out, size_t* consumed) {
    BitReader br{src, src_size};
    bool last;
    do {
        last = br.Bits(1) != 0;
        uint32_t type = br.Bits(2);
        if (br.error) return false;
        bool ok = false;
        switch (type) {
            case 0: ok = InflateStored(br, out, max_out); break;
            case 1: ok = InflateFixed(br, out, max_out); break;
            case 2: ok = InflateDynamic(br, out, max_out); break;
            default: ok = false;
        }
        if (!ok || br.error) return false;
    } while (!last);
    if (consumed) *consumed = br.pos;
    return true;
}

std::optional<PngImage> PngCodec::Decode(const uint8_t* data, size_t size, std::string* error) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || std::memcmp(data, kSig, 8) != 0) { SetError(error, "not a PNG"); return std::nullopt; }

    uint32_t width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<uint8_t> palette;      // RGB triples
    std::vector<uint8_t> palette_alpha;
    std::vector<uint8_t> idat;
    bool have_ihdr = false, have_iend = false;

    size_t pos = 8;
    while (pos + 12 <= size && !have_iend) {
        uint32_t len = ReadBE32(data + pos);
        if (len > size - pos - 12) { SetError(error, "truncated chunk"); return std::nullopt; }
        const uint8_t* type = data + pos + 4;
        const uint8_t* body = data + pos + 8;
        if (Crc32(type, 4 + len) != ReadBE32(body + len)) { SetError(error, "chunk CRC mismatch"); return std::nullopt; }

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) { SetError(error, "bad IHDR"); return std::nullopt; }
            width = ReadBE32(body);
            height = ReadBE32(body + 4);
            bit_depth = body[8];
            color_type = body[9];
            interlace = body[12];
            have_ihdr = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            if (len % 3 != 0 || len > 768) { SetError(error, "bad PLTE"); return std::nullopt; }
            palette.assign(body, body + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            if (color_type == 3) palette_alpha.assign(body, body + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            have_iend = true;
        }
        pos += 12 + len;
    }
    if (!have_ihdr || idat.empty()) { SetError(error, "missing IHDR/IDAT"); return std::nullopt; }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
        static_cast<uint64_t>(width) * height > kMaxPixels) { SetError(error, "unsupported dimensions"); return std::nullopt; }
    if (interlace != 0) { SetError(error, "interlaced PNG not supported"); return std::nullopt; }

    int channels;
    switch (color_type) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: SetError(error, "bad color type"); return std::nullopt;
    }
    bool depth_ok = (bit_depth == 8 || bit_depth == 16) ||
                    ((color_type == 0 || color_type == 3) && (bit_depth == 1 || bit_depth == 2 || bit_depth == 4));
    if (!depth_ok) { SetError(error, "bad bit depth"); return std::nullopt; }
    if (color_type == 3 && palette.empty()) { SetError(error, "missing PLTE"); return std::nullopt; }

    const size_t bits_pp = static_cast<size_t>(channels) * bit_depth;
    const size_t bpp = std::max<size_t>(1, bits_pp / 8);
    const size_t row_bytes = (static_cast<size_t>(width) * bits_pp + 7) / 8;
    const size_t expected = (row_bytes + 1) * height;

    // zlib wrapper: CMF/FLG header, deflate stream, Adler-32
    if (idat.size() < 6 || (idat[0] & 0x0F) != 8 || ((idat[0] << 8) | idat[1]) % 31 != 0 || (idat[1] & 0x20)) {
        SetError(error, "bad zlib header"); return std::nullopt;
    }
    std::vector<uint8_t> raw;
    raw.reserve(expected);
    size_t consumed = 0;
    if (!Inflate(idat.data() + 2, idat.size() - 2, raw, expected, &consumed) || raw.size() != expected) {
        SetError(error, "corrupt or oversized deflate stream"); return std::nullopt;
    }
    if (2 + consumed + 4 <= idat.size() &&
        Adler32(raw.data(), raw.size()) != ReadBE32(idat.data() + 2 + consumed)) {
        SetError(error, "Adler-32 mismatch"); return std::nullopt;
    }

    // Unfilter in place
    std::vector<uint8_t> img(row_bytes * height);
    std::vector<uint8_t> zero_row(row_bytes, 0);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t filter = raw[y * (row_bytes + 1)];
        const uint8_t* src = &raw[y * (row_bytes + 1) + 1];
        uint8_t* cur = &img[y * row_bytes];
        const uint8_t* prev = y ? &img[(y - 1) * row_bytes] : zero_row.data();
        if (filter > 4) { SetError(error, "bad filter type"); return std::nullopt; }
        for (size_t i = 0; i < row_bytes; ++i) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            int x = src[i];
            switch (filter) {
                case 0: break;
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: x += Paeth(a, b, c); break;
            }
            cur[i] = static_cast<uint8_t>(x);
        }
    }

    // Expand to RGBA8
    PngImage out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);
    auto sample = [&](const uint8_t* row, uint32_t x, int ch) -> uint32_t {
        // returns the sample scaled to 8 bits (palette index/gray handled by callers)
        if (bit_depth == 8) return row[x * channels + ch];
        if (bit_depth == 16) return row[(x * channels + ch) * 2]; // high byte
        // 1/2/4-bit single channel
        size_t bit = static_cast<size_t>(x) * bit_depth;
        uint32_t v = (row[bit / 8] >> (8 - bit_depth - (bit % 8))) & ((1u << bit_depth) - 1u);
        return v;
    };
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = &img[y * row_bytes];
        uint8_t* dst = &out.rgba[static_cast<size_t>(y) * width * 4];
        for (uint32_t x = 0; x < width; ++x, dst += 4) {
            switch (color_type) {
                case 0: {
                    uint32_t g = sample(row, x, 0);
                    if (bit_depth < 8) g = g * 255 / ((1u << bit_depth) - 1u);
                    dst[0] = dst[1] = dst[2] = static_cast<uint8_t>(g); dst[3] = 255; break;
                }
                case 2:
                    dst[0] = static_cast<uint8_t>(sample(row, x, 0));
                    dst[1] = static_cast<uint8_t>(sample(row, x, 1));
                    dst[2] = static_cast<uint8_t>(sample(row, x, 2));
                    dst[3] = 255; break;
                case 3: {
                    uint32_t idx = sample(row, x, 0);
                    if (idx * 3 + 2 >= palette.size()) { SetError(error, "palette index out of range"); return std::nullopt; }
                    dst[0] = palette[idx * 3]; dst[1] = palette[idx * 3 + 1]; dst[2] = palette[idx * 3 + 2];
                    dst[3] = idx < palette_alpha.size() ? palette_alpha[idx] : 255; break;
                }
                case 4: {
                    uint8_t g = static_cast<uint8_t>(sample(row, x, 0));
                    dst[0] = dst[1] = dst[2] = g; dst[3] = static_cast<uint8_t>(sample(row, x, 1)); break;
                }
                case 6:
                    for (int c = 0; c < 4; ++c) dst[c] = static_cast<uint8_t>(sample(row, x, c));
                    break;
            }
        }
    }
    return out;
}

std::optional<PngImage> PngCodec::DecodeFile(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { SetError(error, "cannot open file"); return std::nullopt; }
    std::streamoff len = f.tellg();
    if (len <= 0 || len > 256ll * 1024 * 1024) { SetError(error, "bad file size"); return std::nullopt; }
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), len);
    if (!f) { SetError(error, "read error"); return std::nullopt; }
    return Decode(buf.data(), buf.size(), error);
}

std::vector<uint8_t> PngCodec::Encode(uint32_t width, uint32_t height, const uint8_t* rgba) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    WriteBE32(ihdr, width);
    WriteBE32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    WriteChunk(out, "IHDR", ihdr);

    const size_t row_bytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((row_bytes + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: None
        raw.insert(raw.end(), rgba + y * row_bytes, rgba + (y + 1) * row_bytes);
    }

    std::vector<uint8_t> z = {0x78, 0x01};
    size_t off = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                 raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
    } while (off < raw.size());
    WriteBE32(z, Adler32(raw.data(), raw.size()));
    WriteChunk(out, "IDAT", z);
    WriteChunk(out, "IEND", {});
    return out;
}

bool PngCodec::EncodeToFile(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
    std::vector<uint8_t> bytes = Encode(width, height, rgba);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

} // namespace personarm
