#include "rendering/ps1_gpu.h"
#include "core/logger.h"
#include <cstring>
#include <algorithm>

namespace personarm {

Ps1Gpu::Ps1Gpu() {
    m_vram.resize(PS1_VRAM_WORDS, 0);
    Reset();
}

void Ps1Gpu::Reset() {
    std::fill(m_vram.begin(), m_vram.end(), 0);

    m_display.x = 0;
    m_display.y = 0;
    m_display.width = 320;
    m_display.height = 240;
    m_display.is_24bit = false;
    m_display.is_pal = false;
    m_display.is_interlaced = false;

    m_drawing.x1 = 0;
    m_drawing.y1 = 0;
    m_drawing.x2 = 319;
    m_drawing.y2 = 239;
    m_drawing.offset_x = 0;
    m_drawing.offset_y = 0;

    m_gpustat = 0x14802000;
    m_display_disabled = false;
    m_dma_direction = GpuDmaDirection::Off;

    m_gp0_packet.clear();
    m_gp0_words_remaining = 0;
    m_in_vram_transfer = false;
}

uint16_t Ps1Gpu::ReadVram(uint32_t x, uint32_t y) const {
    x &= (PS1_VRAM_WIDTH - 1);
    y &= (PS1_VRAM_HEIGHT - 1);
    return m_vram[y * PS1_VRAM_WIDTH + x];
}

void Ps1Gpu::WriteVram(uint32_t x, uint32_t y, uint16_t pixel) {
    x &= (PS1_VRAM_WIDTH - 1);
    y &= (PS1_VRAM_HEIGHT - 1);
    m_vram[y * PS1_VRAM_WIDTH + x] = pixel;
}

void Ps1Gpu::FillVramRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color16) {
    for (uint32_t cy = 0; cy < h; ++cy) {
        uint32_t py = (y + cy) & (PS1_VRAM_HEIGHT - 1);
        for (uint32_t cx = 0; cx < w; ++cx) {
            uint32_t px = (x + cx) & (PS1_VRAM_WIDTH - 1);
            m_vram[py * PS1_VRAM_WIDTH + px] = color16;
        }
    }
}

bool Ps1Gpu::LoadTimToVram(const TimImage& tim) {
    if (tim.pixels_rgba.empty() || tim.width == 0 || tim.height == 0) {
        return false;
    }

    // Convert RGBA8888 back to PS1 16-bit RGB 1:5:5:5 for direct VRAM storage
    uint32_t vram_x = tim.vram_x & (PS1_VRAM_WIDTH - 1);
    uint32_t vram_y = tim.vram_y & (PS1_VRAM_HEIGHT - 1);

    for (uint32_t y = 0; y < tim.height; ++y) {
        uint32_t py = (vram_y + y) & (PS1_VRAM_HEIGHT - 1);
        for (uint32_t x = 0; x < tim.width; ++x) {
            uint32_t px = (vram_x + x) & (PS1_VRAM_WIDTH - 1);
            const auto& c = tim.pixels_rgba[y * tim.width + x];

            uint16_t r5 = (c.r >> 3) & 0x1F;
            uint16_t g5 = (c.g >> 3) & 0x1F;
            uint16_t b5 = (c.b >> 3) & 0x1F;
            uint16_t stp = (c.a > 0 && c.a < 255) ? 0x8000 : 0;

            uint16_t ps1_pixel = (b5 << 10) | (g5 << 5) | r5 | stp;
            m_vram[py * PS1_VRAM_WIDTH + px] = ps1_pixel;
        }
    }

    PRM_LOG_INFO("GPU", "Loaded TIM texture into VRAM at (" + std::to_string(vram_x) +
                        "," + std::to_string(vram_y) + ") Size: " +
                        std::to_string(tim.width) + "x" + std::to_string(tim.height));
    return true;
}

void Ps1Gpu::WriteGP1(uint32_t word) {
    ExecuteGP1Command(word);
}

