#include "rendering/mdec_decoder.h"
#include "core/logger.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace personarm {

namespace {

// Precalculated 8x8 1D IDCT basis matrix:
// T[u][x] = C(u) * cos((2x + 1) * u * pi / 16) / 2
// where C(0) = 1 / sqrt(2), C(u) = 1 for u > 0
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

    // 2D IDCT using separable 1D row-column transformation
    double temp[8][8];

    // 1. Column transform: for each column x
    for (int y = 0; y < 8; ++y) {
        for (int v = 0; v < 8; ++v) {
            double sum = 0.0;
            for (int u = 0; u < 8; ++u) {
                sum += s_idct_basis[u][y] * in_coeff[v * 8 + u];
            }
            temp[v][y] = sum;
        }
    }

    // 2. Row transform: for each row y
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (int v = 0; v < 8; ++v) {
                sum += s_idct_basis[v][x] * temp[v][y];
            }
            // Add level shift +128 for 8-bit unsigned spatial output
            int val = static_cast<int>(std::round(sum + 128.0));
            out_pixels[y * 8 + x] = static_cast<uint8_t>(std::clamp(val, 0, 255));
        }
    }
}

uint32_t MdecDecoder::YCbCrToRgba(int y, int cb, int cr) {
    // PS1 MDEC / CCIR 601 Color Matrix Transformation
    // R = Y + 1.402 * (Cr - 128)
    // G = Y - 0.344136 * (Cb - 128) - 0.714136 * (Cr - 128)
    // B = Y + 1.772 * (Cb - 128)

    double dy = static_cast<double>(y);
    double dcb = static_cast<double>(cb - 128);
    double dcr = static_cast<double>(cr - 128);

    int r = static_cast<int>(std::round(dy + 1.402000 * dcr));
    int g = static_cast<int>(std::round(dy - 0.344136 * dcb - 0.714136 * dcr));
    int b = static_cast<int>(std::round(dy + 1.772000 * dcb));

    uint32_t ur = static_cast<uint32_t>(std::clamp(r, 0, 255));
    uint32_t ug = static_cast<uint32_t>(std::clamp(g, 0, 255));
    uint32_t ub = static_cast<uint32_t>(std::clamp(b, 0, 255));
    uint32_t ua = 0xFF; // Fully opaque

    // Format: 0xAABBGGRR
    return (ua << 24) | (ub << 16) | (ug << 8) | ur;
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
    out_frame.pixels_rgba.resize(static_cast<size_t>(width) * height);

    // Number of 16x16 macroblocks
    uint16_t mb_width = (width + 15) / 16;
    uint16_t mb_height = (height + 15) / 16;

    // Default neutral gray frame if bitstream is empty
    if (!bitstream_data || bitstream_size < 4) {
        uint32_t default_pixel = YCbCrToRgba(128, 128, 128);
        std::fill(out_frame.pixels_rgba.begin(), out_frame.pixels_rgba.end(), default_pixel);
        m_decoded_frames++;
        return true;
    }

    // Macroblock buffers (4:2:0 format: 6 blocks of 8x8 each)
    // block 0: Cr (8x8)
    // block 1: Cb (8x8)
    // block 2: Y0 (top-left 8x8)
    // block 3: Y1 (top-right 8x8)
    // block 4: Y2 (bottom-left 8x8)
    // block 5: Y3 (bottom-right 8x8)
    uint8_t cr_pixels[64];
    uint8_t cb_pixels[64];
    uint8_t y_pixels[4][64];

    size_t bit_ptr = 0;
    const uint16_t* words = reinterpret_cast<const uint16_t*>(bitstream_data);
    size_t total_words = bitstream_size / 2;

    for (uint16_t mby = 0; mby < mb_height; ++mby) {
        for (uint16_t mbx = 0; mbx < mb_width; ++mbx) {
            // Decode 6 blocks for each macroblock
            for (int block_idx = 0; block_idx < 6; ++block_idx) {
                int16_t coeff[64] = {0};

                // Decode run-length pairs or raw coefficients
                int coeff_pos = 0;
                while (coeff_pos < 64 && bit_ptr < total_words) {
                    uint16_t word = words[bit_ptr++];
                    if (word == 0xFE00) {
                        // End of block marker
                        break;
                    }
                    uint16_t run = (word >> 10) & 0x3F;
                    int16_t val = static_cast<int16_t>((word & 0x03FF) << 6) >> 6; // Sign extend 10-bit

                    coeff_pos += run;
                    if (coeff_pos < 64) {
                        coeff[coeff_pos++] = val;
                    }
                }

                // IDCT into destination block
                if (block_idx == 0) {
                    PerformIdct8x8(coeff, cr_pixels);
                } else if (block_idx == 1) {
                    PerformIdct8x8(coeff, cb_pixels);
                } else {
                    PerformIdct8x8(coeff, y_pixels[block_idx - 2]);
                }
            }

            // Assemble 16x16 pixels into output RGBA frame
            for (int py = 0; py < 16; ++py) {
                int dst_y = mby * 16 + py;
                if (dst_y >= height) continue;

                for (int px = 0; px < 16; ++px) {
                    int dst_x = mbx * 16 + px;
                    if (dst_x >= width) continue;

                    // Chroma sub-sampling: 8x8 shared across 16x16 (px/2, py/2)
                    int cx = px / 2;
                    int cy = py / 2;
                    uint8_t cr = cr_pixels[cy * 8 + cx];
                    uint8_t cb = cb_pixels[cy * 8 + cx];

                    // Luminance quad selection: (0: TL, 1: TR, 2: BL, 3: BR)
                    int sub_idx = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
                    int lx = px % 8;
                    int ly = py % 8;
                    uint8_t y = y_pixels[sub_idx][ly * 8 + lx];

                    uint32_t rgba = YCbCrToRgba(y, cb, cr);
                    out_frame.pixels_rgba[dst_y * width + dst_x] = rgba;
                }
            }
        }
    }

    m_decoded_frames++;
    return true;
}

} // namespace personarm
