#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include "loader/tim_parser.h"

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 GPU Hardware Dimensions & Standards
// ───────────────────────────────────────────────────────────────────────────

constexpr uint32_t PS1_VRAM_WIDTH  = 1024;
constexpr uint32_t PS1_VRAM_HEIGHT = 512;
constexpr size_t   PS1_VRAM_WORDS  = PS1_VRAM_WIDTH * PS1_VRAM_HEIGHT; // 1 MB (16-bit pixels)

enum class GpuDmaDirection : uint8_t {
    Off        = 0,
    Fifo       = 1,
    CpuToVram  = 2,
    VramToCpu  = 3
};

struct GpuDisplayArea {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 320;
    uint16_t height = 240;
    bool is_24bit = false;
    bool is_pal = false;
    bool is_interlaced = false;
};

struct GpuDrawingArea {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 319;
    int32_t y2 = 239;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
};

// ───────────────────────────────────────────────────────────────────────────
// Ps1Gpu Class — PS1 Hardware GPU & VRAM Emulator
// ───────────────────────────────────────────────────────────────────────────

class Ps1Gpu {
public:
    Ps1Gpu();
    ~Ps1Gpu() = default;

    void Reset();

    // Hardware Register Access (0x1F801810 / 0x1F801814)
    void WriteGP0(uint32_t word);
    void WriteGP1(uint32_t word);

    uint32_t ReadGP0();
    uint32_t ReadGPUSTAT() const;

    // VRAM direct access
    uint16_t ReadVram(uint32_t x, uint32_t y) const;
    void WriteVram(uint32_t x, uint32_t y, uint16_t pixel);

    // Direct texture injection into VRAM (from parsed TIM images)
    bool LoadTimToVram(const TimImage& tim);

    // Display Area
    const GpuDisplayArea& GetDisplayArea() const { return m_display; }
    const GpuDrawingArea& GetDrawingArea() const { return m_drawing; }

    // Render active display area into RGBA8888 buffer for output presentation
    void OutputFrameToRGBA(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) const;

    // Fast clear screen / fill VRAM rect
    void FillVramRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color16);

    const std::vector<uint16_t>& GetVram() const { return m_vram; }
    void SetVram(const std::vector<uint16_t>& vram) { m_vram = vram; }
    /// GP0 polygon commands (20h-3Fh) executed since construction (boot instrumentation).
    uint64_t GetPolygonCount() const { return m_polygon_count; }

private:
    // One rasterizer vertex: VRAM-space position (drawing offset applied), 24-bit color, texcoord.
    struct Vertex {
        int32_t x = 0, y = 0;
        int32_t r = 0, g = 0, b = 0;
        int32_t u = 0, v = 0;
    };
    // Per-primitive render flags decoded from the GP0 command byte.
    struct DrawFlags {
        bool textured = false;
        bool raw = false;      // raw texture (no modulation)
        bool semi = false;     // semi-transparency requested
        bool dither = false;   // E1.9 and (gouraud or modulated texture)
        uint16_t clut = 0;     // CLUT attribute (X/16 in bits 0-5, Y in bits 6-14)
    };

    void ExecuteGP0Command();
    void ExecuteGP1Command(uint32_t word);
    static uint32_t Gp0CommandWords(uint8_t cmd);

    void DrawPolygon(uint8_t cmd);
    void DrawLineCommand(uint8_t cmd);
    void DrawRectangle(uint8_t cmd);
    void DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c, bool gouraud, const DrawFlags& f);
    void DrawLine(const Vertex& a, const Vertex& b, bool gouraud, bool semi);
    void PlotPixel(int32_t x, int32_t y, int32_t r, int32_t g, int32_t b,
                   int32_t u, int32_t v, const DrawFlags& f);
    uint16_t FetchTexel(int32_t u, int32_t v, uint16_t clut) const;
    Vertex ParseVertex(uint32_t xy_word, uint32_t color_word) const;
    void UpdateStatDrawBits();

    // E1h draw mode (bits 0-13), E2h texture window (bits 0-19), E6h mask settings
    uint32_t m_texpage = 0;
    uint32_t m_texwindow = 0;
    bool m_mask_set = false;
    bool m_mask_check = false;
    uint64_t m_polygon_count = 0;

    // Polyline (GP0 48h-4Fh / 58h-5Fh) streaming state
    bool m_in_polyline = false;
    bool m_polyline_gouraud = false;
    bool m_polyline_semi = false;
    Vertex m_polyline_last;
    uint32_t m_polyline_pending_color = 0;
    bool m_polyline_have_color = false;

    // VRAM to CPU (C0h) transfer state
    uint32_t m_read_x = 0, m_read_y = 0, m_read_w = 0, m_read_h = 0;
    uint32_t m_read_cur_x = 0, m_read_cur_y = 0;
    uint32_t m_read_words_left = 0;
    uint32_t m_gpuread_latch = 0;

    // 1 MB VRAM (1024x512 16-bit pixels in RGB 1:5:5:5)
    std::vector<uint16_t> m_vram;

    // Display & Drawing state
    GpuDisplayArea m_display;
    GpuDrawingArea m_drawing;

    // GPU Status Flags
    uint32_t m_gpustat = 0x14802000;
    bool m_display_disabled = false;
    GpuDmaDirection m_dma_direction = GpuDmaDirection::Off;

    // Command FIFO & Packet assembly
    std::vector<uint32_t> m_gp0_packet;
    uint32_t m_gp0_words_remaining = 0;

    // CPU to VRAM transfer state
    bool m_in_vram_transfer = false;
    uint16_t m_transfer_x = 0;
    uint16_t m_transfer_y = 0;
    uint16_t m_transfer_w = 0;
    uint16_t m_transfer_h = 0;
    uint32_t m_transfer_cur_x = 0;
    uint32_t m_transfer_cur_y = 0;
    uint32_t m_transfer_words_left = 0;
};

} // namespace personarm