void Ps1Gpu::ExecuteGP1Command(uint32_t word) {
    uint8_t cmd = (word >> 24) & 0xFF;
    switch (cmd) {
        case 0x00: // Reset GPU
            Reset();
            break;

        case 0x01: // Reset Command Buffer
            m_gp0_packet.clear();
            m_gp0_words_remaining = 0;
            m_in_vram_transfer = false;
            break;

        case 0x02: // Acknowledge GPU Interrupt
            m_gpustat &= ~(1 << 24);
            break;

        case 0x03: // Display Enable
            m_display_disabled = (word & 1) != 0;
            if (m_display_disabled) m_gpustat |= (1 << 23);
            else m_gpustat &= ~(1 << 23);
            break;

        case 0x04: // DMA Direction
            m_dma_direction = static_cast<GpuDmaDirection>(word & 3);
            m_gpustat = (m_gpustat & ~(3 << 29)) | ((word & 3) << 29);
            break;

        case 0x05: // Start of Display Area in VRAM
            m_display.x = word & 0x3FF;
            m_display.y = (word >> 10) & 0x1FF;
            break;

        case 0x06: // Horizontal Display Range
            break;

        case 0x07: // Vertical Display Range
            break;

        case 0x08: { // Display Mode
            uint8_t hres1 = word & 0x03;
            bool vres = (word & 0x04) != 0;
            bool is_pal = (word & 0x08) != 0;
            bool is_24bit = (word & 0x10) != 0;
            bool is_interlaced = (word & 0x20) != 0;
            bool hres2 = (word & 0x40) != 0;

            if (hres2) {
                m_display.width = 368;
            } else {
                switch (hres1) {
                    case 0: m_display.width = 256; break;
                    case 1: m_display.width = 320; break;
                    case 2: m_display.width = 512; break;
                    case 3: m_display.width = 640; break;
                }
            }

            m_display.height = vres ? 480 : 240;
            m_display.is_pal = is_pal;
            m_display.is_24bit = is_24bit;
            m_display.is_interlaced = is_interlaced;
            break;
        }

        default:
            break;
    }
}

