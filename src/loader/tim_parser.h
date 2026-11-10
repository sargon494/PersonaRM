#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace personarm {

enum class TimBppMode : uint8_t {
    Indexed4Bit  = 0,
    Indexed8Bit  = 1,
    Direct15Bit  = 2,
    Direct24Bit  = 3
};

struct TimColorRGBA {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

struct TimImage {
    uint32_t width = 0;
    uint32_t height = 0;
    TimBppMode bpp_mode = TimBppMode::Indexed4Bit;
    bool has_clut = false;
    uint16_t vram_x = 0;
    uint16_t vram_y = 0;
    std::vector<TimColorRGBA> pixels_rgba; // Decoded RGBA8888 buffer
};

class TimParser {
public:
    static constexpr uint32_t TIM_MAGIC = 0x00000010;

    /// Parse TIM file from raw memory buffer
    static std::optional<TimImage> ParseFromMemory(const uint8_t* data, size_t size);

    /// Parse TIM file directly from disc/filesystem path
    static std::optional<TimImage> ParseFromFile(const std::string& filepath);

    /// Convert 16-bit PS1 color (ABBBBBGGGGGRRRRR) to RGBA8888
    static TimColorRGBA ConvertPs1ColorToRGBA(uint16_t ps1_color);
};

} // namespace personarm
