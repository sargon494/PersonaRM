#include "rendering/mdec_decoder.h"
#include "core/logger.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace personarm {

namespace mdec {

// Standard zigzag scan (inverse of the psx-spx "zigzag" table; checked in test_mdec).
const uint8_t kZagZig[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// psx-spx "Macroblock Decoder (MDEC)", MDEC(3) standard scale table (negatives are one's complement).
const int16_t kDefaultScale[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, -0x18F9, -0x471D, -0x6A6E, -0x7D8B,
    0x7641, 0x30FB, -0x30FC, -0x7642, -0x7642, -0x30FC, 0x30FB, 0x7641,
    0x6A6D, -0x18F9, -0x7D8B, -0x471D, 0x471C, 0x7D8A, 0x18F8, -0x6A6E,
    0x5A82, -0x5A83, -0x5A83, 0x5A82, 0x5A82, -0x5A83, -0x5A83, 0x5A82,
    0x471C, -0x7D8B, 0x18F8, 0x6A6D, -0x6A6E, -0x18F9, 0x7D8A, -0x471D,
    0x30FB, -0x7642, 0x7641, -0x30FC, -0x30FC, 0x7641, -0x7642, 0x30FB,
    0x18F8, -0x471D, 0x6A6D, -0x7D8B, 0x7D8A, -0x6A6E, 0x471C, -0x18F9};

const uint8_t kDefaultQuant[64] = {
    2,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
    27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};

namespace {
int Signed10(uint16_t n) { return static_cast<int16_t>(static_cast<uint16_t>(n << 6)) >> 6; }
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

// psx-spx rl_decode_block. DC: val = signed10*qt[0] (no q_scale, no /8); AC:
// (signed10*qt[k]*q_scale+4)/8; q_scale=0 is the "special mode" val = signed10*2 stored unzigzagged.
bool BlockDecoder::Push(uint16_t n, const uint8_t qt[64]) {
    int val;
    if (k < 0) {
        if (n == 0xFE00) return false;  // padding before a block is skipped
        std::memset(blk, 0, sizeof(blk));
        qscale = (n >> 10) & 0x3F;
        k = 0;
        val = Signed10(n) * qt[0];
    } else {
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) {
            k = -1;
            return true;
        }
        // unverified: "/8" taken as arithmetic shift (psx-spx gives no rounding mode).
        val = (Signed10(n) * qt[k] * qscale + 4) >> 3;
    }
    if (qscale == 0) val = Signed10(n) * 2;
    val = Clamp(val, -0x400, 0x3FF);
    blk[qscale > 0 ? kZagZig[k] : k] = static_cast<int16_t>(val);
    return false;
}

// psx-spx real_idct_core: per pass dst[x+y*8] = (sum_z src[y+z*8]*(scale[x+z*8]/8) + 0FFFh) / 2000h.
// unverified: both divisions taken as arithmetic shifts (hardware keeps the upper 13 bits of the table).
void Idct(int16_t blk[64], const int16_t scale[64]) {
    int32_t a[64], b[64];
    for (int i = 0; i < 64; ++i) a[i] = blk[i];
    int32_t* src = a;
    int32_t* dst = b;
    for (int pass = 0; pass < 2; ++pass) {
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                int64_t sum = 0;
                for (int z = 0; z < 8; ++z) sum += static_cast<int64_t>(src[y + z * 8]) * (scale[x + z * 8] >> 3);
                dst[x + y * 8] = static_cast<int32_t>((sum + 0x0FFF) >> 13);
            }
        }
        std::swap(src, dst);
    }
    for (int i = 0; i < 64; ++i) blk[i] = static_cast<int16_t>(Clamp(src[i], -0x8000, 0x7FFF));
}

// psx-spx yuv_to_rgb: R=Y+1.402Cr, G=Y-0.3437Cb-0.7143Cr, B=Y+1.772Cb, each clamped to -128..127.
// unverified: fixed-point resolution (psx-spx: unknown); 10 fractional bits used here.
void YuvToRgb(const int16_t cr[64], const int16_t cb[64], const int16_t y[64], int xx, int yy, int8_t rgb[768]) {
    for (int py = 0; py < 8; ++py) {
        for (int px = 0; px < 8; ++px) {
            const int c = ((px + xx) >> 1) + ((py + yy) >> 1) * 8;
            const int r = cr[c], bl = cb[c];
            const int Y = y[px + py * 8];
            int8_t* o = rgb + ((px + xx) + (py + yy) * 16) * 3;
            o[0] = static_cast<int8_t>(Clamp(Y + ((1436 * r) >> 10), -128, 127));
            o[1] = static_cast<int8_t>(Clamp(Y + ((-352 * bl - 731 * r) >> 10), -128, 127));
            o[2] = static_cast<int8_t>(Clamp(Y + ((1815 * bl) >> 10), -128, 127));
        }
    }
}

// psx-spx y_to_mono: Y AND 1FFh as signed 9 bit, then saturate to signed 8 bit.
int8_t YToMono(int16_t y) {
    const int v = static_cast<int16_t>(static_cast<uint16_t>(y << 7)) >> 7;
    return static_cast<int8_t>(Clamp(v, -128, 127));
}

} // namespace mdec