void Ps1Gpu::WriteGP0(uint32_t word) {
    if (m_in_vram_transfer) {
        // CPU to VRAM pixel transfer: word contains two 16-bit pixels
        uint16_t p0 = word & 0xFFFF;
        uint16_t p1 = (word >> 16) & 0xFFFF;

        uint32_t px0 = (m_transfer_x + m_transfer_cur_x) & (PS1_VRAM_WIDTH - 1);
        uint32_t py0 = (m_transfer_y + m_transfer_cur_y) & (PS1_VRAM_HEIGHT - 1);
        m_vram[py0 * PS1_VRAM_WIDTH + px0] = p0;

        m_transfer_cur_x++;
        if (m_transfer_cur_x >= m_transfer_w) {
            m_transfer_cur_x = 0;
            m_transfer_cur_y++;
        }

        if (m_transfer_words_left > 1) {
            uint32_t px1 = (m_transfer_x + m_transfer_cur_x) & (PS1_VRAM_WIDTH - 1);
            uint32_t py1 = (m_transfer_y + m_transfer_cur_y) & (PS1_VRAM_HEIGHT - 1);
            m_vram[py1 * PS1_VRAM_WIDTH + px1] = p1;

            m_transfer_cur_x++;
            if (m_transfer_cur_x >= m_transfer_w) {
                m_transfer_cur_x = 0;
                m_transfer_cur_y++;
            }
        }

        m_transfer_words_left--;
        if (m_transfer_words_left == 0) {
            m_in_vram_transfer = false;
        }
        return;
    }

    if (m_gp0_words_remaining == 0) {
        // First word of command
        uint8_t cmd = (word >> 24) & 0xFF;
        m_gp0_packet.clear();
        m_gp0_packet.push_back(word);

        // Determine packet size based on command opcode
        switch (cmd) {
            case 0x00: // NOP
                m_gp0_words_remaining = 0;
                break;
            case 0x01: // Clear Cache
                m_gp0_words_remaining = 0;
                break;
            case 0x02: // Quick Fill Rect
                m_gp0_words_remaining = 2; // (color already in word), x+y, w+h
                break;
            case 0x20: case 0x21: case 0x22: case 0x23: // Mono 3-pt poly
                m_gp0_words_remaining = 3; // v0, v1, v2
                break;
            case 0x24: case 0x25: case 0x26: case 0x27: // Textured 3-pt poly
                m_gp0_words_remaining = 6; // v0+uv0, v1+uv1, v2+uv2
                break;
            case 0x28: case 0x29: case 0x2A: case 0x2B: // Mono 4-pt poly
                m_gp0_words_remaining = 4; // v0, v1, v2, v3
                break;
            case 0x2C: case 0x2D: case 0x2E: case 0x2F: // Textured 4-pt poly
                m_gp0_words_remaining = 8;
                break;
            case 0x30: case 0x32: // Gouraud 3-pt poly
                m_gp0_words_remaining = 5;
                break;
            case 0x38: case 0x3A: // Gouraud 4-pt poly
                m_gp0_words_remaining = 7;
                break;
            case 0x60: case 0x62: // Free-size Rectangle
                m_gp0_words_remaining = 2; // x+y, w+h
                break;
            case 0x64: case 0x66: // Textured Free-size Rect
                m_gp0_words_remaining = 3; // x+y, uv+clut, w+h
                break;
            case 0x68: case 0x6A: // 1x1 Dot
                m_gp0_words_remaining = 1; // x+y
                break;
            case 0x70: case 0x72: // 8x8 Rect
                m_gp0_words_remaining = 1; // x+y
                break;
            case 0x78: case 0x7A: // 16x16 Rect
                m_gp0_words_remaining = 1; // x+y
                break;
            case 0x80: // VRAM-to-VRAM Blit
                m_gp0_words_remaining = 3; // src_x_y, dst_x_y, w_h
                break;
            case 0xA0: // CPU-to-VRAM Blit
                m_gp0_words_remaining = 2; // dst_x_y, w_h
                break;
            case 0xC0: // VRAM-to-CPU Read
                m_gp0_words_remaining = 2;
                break;
            case 0xE1: // Draw Mode
            case 0xE2: // Texture Window
            case 0xE3: // Set Drawing Area Top-Left
            case 0xE4: // Set Drawing Area Bottom-Right
            case 0xE5: // Set Drawing Offset
            case 0xE6: // Mask Bit
                m_gp0_words_remaining = 0;
                ExecuteGP0Command();
                break;
            default:
                m_gp0_words_remaining = 0;
                break;
        }
    } else {
        m_gp0_packet.push_back(word);
        m_gp0_words_remaining--;

        if (m_gp0_words_remaining == 0) {
            ExecuteGP0Command();
        }
    }
}

