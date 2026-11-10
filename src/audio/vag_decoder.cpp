#include "audio/vag_decoder.h"
#include "core/logger.h"
#include <fstream>
#include <algorithm>
#include <cstring>

namespace personarm {

// Hardware PS1 SPU Predictor Coefficients (fixed-point multiplied by 64)
static constexpr double K0_TABLE[5] = { 0.0,  60.0 / 64.0, 115.0 / 64.0,  98.0 / 64.0, 122.0 / 64.0 };
static constexpr double K1_TABLE[5] = { 0.0,   0.0 / 64.0, -52.0 / 64.0, -55.0 / 64.0, -60.0 / 64.0 };

std::optional<VagAudioData> VagDecoder::DecodeFromMemory(const uint8_t* data, size_t size) {
    if (!data || size < 48) {
        PRM_LOG_ERROR("VagDecoder", "Buffer too small for VAG header (min 48 bytes)");
        return std::nullopt;
    }

    // 1. Check Magic "VAGp"
    uint32_t magic = ReadBe32(data);
    if (magic != VAG_MAGIC_BE) {
        PRM_LOG_ERROR("VagDecoder", "Magic mismatch, expected 'VAGp'");
        return std::nullopt;
    }

    // 2. Parse Header (Big Endian)
    // uint32_t version = ReadBe32(data + 4);
    uint32_t data_size = ReadBe32(data + 12);
    uint32_t sample_rate = ReadBe32(data + 16);

    char name_buf[17] = {0};
    std::memcpy(name_buf, data + 32, 16);

    VagAudioData result;
    result.sample_rate = (sample_rate > 0) ? sample_rate : 44100;
    result.sound_name = std::string(name_buf);

    size_t adpcm_offset = 48;
    size_t available_adpcm = size - adpcm_offset;
    size_t blocks_to_read = std::min(static_cast<size_t>(data_size), available_adpcm) / 16;

    result.pcm_samples.reserve(blocks_to_read * 28);

    double s_1 = 0.0;
    double s_2 = 0.0;

    for (size_t b = 0; b < blocks_to_read; ++b) {
        const uint8_t* block = data + adpcm_offset + (b * 16);

        uint8_t byte0 = block[0];
        uint8_t byte1 = block[1]; // flags

        int shift = byte0 & 0x0F;
        int filter = (byte0 >> 4) & 0x07;

        if (filter > 4) filter = 4; // Safety clamp

        double k0 = K0_TABLE[filter];
        double k1 = K1_TABLE[filter];

        // Flag checks
        if (byte1 & 0x04) {
            result.has_loop = true;
            result.loop_start_sample = static_cast<uint32_t>(result.pcm_samples.size());
        }
        if (byte1 & 0x01) {
            result.loop_end_sample = static_cast<uint32_t>(result.pcm_samples.size() + 28);
        }

        // Process 28 samples (14 bytes * 2 nibbles)
        for (int i = 2; i < 16; ++i) {
            uint8_t sample_byte = block[i];

            // Low nibble first
            int nibble0 = sample_byte & 0x0F;
            if (nibble0 >= 8) nibble0 -= 16; // Sign extend 4-bit to signed

            // High nibble second
            int nibble1 = (sample_byte >> 4) & 0x0F;
            if (nibble1 >= 8) nibble1 -= 16;

            int nibbles[2] = { nibble0, nibble1 };

            for (int n = 0; n < 2; ++n) {
                int s_unscaled = nibbles[n] << (12 - shift);
                double sample = static_cast<double>(s_unscaled) + (s_1 * k0) + (s_2 * k1);

                s_2 = s_1;
                s_1 = sample;

                // Clamp to 16-bit signed range
                int32_t clamped = static_cast<int32_t>(sample);
                if (clamped > 32767) clamped = 32767;
                if (clamped < -32768) clamped = -32768;

                result.pcm_samples.push_back(static_cast<int16_t>(clamped));
            }
        }

        if (byte1 & 0x01) {
            // Loop end reached
            break;
        }
    }

    PRM_LOG_INFO("VagDecoder", "Decoded VAG sound '" + result.sound_name + "': " +
                               std::to_string(result.pcm_samples.size()) + " samples at " +
                               std::to_string(result.sample_rate) + " Hz");
    return result;
}

std::optional<VagAudioData> VagDecoder::DecodeFromFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        PRM_LOG_ERROR("VagDecoder", "Failed to open file: " + filepath);
        return std::nullopt;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        PRM_LOG_ERROR("VagDecoder", "Failed to read file: " + filepath);
        return std::nullopt;
    }

    return DecodeFromMemory(buffer.data(), buffer.size());
}

} // namespace personarm
