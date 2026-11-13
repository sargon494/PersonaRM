#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// mdec:: — hardware MDEC decode core, shared by hw::Mdec (register device) and
// MdecDecoder (whole-frame helper). Source: psx-spx "Macroblock Decoder (MDEC)":
// rl_decode_block, real_idct_core, yuv_to_rgb, y_to_mono, MDEC(3) default scale table.
// ───────────────────────────────────────────────────────────────────────────
namespace mdec {

/// zagzig[k] = raster index of the k-th coefficient in zigzag order (psx-spx: zagzig[zigzag[i]]=i).
extern const uint8_t kZagZig[64];
/// Standard scale table uploaded with MDEC(3) (psx-spx table, row = frequency, column = position).
extern const int16_t kDefaultScale[64];
/// MPEG-1 intra quant matrix with DC entry 2, in zigzag order (the MDEC(2) upload order).
// unverified: PsyQ DecDCTvlc uploads this table; psx-spx only shows its first bytes (02h,10h,10h,13h).
extern const uint8_t kDefaultQuant[64];

/// rl_decode_block as a halfword state machine (one RL halfword at a time).
struct BlockDecoder {
    int16_t blk[64] = {};  // dequantized coefficients, raster order
    int k = -1;            // -1 = waiting for the DC halfword
    int qscale = 0;
    /// Returns true when this halfword completed the block (k pushed past 63, usually by FE00h).
    bool Push(uint16_t n, const uint8_t qt[64]);
};

/// real_idct_core: two passes of sum(src*(scale/8)), (sum+0FFFh)/2000h. In place.
void Idct(int16_t blk[64], const int16_t scale[64]);

/// yuv_to_rgb for one Y block at (xx,yy) of the 16x16 macroblock; rgb is 16*16*3 signed bytes (R,G,B).
void YuvToRgb(const int16_t cr[64], const int16_t cb[64], const int16_t y[64], int xx, int yy, int8_t rgb[768]);

/// y_to_mono for one sample: clip to signed 9 bit, saturate to -128..127 (signed result).
int8_t YToMono(int16_t y);

} // namespace mdec

// ───────────────────────────────────────────────────────────────────────────
// MdecFrame — Decoded Video Frame Buffer (32-bit RGBA)
// ───────────────────────────────────────────────────────────────────────────

struct MdecFrame {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint32_t> pixels_rgba; // width * height pixels in 0xAABBGGRR format
};

// ───────────────────────────────────────────────────────────────────────────
// MdecDecoder — whole-frame software path: MDEC run-length stream -> RGBA frame
// using the default quant/scale tables (what the game uploads for STR playback).
// ───────────────────────────────────────────────────────────────────────────

class MdecDecoder {
public:
    MdecDecoder();
    ~MdecDecoder() = default;

    void Reset();

    /// Decodes an MDEC run-length stream (little-endian halfwords, as sent with MDEC(1);
    /// produce it from an STR/BS frame with bs::ToMdec) into an RGBA frame. Macroblocks are
    /// placed column by column (top to bottom, then left to right) as in STR frames.
    /// An empty stream yields a neutral gray frame. Returns true if decoding succeeded.
    bool DecodeFrame(const uint8_t* bitstream_data, size_t bitstream_size,
                     uint16_t width, uint16_t height,
                     MdecFrame& out_frame);

    /// Float reference 8x8 IDCT (not the hardware algorithm; see mdec::Idct).
    static void PerformIdct8x8(const int16_t in_coeff[64], uint8_t out_pixels[64]);

    /// Float CCIR 601 YCbCr (0..255) to RGBA (not the hardware algorithm; see mdec::YuvToRgb).
    static uint32_t YCbCrToRgba(int y, int cb, int cr);

    uint32_t GetDecodedFrameCount() const { return m_decoded_frames; }

private:
    uint32_t m_decoded_frames = 0;
};

} // namespace personarm
