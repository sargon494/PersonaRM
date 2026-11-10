#include "loader/tim_parser.h"
#include "core/logger.h"
#include <fstream>
#include <cstring>

namespace personarm {

TimColorRGBA TimParser::ConvertPs1ColorToRGBA(uint16_t ps1_color) {
    TimColorRGBA out;
    // Format: 1:5:5:5 (STP, Blue 5-bit, Green 5-bit, Red 5-bit)
    uint8_t r5 = (ps1_color >> 0) & 0x1F;
    uint8_t g5 = (ps1_color >> 5) & 0x1F;
    uint8_t b5 = (ps1_color >> 10) & 0x1F;
    bool stp   = (ps1_color & 0x8000) != 0;

    // Expand 5-bit to 8-bit: (val * 255 + 15) / 31 or (val << 3) | (val >> 2)
    out.r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    out.g = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
    out.b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));

    // PS1 Transparency rules:
    // If RGB == 0: STP=0 -> Transparent (Alpha 0), STP=1 -> Opaque black (Alpha 255)
    // If RGB != 0: STP=0 -> Normal Opaque (Alpha 255), STP=1 -> Semi-transparent (Alpha 128 / blend)
    if (r5 == 0 && g5 == 0 && b5 == 0) {
        out.a = stp ? 255 : 0;
    } else {
        out.a = stp ? 180 : 255;
    }

    return out;
}

