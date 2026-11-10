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

private:
    void ExecuteGP0Command();
    void ExecuteGP1Command(uint32_t word);

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

