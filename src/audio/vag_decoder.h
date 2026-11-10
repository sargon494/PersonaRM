#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace personarm {

struct VagAudioData {
    uint32_t sample_rate = 44100;
    uint32_t num_channels = 1; // VAGp is mono
    std::string sound_name;
    bool has_loop = false;
    uint32_t loop_start_sample = 0;
    uint32_t loop_end_sample = 0;
    std::vector<int16_t> pcm_samples; // 16-bit signed PCM
};

class VagDecoder {
public:
    static constexpr uint32_t VAG_MAGIC_BE = 0x56414770; // "VAGp" in Big-Endian

    /// Decode VAG file from memory buffer to 16-bit PCM
    static std::optional<VagAudioData> DecodeFromMemory(const uint8_t* data, size_t size);

    /// Decode VAG file from filesystem path
    static std::optional<VagAudioData> DecodeFromFile(const std::string& filepath);

private:
    static uint32_t ReadBe32(const uint8_t* ptr) {
        return (static_cast<uint32_t>(ptr[0]) << 24) |
               (static_cast<uint32_t>(ptr[1]) << 16) |
               (static_cast<uint32_t>(ptr[2]) << 8)  |
               (static_cast<uint32_t>(ptr[3]));
    }
};

} // namespace personarm