namespace {

// Float reference IDCT basis: T[u][x] = C(u) * cos((2x + 1) * u * pi / 16) / 2.
constexpr double PI = 3.14159265358979323846;

static double s_idct_basis[8][8];
static bool s_basis_initialized = false;

static void InitIdctBasis() {
    if (s_basis_initialized) return;
    for (int u = 0; u < 8; ++u) {
        double cu = (u == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
        for (int x = 0; x < 8; ++x) {
            s_idct_basis[u][x] = 0.5 * cu * std::cos((2.0 * x + 1.0) * u * PI / 16.0);
        }
    }
    s_basis_initialized = true;
}

} // anonymous namespace

MdecDecoder::MdecDecoder() {
    InitIdctBasis();
    Reset();
}

void MdecDecoder::Reset() {
    m_decoded_frames = 0;
}

void MdecDecoder::PerformIdct8x8(const int16_t in_coeff[64], uint8_t out_pixels[64]) {
    InitIdctBasis();
    double temp[8][8];
    for (int y = 0; y < 8; ++y) {
        for (int v = 0; v < 8; ++v) {
            double sum = 0.0;
            for (int u = 0; u < 8; ++u) sum += s_idct_basis[u][y] * in_coeff[v * 8 + u];
            temp[v][y] = sum;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (int v = 0; v < 8; ++v) sum += s_idct_basis[v][x] * temp[v][y];
            int val = static_cast<int>(std::round(sum + 128.0));
            out_pixels[y * 8 + x] = static_cast<uint8_t>(std::clamp(val, 0, 255));
        }
    }
}

uint32_t MdecDecoder::YCbCrToRgba(int y, int cb, int cr) {
    double dy = static_cast<double>(y);
    double dcb = static_cast<double>(cb - 128);
    double dcr = static_cast<double>(cr - 128);
    int r = static_cast<int>(std::round(dy + 1.402000 * dcr));
    int g = static_cast<int>(std::round(dy - 0.344136 * dcb - 0.714136 * dcr));
    int b = static_cast<int>(std::round(dy + 1.772000 * dcb));
    uint32_t ur = static_cast<uint32_t>(std::clamp(r, 0, 255));
    uint32_t ug = static_cast<uint32_t>(std::clamp(g, 0, 255));
    uint32_t ub = static_cast<uint32_t>(std::clamp(b, 0, 255));
    return (0xFFu << 24) | (ub << 16) | (ug << 8) | ur;  // 0xAABBGGRR
}

bool MdecDecoder::DecodeFrame(const uint8_t* bitstream_data, size_t bitstream_size,
                              uint16_t width, uint16_t height,
                              MdecFrame& out_frame) {
    if (width == 0 || height == 0) {
        PRM_LOG_ERROR("MDEC", "Invalid frame dimensions: " + std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    out_frame.width = width;
    out_frame.height = height;
    out_frame.pixels_rgba.assign(static_cast<size_t>(width) * height, YCbCrToRgba(128, 128, 128));

    const int mb_w = (width + 15) / 16;
    const int mb_h = (height + 15) / 16;
    const size_t total = bitstream_data ? bitstream_size / 2 : 0;
    size_t pos = 0;

    // Color macroblock: Cr, Cb, Y1..Y4 (psx-spx decode_colored_macroblock).
    // unverified: STR playback uploads kDefaultQuant for both the luma and the color table.
    int16_t blocks[6][64];
    int8_t rgb[768];
    mdec::BlockDecoder dec;
    for (int mb = 0; mb < mb_w * mb_h && pos < total; ++mb) {
        int b = 0;
        while (b < 6 && pos < total) {
            const uint16_t n = static_cast<uint16_t>(bitstream_data[pos * 2] | (bitstream_data[pos * 2 + 1] << 8));
            ++pos;
            if (dec.Push(n, mdec::kDefaultQuant)) {
                std::memcpy(blocks[b], dec.blk, sizeof(dec.blk));
                mdec::Idct(blocks[b], mdec::kDefaultScale);
                ++b;
            }
        }
        if (b < 6) break;  // truncated stream: keep what was decoded
        for (int i = 0; i < 4; ++i) mdec::YuvToRgb(blocks[0], blocks[1], blocks[2 + i], (i & 1) * 8, (i >> 1) * 8, rgb);
        const int ox = (mb / mb_h) * 16, oy = (mb % mb_h) * 16;  // STR macroblocks run in columns
        for (int py = 0; py < 16 && oy + py < height; ++py) {
            for (int px = 0; px < 16 && ox + px < width; ++px) {
                const int8_t* p = rgb + (px + py * 16) * 3;
                const uint32_t r = static_cast<uint8_t>(p[0] ^ 0x80), g = static_cast<uint8_t>(p[1] ^ 0x80),
                               bl = static_cast<uint8_t>(p[2] ^ 0x80);
                out_frame.pixels_rgba[static_cast<size_t>(oy + py) * width + ox + px] = 0xFF000000u | (bl << 16) | (g << 8) | r;
            }
        }
    }

    m_decoded_frames++;
    return true;
}

} // namespace personarm
