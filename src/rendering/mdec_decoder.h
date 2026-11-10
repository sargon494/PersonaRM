#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// MdecFrame — Decoded Video Frame Buffer (32-bit RGBA)
// ───────────────────────────────────────────────────────────────────────────

struct MdecFrame {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint32_t> pixels_rgba; // width * height pixels in 0xAABBGGRR format
};

// ───────────────────────────────────────────────────────────────────────────
// MdecDecoder — PlayStation 1 Hardware Macroblock Decoder Emulation
// Responsible for MDEC IDCT 8x8 decoding, YCbCr->RGB color space conversion,
// and 16x16 macroblock frame assembly.
// ───────────────────────────────────────────────────────────────────────────

class MdecDecoder {
public:
    MdecDecoder();
    ~MdecDecoder() = default;

    void Reset();

    /// Decodes an MDEC compressed bitstream or chunk payload into an RGBA frame.
    /// Returns true if decoding succeeded.
    bool DecodeFrame(const uint8_t* bitstream_data, size_t bitstream_size,
                     uint16_t width, uint16_t height,
                     MdecFrame& out_frame);

    /// Decodes a single 8x8 block using 2D Inverse Discrete Cosine Transform (IDCT).
    /// in_coeff: 64 frequency coefficients (DC at [0], AC at [1..63])
    /// out_pixels: 64 output spatial luminance/chrominance values clamped [0..255]
    static void PerformIdct8x8(const int16_t in_coeff[64], uint8_t out_pixels[64]);

    /// Converts YCbCr (Y: 0..255, Cb: 0..255, Cr: 0..255) to 32-bit RGBA
    static uint32_t YCbCrToRgba(int y, int cb, int cr);

    uint32_t GetDecodedFrameCount() const { return m_decoded_frames; }

private:
    uint32_t m_decoded_frames = 0;
};

} // namespace personarm