void Ps1Gpu::ExecuteGP0Command() {
    if (m_gp0_packet.empty()) return;
    uint32_t w0 = m_gp0_packet[0];
    uint8_t cmd = (w0 >> 24) & 0xFF;

    switch (cmd) {
        case 0x02: { // Quick Fill VRAM Rect
            if (m_gp0_packet.size() < 3) return;
            uint16_t r5 = (w0 >> 3) & 0x1F;
            uint16_t g5 = (w0 >> 11) & 0x1F;
            uint16_t b5 = (w0 >> 19) & 0x1F;
            uint16_t color16 = (b5 << 10) | (g5 << 5) | r5;

            uint16_t x = m_gp0_packet[1] & 0x3FF;
            uint16_t y = (m_gp0_packet[1] >> 16) & 0x1FF;
            uint16_t w = m_gp0_packet[2] & 0x3FF;
            uint16_t h = (m_gp0_packet[2] >> 16) & 0x1FF;

            // PS1 hardware rounds width up to 16-pixel multiples
            w = ((w + 0x0F) & ~0x0F);
            FillVramRect(x, y, w, h, color16);
            break;
        }

        case 0xA0: { // CPU-to-VRAM Blit Setup
            if (m_gp0_packet.size() < 3) return;
            m_transfer_x = m_gp0_packet[1] & 0x3FF;
            m_transfer_y = (m_gp0_packet[1] >> 16) & 0x1FF;
            m_transfer_w = m_gp0_packet[2] & 0x3FF;
            m_transfer_h = (m_gp0_packet[2] >> 16) & 0x1FF;

            m_transfer_cur_x = 0;
            m_transfer_cur_y = 0;
            // Two 16-bit pixels per 32-bit word
            m_transfer_words_left = ((m_transfer_w * m_transfer_h) + 1) / 2;

            if (m_transfer_words_left > 0) {
                m_in_vram_transfer = true;
            }
            break;
        }

        case 0x80: { // VRAM-to-VRAM Blit
            if (m_gp0_packet.size() < 4) return;
            uint16_t sx = m_gp0_packet[1] & 0x3FF;
            uint16_t sy = (m_gp0_packet[1] >> 16) & 0x1FF;
            uint16_t dx = m_gp0_packet[2] & 0x3FF;
            uint16_t dy = (m_gp0_packet[2] >> 16) & 0x1FF;
            uint16_t w  = m_gp0_packet[3] & 0x3FF;
            uint16_t h  = (m_gp0_packet[3] >> 16) & 0x1FF;

            for (uint32_t cy = 0; cy < h; ++cy) {
                for (uint32_t cx = 0; cx < w; ++cx) {
                    uint16_t p = ReadVram(sx + cx, sy + cy);
                    WriteVram(dx + cx, dy + cy, p);
                }
            }
            break;
        }

        case 0xE3: // Set Drawing Area Top-Left
            m_drawing.x1 = w0 & 0x3FF;
            m_drawing.y1 = (w0 >> 10) & 0x1FF;
            break;

        case 0xE4: // Set Drawing Area Bottom-Right
            m_drawing.x2 = w0 & 0x3FF;
            m_drawing.y2 = (w0 >> 10) & 0x1FF;
            break;

        case 0xE5: // Set Drawing Offset
            m_drawing.offset_x = static_cast<int32_t>((w0 & 0x7FF) << 21) >> 21; // sign extend 11-bit
            m_drawing.offset_y = static_cast<int32_t>(((w0 >> 11) & 0x7FF) << 21) >> 21;
            break;

        default:
            break;
    }
}

uint32_t Ps1Gpu::ReadGP0() {
    return 0;
}

uint32_t Ps1Gpu::ReadGPUSTAT() const {
    uint32_t stat = m_gpustat;
    stat |= (1 << 26); // Ready to receive command word
    stat |= (1 << 27); // Ready to send VRAM to CPU
    stat |= (1 << 28); // Ready to receive DMA block
    return stat;
}

void Ps1Gpu::OutputFrameToRGBA(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) const {
    out_w = m_display.width > 0 ? m_display.width : 320;
    out_h = m_display.height > 0 ? m_display.height : 240;

    size_t total_pixels = static_cast<size_t>(out_w) * out_h;
    out_rgba.resize(total_pixels);

    if (m_display_disabled) {
        std::fill(out_rgba.begin(), out_rgba.end(), 0xFF000000); // Black screen
        return;
    }

    uint32_t base_x = m_display.x & (PS1_VRAM_WIDTH - 1);
    uint32_t base_y = m_display.y & (PS1_VRAM_HEIGHT - 1);

    for (uint32_t y = 0; y < out_h; ++y) {
        uint32_t vram_y = (base_y + y) & (PS1_VRAM_HEIGHT - 1);
        for (uint32_t x = 0; x < out_w; ++x) {
            uint32_t vram_x = (base_x + x) & (PS1_VRAM_WIDTH - 1);

            uint16_t p = m_vram[vram_y * PS1_VRAM_WIDTH + vram_x];
            uint8_t r = ((p >> 0) & 0x1F) << 3;
            uint8_t g = ((p >> 5) & 0x1F) << 3;
            uint8_t b = ((p >> 10) & 0x1F) << 3;
            uint8_t a = 255;

            // Pack to 0xAABBGGRR / 0xAARRGGBB depending on endianness
            out_rgba[y * out_w + x] = (static_cast<uint32_t>(a) << 24) |
                                      (static_cast<uint32_t>(b) << 16) |
                                      (static_cast<uint32_t>(g) << 8)  |
                                      (static_cast<uint32_t>(r));
        }
    }
}

} // namespace personarm