std::optional<TimImage> TimParser::ParseFromMemory(const uint8_t* data, size_t size) {
    if (!data || size < 8) {
        PRM_LOG_ERROR("TimParser", "Invalid buffer or size < 8 bytes");
        return std::nullopt;
    }

    // 1. Check Magic
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(uint32_t));
    if (magic != TIM_MAGIC) {
        PRM_LOG_ERROR("TimParser", "Magic mismatch, expected 0x00000010");
        return std::nullopt;
    }

    // 2. Read Flags
    uint32_t flags = 0;
    std::memcpy(&flags, data + 4, sizeof(uint32_t));

    uint8_t mode_val = static_cast<uint8_t>(flags & 0x07);
    bool has_clut = (flags & 0x08) != 0;

    if (mode_val > 3) {
        PRM_LOG_ERROR("TimParser", "Unsupported TIM mode: " + std::to_string(mode_val));
        return std::nullopt;
    }

    TimBppMode bpp_mode = static_cast<TimBppMode>(mode_val);
    size_t offset = 8;

    // 3. Parse CLUT if present
    std::vector<TimColorRGBA> clut_palette;
    if (has_clut) {
        if (offset + 12 > size) {
            PRM_LOG_ERROR("TimParser", "Unexpected EOF reading CLUT header");
            return std::nullopt;
        }

        uint32_t clut_size = 0;
        std::memcpy(&clut_size, data + offset, sizeof(uint32_t));

        uint16_t clut_w = 0, clut_h = 0;
        std::memcpy(&clut_w, data + offset + 8, sizeof(uint16_t));
        std::memcpy(&clut_h, data + offset + 10, sizeof(uint16_t));

        size_t total_entries = static_cast<size_t>(clut_w) * clut_h;
        if (offset + clut_size > size) {
            PRM_LOG_ERROR("TimParser", "CLUT size exceeds buffer");
            return std::nullopt;
        }

        const uint16_t* clut_data = reinterpret_cast<const uint16_t*>(data + offset + 12);
        clut_palette.reserve(total_entries);
        for (size_t i = 0; i < total_entries; ++i) {
            clut_palette.push_back(ConvertPs1ColorToRGBA(clut_data[i]));
        }

        offset += clut_size;
    }

    // 4. Parse Image Data Block
    if (offset + 12 > size) {
        PRM_LOG_ERROR("TimParser", "Unexpected EOF reading Image header");
        return std::nullopt;
    }

    uint32_t img_size = 0;
    std::memcpy(&img_size, data + offset, sizeof(uint32_t));

    uint16_t vram_x = 0, vram_y = 0;
    uint16_t img_w_words = 0, img_h = 0;
    std::memcpy(&vram_x, data + offset + 4, sizeof(uint16_t));
    std::memcpy(&vram_y, data + offset + 6, sizeof(uint16_t));
    std::memcpy(&img_w_words, data + offset + 8, sizeof(uint16_t));
    std::memcpy(&img_h, data + offset + 10, sizeof(uint16_t));

    if (offset + img_size > size) {
        PRM_LOG_ERROR("TimParser", "Image data size exceeds buffer");
        return std::nullopt;
    }

    const uint8_t* pixel_bytes = data + offset + 12;

    TimImage result;
    result.vram_x = vram_x;
    result.vram_y = vram_y;
    result.height = img_h;
    result.bpp_mode = bpp_mode;
    result.has_clut = has_clut;

    // Calculate real pixel width based on BPP mode
    switch (bpp_mode) {
        case TimBppMode::Indexed4Bit:
            result.width = static_cast<uint32_t>(img_w_words) * 4;
            break;
        case TimBppMode::Indexed8Bit:
            result.width = static_cast<uint32_t>(img_w_words) * 2;
            break;
        case TimBppMode::Direct15Bit:
            result.width = static_cast<uint32_t>(img_w_words);
            break;
        case TimBppMode::Direct24Bit:
            result.width = (static_cast<uint32_t>(img_w_words) * 2) / 3;
            break;
    }

    size_t total_pixels = static_cast<size_t>(result.width) * result.height;
    result.pixels_rgba.resize(total_pixels);

    // Decode to RGBA
    if (bpp_mode == TimBppMode::Indexed4Bit) {
        size_t byte_idx = 0;
        for (size_t y = 0; y < result.height; ++y) {
            for (size_t x = 0; x < result.width; x += 2) {
                uint8_t byte = pixel_bytes[byte_idx++];
                uint8_t c0 = byte & 0x0F;
                uint8_t c1 = (byte >> 4) & 0x0F;

                size_t out_idx0 = y * result.width + x;
                size_t out_idx1 = out_idx0 + 1;

                if (out_idx0 < total_pixels) {
                    result.pixels_rgba[out_idx0] = (c0 < clut_palette.size()) ? clut_palette[c0] : TimColorRGBA{};
                }
                if (out_idx1 < total_pixels) {
                    result.pixels_rgba[out_idx1] = (c1 < clut_palette.size()) ? clut_palette[c1] : TimColorRGBA{};
                }
            }
        }
    } else if (bpp_mode == TimBppMode::Indexed8Bit) {
        size_t byte_idx = 0;
        for (size_t i = 0; i < total_pixels; ++i) {
            uint8_t color_idx = pixel_bytes[byte_idx++];
            result.pixels_rgba[i] = (color_idx < clut_palette.size()) ? clut_palette[color_idx] : TimColorRGBA{};
        }
    } else if (bpp_mode == TimBppMode::Direct15Bit) {
        const uint16_t* words = reinterpret_cast<const uint16_t*>(pixel_bytes);
        for (size_t i = 0; i < total_pixels; ++i) {
            result.pixels_rgba[i] = ConvertPs1ColorToRGBA(words[i]);
        }
    } else if (bpp_mode == TimBppMode::Direct24Bit) {
        size_t byte_idx = 0;
        for (size_t i = 0; i < total_pixels; ++i) {
            TimColorRGBA color;
            color.r = pixel_bytes[byte_idx++];
            color.g = pixel_bytes[byte_idx++];
            color.b = pixel_bytes[byte_idx++];
            color.a = 255;
            result.pixels_rgba[i] = color;
        }
    }

    PRM_LOG_INFO("TimParser", "Decoded TIM: " + std::to_string(result.width) + "x" +
                              std::to_string(result.height) + " BPP mode=" + std::to_string(mode_val));
    return result;
}

std::optional<TimImage> TimParser::ParseFromFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        PRM_LOG_ERROR("TimParser", "Failed to open file: " + filepath);
        return std::nullopt;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        PRM_LOG_ERROR("TimParser", "Failed to read file: " + filepath);
        return std::nullopt;
    }

    return ParseFromMemory(buffer.data(), buffer.size());
}

} // namespace personarm
