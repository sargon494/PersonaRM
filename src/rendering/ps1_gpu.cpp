#include "rendering/ps1_gpu.h"
#include "core/logger.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

namespace personarm {

Ps1Gpu::Ps1Gpu() {
    m_vram.resize(PS1_VRAM_WORDS, 0);
    Reset();
}

void Ps1Gpu::Reset() {
    std::fill(m_vram.begin(), m_vram.end(), static_cast<uint16_t>(0));

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

    m_texpage = 0;
    m_texwindow = 0;
    m_mask_set = false;
    m_mask_check = false;
    m_in_polyline = false;
    m_read_words_left = 0;
    m_gpuread_latch = 0;
    UpdateStatDrawBits();
}

// GPUSTAT bits 0-10 mirror E1h bits 0-10, bit 11/12 mirror E6h, bit 15 mirrors E1h bit 11
// (psx-spx "GPU Status Register").
void Ps1Gpu::UpdateStatDrawBits() {
    m_gpustat &= ~0x00009FFFu;
    m_gpustat |= (m_texpage & 0x7FF);
    m_gpustat |= (m_mask_set ? 1u : 0u) << 11;
    m_gpustat |= (m_mask_check ? 1u : 0u) << 12;
    m_gpustat |= ((m_texpage >> 11) & 1u) << 15;
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
            m_in_polyline = false;
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

        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            // Get GPU Info (psx-spx "GPU Status Register / GP1(10h)"): latched into GPUREAD; indices
            // 0-1, 6, 9-F leave the previous value. Index bits 0-3 (new 208-pin GPU).
            switch (word & 0xF) {
                case 2: m_gpuread_latch = m_texwindow; break;
                case 3: m_gpuread_latch = uint32_t(m_drawing.x1 | (m_drawing.y1 << 10)); break;
                case 4: m_gpuread_latch = uint32_t(m_drawing.x2 | (m_drawing.y2 << 10)); break;
                case 5: m_gpuread_latch = (uint32_t(m_drawing.offset_x) & 0x7FF) | ((uint32_t(m_drawing.offset_y) & 0x7FF) << 11); break;
                case 7: m_gpuread_latch = 2; break;  // GPU type
                case 8: m_gpuread_latch = 0; break;
                default: break;
            }
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

        if (m_transfer_cur_y < m_transfer_h) { // the high half is padding only for odd pixel counts
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

    if (m_in_polyline) {
        // psx-spx "GPU Render Line Commands": polylines run until a 5xxx5xxxh terminator word.
        // unverified: terminator is only tested where a new vertex (or its gouraud color) starts.
        if ((word & 0xF000F000u) == 0x50005000u && !(m_polyline_gouraud && m_polyline_have_color)) {
            m_in_polyline = false;
            return;
        }
        if (m_polyline_gouraud && !m_polyline_have_color) {
            m_polyline_pending_color = word;
            m_polyline_have_color = true;
            return;
        }
        Vertex next = ParseVertex(word, m_polyline_pending_color);
        DrawLine(m_polyline_last, next, m_polyline_gouraud, m_polyline_semi);
        m_polyline_last = next;
        m_polyline_have_color = false;
        return;
    }

    if (m_gp0_words_remaining == 0) {
        // First word of command
        uint8_t cmd = (word >> 24) & 0xFF;
        m_gp0_packet.clear();
        m_gp0_packet.push_back(word);
        m_gp0_words_remaining = Gp0CommandWords(cmd) - 1;
        if (m_gp0_words_remaining == 0) {
            ExecuteGP0Command();
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

    if (cmd >= 0x20 && cmd <= 0x3F) { DrawPolygon(cmd); return; }
    if (cmd >= 0x40 && cmd <= 0x5F) { DrawLineCommand(cmd); return; }
    if (cmd >= 0x60 && cmd <= 0x7F) { DrawRectangle(cmd); return; }
    // psx-spx "GPU Memory Transfer Commands": 81h-9Fh, A1h-BFh, C1h-DFh mirror 80h/A0h/C0h.
    if (cmd >= 0x80 && cmd <= 0xDF) cmd &= 0xE0;

    switch (cmd) {
        case 0x1F: // Interrupt Request (psx-spx "GPU Other Commands")
            m_gpustat |= (1u << 24);
            break;

        case 0xC0: { // VRAM-to-CPU Read setup (psx-spx "GPU Memory Transfer Commands")
            if (m_gp0_packet.size() < 3) return;
            m_read_x = m_gp0_packet[1] & 0x3FF;
            m_read_y = (m_gp0_packet[1] >> 16) & 0x1FF;
            // Size 0 means maximum: ((n-1) AND mask)+1.
            m_read_w = ((((m_gp0_packet[2] & 0xFFFF) - 1) & 0x3FF) + 1);
            m_read_h = (((((m_gp0_packet[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1);
            m_read_cur_x = 0;
            m_read_cur_y = 0;
            m_read_words_left = (m_read_w * m_read_h + 1) / 2;
            break;
        }

        case 0xE1: // Draw Mode setting (psx-spx "GPU Rendering Attributes")
            m_texpage = w0 & 0x3FFF;
            UpdateStatDrawBits();
            break;

        case 0xE2: // Texture Window setting
            m_texwindow = w0 & 0xFFFFF;
            break;

        case 0xE6: // Mask Bit setting
            m_mask_set = (w0 & 1) != 0;
            m_mask_check = (w0 & 2) != 0;
            UpdateStatDrawBits();
            break;

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

// GPUREAD: during a C0h transfer returns two pixels per word (first pixel in the low half);
// otherwise returns the last latched value (psx-spx "GPU Memory Transfer Commands").
uint32_t Ps1Gpu::ReadGP0() {
    if (m_read_words_left == 0) return m_gpuread_latch;
    uint32_t halves[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        if (m_read_cur_y >= m_read_h) break; // unverified: padding half of an odd-sized read is 0
        halves[i] = ReadVram(m_read_x + m_read_cur_x, m_read_y + m_read_cur_y);
        if (++m_read_cur_x >= m_read_w) {
            m_read_cur_x = 0;
            ++m_read_cur_y;
        }
    }
    m_gpuread_latch = halves[0] | (halves[1] << 16);
    --m_read_words_left;
    return m_gpuread_latch;
}

uint32_t Ps1Gpu::ReadGPUSTAT() const {
    uint32_t stat = m_gpustat;
    stat |= (1 << 26); // Ready to receive command word
    if (m_read_words_left > 0) stat |= (1 << 27); // Ready to send VRAM to CPU
    stat |= (1 << 28); // Ready to receive DMA block
    return stat;
}

// ───────────────────────────────────────────────────────────────────────────
// Rasterizer (psx-spx "GPU Render Polygon/Line/Rectangle Commands")
// ───────────────────────────────────────────────────────────────────────────

// Total packet length in words (including the command word) for fixed-size GP0 commands.
// Polylines report only their first segment; the rest streams through WriteGP0.
uint32_t Ps1Gpu::Gp0CommandWords(uint8_t cmd) {
    if (cmd == 0x02) return 3;
    if (cmd >= 0x20 && cmd <= 0x3F) {
        uint32_t n = (cmd & 0x08) ? 4 : 3;
        uint32_t per_vertex = (cmd & 0x04) ? 2 : 1;
        uint32_t colors = (cmd & 0x10) ? n - 1 : 0;
        return 1 + n * per_vertex + colors;
    }
    if (cmd >= 0x40 && cmd <= 0x5F) return (cmd & 0x10) ? 4 : 3;
    if (cmd >= 0x60 && cmd <= 0x7F) {
        return 2 + ((cmd & 0x04) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
    }
    if (cmd >= 0x80 && cmd <= 0x9F) return 4;
    if (cmd >= 0xA0 && cmd <= 0xDF) return 3;
    return 1;
}

static int32_t SignExtend11(uint32_t v) {
    return static_cast<int32_t>((v & 0x7FF) << 21) >> 21;
}

Ps1Gpu::Vertex Ps1Gpu::ParseVertex(uint32_t xy_word, uint32_t color_word) const {
    Vertex vtx;
    // psx-spx "GPU Render Polygon Commands": vertices are signed 11-bit, plus drawing offset (E5h).
    vtx.x = SignExtend11(xy_word) + m_drawing.offset_x;
    vtx.y = SignExtend11(xy_word >> 16) + m_drawing.offset_y;
    vtx.r = color_word & 0xFF;
    vtx.g = (color_word >> 8) & 0xFF;
    vtx.b = (color_word >> 16) & 0xFF;
    return vtx;
}

void Ps1Gpu::DrawPolygon(uint8_t cmd) {
    ++m_polygon_count;
    const bool gouraud = (cmd & 0x10) != 0;
    const bool quad = (cmd & 0x08) != 0;
    DrawFlags f;
    f.textured = (cmd & 0x04) != 0;
    f.semi = (cmd & 0x02) != 0;
    f.raw = f.textured && (cmd & 0x01) != 0;

    const int n = quad ? 4 : 3;
    Vertex v[4];
    size_t idx = 1;
    uint32_t color = m_gp0_packet[0];
    for (int i = 0; i < n; ++i) {
        if (gouraud && i > 0) color = m_gp0_packet[idx++];
        v[i] = ParseVertex(m_gp0_packet[idx++], color);
        if (f.textured) {
            uint32_t uvw = m_gp0_packet[idx++];
            v[i].u = uvw & 0xFF;
            v[i].v = (uvw >> 8) & 0xFF;
            if (i == 0) f.clut = static_cast<uint16_t>(uvw >> 16);
            // psx-spx "Texpage Attribute": the polygon's texpage overwrites E1h bits 0-8 and 11.
            if (i == 1) {
                m_texpage = (m_texpage & ~0x9FFu) | ((uvw >> 16) & 0x9FF);
                UpdateStatDrawBits();
            }
        }
    }
    // psx-spx "GPU Rendering Attributes" (E1h bit 9): dithering applies to gouraud-shaded
    // and texture-blended (modulated) primitives only.
    f.dither = ((m_texpage >> 9) & 1) && (gouraud || (f.textured && !f.raw));

    // psx-spx "GPU Render Polygon Commands": a quad is drawn as triangles (v0,v1,v2) and (v1,v2,v3).
    DrawTriangle(v[0], v[1], v[2], gouraud, f);
    if (quad) DrawTriangle(v[1], v[2], v[3], gouraud, f);
}

void Ps1Gpu::DrawTriangle(const Vertex& a, const Vertex& b_in, const Vertex& c_in,
                          bool gouraud, const DrawFlags& f) {
    // psx-spx: max vertex distance is 1023 horizontally / 511 vertically; larger polygons are skipped.
    int32_t min_x = std::min({a.x, b_in.x, c_in.x}), max_x = std::max({a.x, b_in.x, c_in.x});
    int32_t min_y = std::min({a.y, b_in.y, c_in.y}), max_y = std::max({a.y, b_in.y, c_in.y});
    if (max_x - min_x > 1023 || max_y - min_y > 511) return;

    auto edge = [](const Vertex& p, const Vertex& q, int64_t x, int64_t y) -> int64_t {
        return static_cast<int64_t>(q.x - p.x) * (y - p.y) - static_cast<int64_t>(q.y - p.y) * (x - p.x);
    };
    Vertex b = b_in, c = c_in;
    int64_t area = edge(a, b, c.x, c.y);
    if (area == 0) return;
    if (area < 0) { std::swap(b, c); area = -area; }

    // Top-left fill rule with samples at integer coordinates: the right and bottom edges are
    // excluded (psx-spx "GPU Render Polygon Commands": polygons exclude their lower-right edges).
    auto top_left = [](const Vertex& p, const Vertex& q) {
        int32_t dy = q.y - p.y, dx = q.x - p.x;
        return dy < 0 || (dy == 0 && dx > 0);
    };
    const int64_t bias0 = top_left(b, c) ? 0 : -1;
    const int64_t bias1 = top_left(c, a) ? 0 : -1;
    const int64_t bias2 = top_left(a, b) ? 0 : -1;

    // Linear interpolation of an attribute by barycentric weights, rounded to nearest.
    // approximate: hardware uses per-span fixed-point stepping, interior values may differ by 1.
    auto lerp = [area](int64_t w0, int64_t w1, int64_t w2, int32_t v0, int32_t v1, int32_t v2) {
        int64_t s = w0 * v0 + w1 * v1 + w2 * v2;
        return static_cast<int32_t>((2 * s + area) / (2 * area));
    };

    const int32_t x0 = std::max(min_x, m_drawing.x1), x1 = std::min(max_x, m_drawing.x2);
    const int32_t y0 = std::max(min_y, m_drawing.y1), y1 = std::min(max_y, m_drawing.y2);
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x) {
            int64_t w0 = edge(b, c, x, y), w1 = edge(c, a, x, y), w2 = edge(a, b, x, y);
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
            int32_t r = a.r, g = a.g, bl = a.b;
            if (gouraud) {
                r = lerp(w0, w1, w2, a.r, b.r, c.r);
                g = lerp(w0, w1, w2, a.g, b.g, c.g);
                bl = lerp(w0, w1, w2, a.b, b.b, c.b);
            }
            int32_t u = 0, v = 0;
            if (f.textured) {
                u = lerp(w0, w1, w2, a.u, b.u, c.u);
                v = lerp(w0, w1, w2, a.v, b.v, c.v);
            }
            PlotPixel(x, y, r, g, bl, u, v, f);
        }
    }
}

void Ps1Gpu::DrawLineCommand(uint8_t cmd) {
    const bool gouraud = (cmd & 0x10) != 0;
    const bool semi = (cmd & 0x02) != 0;
    const uint32_t c0 = m_gp0_packet[0];
    Vertex a = ParseVertex(m_gp0_packet[1], c0);
    Vertex b = gouraud ? ParseVertex(m_gp0_packet[3], m_gp0_packet[2]) : ParseVertex(m_gp0_packet[2], c0);
    DrawLine(a, b, gouraud, semi);

    if (cmd & 0x08) { // polyline: keep streaming vertices until the terminator
        m_in_polyline = true;
        m_polyline_gouraud = gouraud;
        m_polyline_semi = semi;
        m_polyline_last = b;
        m_polyline_pending_color = c0;
        m_polyline_have_color = false;
    }
}

void Ps1Gpu::DrawLine(const Vertex& a, const Vertex& b, bool gouraud, bool semi) {
    const int32_t dx = b.x - a.x, dy = b.y - a.y;
    // psx-spx "GPU Render Line Commands": lines longer than 1023/511 are not drawn.
    if (std::abs(dx) > 1023 || std::abs(dy) > 511) return;

    DrawFlags f;
    f.semi = semi;
    f.dither = ((m_texpage >> 9) & 1) && gouraud;

    // approximate: DDA with round-to-nearest; both endpoints drawn.
    // unverified: hardware line stepping and end-point inclusion.
    const int32_t steps = std::max(std::abs(dx), std::abs(dy));
    auto step = [steps](int32_t from, int32_t to, int32_t i) {
        if (steps == 0) return from;
        int32_t d = to - from;
        int32_t num = 2 * d * i + (d >= 0 ? steps : -steps);
        return from + num / (2 * steps);
    };
    for (int32_t i = 0; i <= steps; ++i) {
        int32_t x = step(a.x, b.x, i), y = step(a.y, b.y, i);
        if (x < m_drawing.x1 || x > m_drawing.x2 || y < m_drawing.y1 || y > m_drawing.y2) continue;
        int32_t r = a.r, g = a.g, bl = a.b;
        if (gouraud) {
            r = step(a.r, b.r, i);
            g = step(a.g, b.g, i);
            bl = step(a.b, b.b, i);
        }
        PlotPixel(x, y, r, g, bl, 0, 0, f);
    }
}

void Ps1Gpu::DrawRectangle(uint8_t cmd) {
    DrawFlags f;
    f.textured = (cmd & 0x04) != 0;
    f.semi = (cmd & 0x02) != 0;
    f.raw = f.textured && (cmd & 0x01) != 0;
    f.dither = false; // psx-spx "GPU Rendering Attributes": rectangles are never dithered

    const uint32_t c0 = m_gp0_packet[0];
    Vertex o = ParseVertex(m_gp0_packet[1], c0);
    size_t idx = 2;
    int32_t u0 = 0, v0 = 0;
    if (f.textured) {
        uint32_t uvw = m_gp0_packet[idx++];
        u0 = uvw & 0xFF;
        v0 = (uvw >> 8) & 0xFF;
        f.clut = static_cast<uint16_t>(uvw >> 16);
    }
    int32_t w = 0, h = 0;
    switch ((cmd >> 3) & 3) {
        case 0: w = m_gp0_packet[idx] & 0x3FF; h = (m_gp0_packet[idx] >> 16) & 0x1FF; break;
        case 1: w = h = 1; break;
        case 2: w = h = 8; break;
        case 3: w = h = 16; break;
    }
    // E1h bits 12/13: textured rectangle X/Y flip. unverified: flipped texcoords simply decrement from u0/v0.
    const bool flip_x = (m_texpage >> 12) & 1, flip_y = (m_texpage >> 13) & 1;
    for (int32_t dy = 0; dy < h; ++dy) {
        int32_t y = o.y + dy;
        if (y < m_drawing.y1 || y > m_drawing.y2) continue;
        for (int32_t dx = 0; dx < w; ++dx) {
            int32_t x = o.x + dx;
            if (x < m_drawing.x1 || x > m_drawing.x2) continue;
            int32_t u = flip_x ? u0 - dx : u0 + dx;
            int32_t v = flip_y ? v0 - dy : v0 + dy;
            PlotPixel(x, y, o.r, o.g, o.b, u, v, f);
        }
    }
}

// Texture lookup with texture window (psx-spx "GPU Texture Window", "GPU Texture Page/CLUT").
uint16_t Ps1Gpu::FetchTexel(int32_t u, int32_t v, uint16_t clut) const {
    const uint32_t mask_x = m_texwindow & 0x1F, mask_y = (m_texwindow >> 5) & 0x1F;
    const uint32_t off_x = (m_texwindow >> 10) & 0x1F, off_y = (m_texwindow >> 15) & 0x1F;
    uint32_t tu = static_cast<uint32_t>(u) & 0xFF, tv = static_cast<uint32_t>(v) & 0xFF;
    tu = (tu & ~(mask_x * 8)) | ((off_x & mask_x) * 8);
    tv = (tv & ~(mask_y * 8)) | ((off_y & mask_y) * 8);

    const uint32_t page_x = (m_texpage & 0xF) * 64;
    const uint32_t page_y = ((m_texpage >> 4) & 1) * 256;
    const uint32_t clut_x = (clut & 0x3F) * 16;
    const uint32_t clut_y = (clut >> 6) & 0x1FF;
    switch ((m_texpage >> 7) & 3) {
        case 0: { // 4-bit CLUT: four indices per halfword, lowest nibble first
            uint16_t word = ReadVram(page_x + tu / 4, page_y + tv);
            return ReadVram(clut_x + ((word >> ((tu & 3) * 4)) & 0xF), clut_y);
        }
        case 1: { // 8-bit CLUT: two indices per halfword, low byte first
            uint16_t word = ReadVram(page_x + tu / 2, page_y + tv);
            return ReadVram(clut_x + ((word >> ((tu & 1) * 8)) & 0xFF), clut_y);
        }
        default: // 15-bit direct (mode 3 "reserved" treated as 15-bit; unverified)
            return ReadVram(page_x + tu, page_y + tv);
    }
}

// Shade, blend and write one pixel already inside the drawing area
// (psx-spx "GPU Rendering Attributes", "GPU Semi-Transparency", "GPU Dithering", "GPU Mask Bit").
void Ps1Gpu::PlotPixel(int32_t x, int32_t y, int32_t r, int32_t g, int32_t b,
                       int32_t u, int32_t v, const DrawFlags& f) {
    static const int8_t kDither[4][4] = {
        {-4, +0, -3, +1},
        {+2, -2, +3, -1},
        {-3, +1, -4, +0},
        {+3, -1, +2, -2},
    };
    const uint32_t addr = (static_cast<uint32_t>(y) & 511) * PS1_VRAM_WIDTH + (static_cast<uint32_t>(x) & 1023);
    const uint16_t bg = m_vram[addr];
    if (m_mask_check && (bg & 0x8000)) return;

    bool blend = f.semi;
    uint16_t stp = 0;
    int32_t c8[3] = {r, g, b}; // 8-bit-scale color before dithering/truncation
    if (f.textured) {
        uint16_t texel = FetchTexel(u, v, f.clut);
        if (texel == 0) return; // 0000h is the fully transparent texel
        stp = texel & 0x8000;
        blend = f.semi && stp; // textured: only texels with bit 15 set are semi-transparent
        int32_t t5[3] = {texel & 0x1F, (texel >> 5) & 0x1F, (texel >> 10) & 0x1F};
        for (int i = 0; i < 3; ++i) {
            // Modulation: texel * color / 128 (80h = unchanged), kept at 8-bit scale for dithering.
            c8[i] = f.raw ? (t5[i] << 3) : ((t5[i] * c8[i]) >> 4);
        }
    }
    int32_t c5[3];
    const int32_t d = f.dither ? kDither[y & 3][x & 3] : 0;
    for (int i = 0; i < 3; ++i) {
        c5[i] = std::clamp(c8[i] + d, 0, 255) >> 3;
    }

    if (blend) {
        const int32_t b5[3] = {bg & 0x1F, (bg >> 5) & 0x1F, (bg >> 10) & 0x1F};
        const uint32_t mode = (m_texpage >> 5) & 3;
        for (int i = 0; i < 3; ++i) {
            int32_t out = 0;
            switch (mode) {
                case 0: out = (b5[i] + c5[i]) >> 1; break; // B/2 + F/2
                case 1: out = b5[i] + c5[i]; break;        // B + F
                case 2: out = b5[i] - c5[i]; break;        // B - F
                case 3: out = b5[i] + (c5[i] >> 2); break; // B + F/4
            }
            c5[i] = std::clamp(out, 0, 31);
        }
    }

    uint16_t pixel = static_cast<uint16_t>(c5[0] | (c5[1] << 5) | (c5[2] << 10)) | stp;
    if (m_mask_set) pixel |= 0x8000;
    m_vram[addr] = pixel;
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

