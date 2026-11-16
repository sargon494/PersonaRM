#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include "loader/tim_parser.h"

namespace personarm {

class VramTextureHook;
class Widescreen2D;
class StateIO;
class Ps1Gpu;

/// Display-only listener for a host GPU renderer (rendering/gpu_renderer.h). Never changes emulation.
class GpuDrawListener {
public:
    virtual ~GpuDrawListener() = default;
    /// Every complete GP0 packet, before it executes (the 1x VRAM still holds what it samples).
    /// Not called for polyline continuation words or upload/read data words.
    virtual void OnGp0(const Ps1Gpu& gpu, const uint32_t* words, size_t n) = 0;
    /// The 1x VRAM rect was just written outside the rasterizer: CPU upload, fill, TIM load, WriteVram,
    /// SetVram (the writes the scaled mirror nearest-upscales). Not called for 80h copies (see OnGp0).
    virtual void OnVramWrite(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) = 0;
    /// One line segment (a line packet or a polyline segment) as the rasterizer draws it: VRAM-space
    /// endpoints (drawing offset and widescreen squeeze applied), 24-bit colours.
    virtual void OnLine(const Ps1Gpu&, int32_t /*ax*/, int32_t /*ay*/, uint32_t /*ac*/, int32_t /*bx*/, int32_t /*by*/,
                        uint32_t /*bc*/, bool /*gouraud*/, bool /*semi*/) {}
    /// Widescreen: the polygon / rectangle of the last OnGp0 is drawn squeezed, with these VRAM-space X
    /// coordinates (polygon: one per vertex; rectangle: left edge and right edge + 1).
    virtual void OnSqueeze(const int32_t* /*x*/, int /*n*/) {}
};

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

    void Reset();          // power-on: clears VRAM too
    void ResetRegisters(); // GP1(00h): keeps VRAM

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
    void SetVram(const std::vector<uint16_t>& vram);
    /// GP0 polygon commands (20h-3Fh) executed since construction (boot instrumentation).
    uint64_t GetPolygonCount() const { return m_polygon_count; }

    /// Display-only capture for frame interpolation (rendering/frame_interp.h); null = off (default) and
    /// emulation is unaffected either way. Appends every executed GP0 packet except VRAM uploads/reads
    /// (A0h-DFh) as [n, w0..wn-1], polyline continuation words as [1, w], and each display-start change
    /// (GP1 05h, a double-buffer flip) as [0, x | y << 16] followed by E1h-E6h packets for the current
    /// draw state, so every segment between two flips replays on its own.
    void SetCommandLog(std::vector<uint32_t>* log) { m_cmd_log = log; }

    /// Texture dump / replacement (rendering/vram_texture_hook.h); null = off (default), no effect on VRAM.
    void SetTextureHook(VramTextureHook* hook);

    /// Display-only internal resolution (1-4, default 1 = off, no extra work). The 1x VRAM stays the only
    /// thing the game sees (GPUREAD, copies, textures); a second (1024*s)x(512*s) VRAM mirrors it:
    /// polygons/rectangles are re-rasterized there at s x (finer edge and UV sampling, textures read from
    /// the 1x VRAM or the HD hook), lines and every CPU/FMV upload, fill and TIM load are nearest-upscaled,
    /// VRAM->VRAM copies copy the scaled pixels. Semi-transparency and the mask bit work on the scaled
    /// pixels; dithering uses the 1x pattern (scaled). Enabling it upscales the current VRAM.
    void SetResolutionScale(uint32_t scale);
    uint32_t GetResolutionScale() const { return m_scale; }
    /// Widescreen: squeeze 2D primitives (no GTE vertex) while a 3D scene is up (rendering/widescreen_2d.h).
    /// Null = off (default): drawing is unchanged.
    void SetWidescreen2D(Widescreen2D* ws) { m_ws2d = ws; }

    /// The scaled VRAM, row stride 1024*scale; empty at scale 1.
    const std::vector<uint16_t>& GetScaledVram() const { return m_vram_hi; }
    /// Save state: VRAM, registers, packet/transfer state (the scaled mirror is rebuilt on load).
    void Serialize(StateIO& io);

    /// Host GPU renderer listener; null = off (default).
    void SetDrawListener(GpuDrawListener* l) { m_listener = l; }
    uint32_t GetTexpage() const { return m_texpage; }      // E1h bits 0-13
    uint32_t GetTexWindow() const { return m_texwindow; }  // E2h bits 0-19
    bool GetMaskSet() const { return m_mask_set; }
    bool GetMaskCheck() const { return m_mask_check; }

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
    // Raster target: the 1x VRAM (s = 1) or the scaled VRAM. Coordinates and UVs are in target units
    // (scaled: UV in 1/(256 s) texel steps).
    struct Target {
        uint16_t* px;
        int32_t s;
    };
    Target Vram1x() { return {m_vram.data(), 1}; }
    void DrawTriangle(const Target& t, const Vertex& a, const Vertex& b, const Vertex& c, bool gouraud,
                      const DrawFlags& f);
    void DrawLine(const Vertex& a, const Vertex& b, bool gouraud, bool semi);
    void DrawLineOn(const Target& t, const Vertex& a, const Vertex& b, bool gouraud, const DrawFlags& f);
    void PlotPixel(const Target& t, int32_t x, int32_t y, int32_t r, int32_t g, int32_t b,
                   int32_t u, int32_t v, const DrawFlags& f);
    uint16_t FetchTexel(int32_t u, int32_t v, uint32_t fu, uint32_t fv, uint32_t s, uint16_t clut) const;
    void UpscaleRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);  // 1x rect -> scaled VRAM (nearest)
    Vertex ParseVertex(uint32_t xy_word, uint32_t color_word) const;
    void UpdateStatDrawBits();

    // E1h draw mode (bits 0-13), E2h texture window (bits 0-19), E6h mask settings
    uint32_t m_texpage = 0;
    uint32_t m_texwindow = 0;
    bool m_mask_set = false;
    bool m_mask_check = false;
    uint64_t m_polygon_count = 0;
    std::vector<uint32_t>* m_cmd_log = nullptr;
    VramTextureHook* m_hook = nullptr;
    Widescreen2D* m_ws2d = nullptr;
    GpuDrawListener* m_listener = nullptr;
    void LogWords(const uint32_t* w, size_t n) {
        m_cmd_log->push_back(static_cast<uint32_t>(n));
        m_cmd_log->insert(m_cmd_log->end(), w, w + n);
    }

    // Polyline (GP0 48h-4Fh / 58h-5Fh) streaming state
    bool m_in_polyline = false;
    bool m_polyline_gouraud = false;
    bool m_polyline_semi = false;
    Vertex m_polyline_last;
    uint32_t m_polyline_pending_color = 0;
    bool m_polyline_have_color = false;
    int32_t m_polyline_cx = INT32_MIN;  // widescreen: centre the polyline's 2D vertices are squeezed around (or kNone)

    // VRAM to CPU (C0h) transfer state
    uint32_t m_read_x = 0, m_read_y = 0, m_read_w = 0, m_read_h = 0;
    uint32_t m_read_cur_x = 0, m_read_cur_y = 0;
    uint32_t m_read_words_left = 0;
    uint32_t m_gpuread_latch = 0;

    // 1 MB VRAM (1024x512 16-bit pixels in RGB 1:5:5:5)
    std::vector<uint16_t> m_vram;
    uint32_t m_scale = 1;
    std::vector<uint16_t> m_vram_hi;  // display-only scaled mirror (SetResolutionScale)

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

