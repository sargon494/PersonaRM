#pragma once

// Self-contained PNG reader/writer (no zlib dependency) for HD texture overrides.
// Decoder: non-interlaced images, color types 0/2/3/4/6, bit depths 1/2/4/8/16.
// Encoder: 8-bit RGBA, uncompressed (stored) deflate blocks - valid but large;
// intended for exporting original textures so artists can repaint them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace personarm {

struct PngImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width * height * 4
};

class PngCodec {
public:
    static constexpr uint32_t kMaxDimension = 16384;
    static constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;

    static std::optional<PngImage> Decode(const uint8_t* data, size_t size, std::string* error = nullptr);
    static std::optional<PngImage> DecodeFile(const std::string& path, std::string* error = nullptr);

    static std::vector<uint8_t> Encode(uint32_t width, uint32_t height, const uint8_t* rgba);
    static bool EncodeToFile(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);

    /// Raw DEFLATE (RFC 1951) decoder, exposed for tests. Fails if output would exceed max_out.
    static bool Inflate(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out,
                        size_t max_out, size_t* consumed = nullptr);
};

} // namespace personarm
