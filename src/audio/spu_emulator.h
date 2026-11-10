#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <string>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 SPU (Sound Processing Unit) Hardware Specifications
// ───────────────────────────────────────────────────────────────────────────

constexpr uint32_t SPU_RAM_SIZE    = 512 * 1024; // 512 KB Sound RAM
constexpr uint32_t SPU_NUM_VOICES  = 24;         // 24 Hardware Voices
constexpr uint32_t SPU_SAMPLE_RATE = 44100;      // 44.1 kHz standard audio output

enum class AdsrPhase : uint8_t {
    Off = 0,
    Attack,
    Decay,
    Sustain,
    Release
};

struct SpuVoice {
    // Volume & Panning (-0x8000 to +0x7FFF)
    int16_t volume_left = 0;
    int16_t volume_right = 0;

    // Pitch (4096 = 44100 Hz, 1.0x rate)
    uint16_t sample_rate_pitch = 4096;

    // Current sound buffer addresses in SPU RAM
    uint32_t start_address = 0;
    uint32_t current_address = 0;
    uint32_t loop_address = 0;

    // ADSR Envelope Configuration
    uint16_t adsr_level = 0;
    AdsrPhase adsr_phase = AdsrPhase::Off;
    uint16_t attack_rate = 0;
    uint16_t decay_rate = 0;
    uint16_t sustain_level = 0;
    uint16_t sustain_rate = 0;
    uint16_t release_rate = 0;

    // Decoding state (Sony ADPCM predictor history)
    int16_t hist1 = 0;
    int16_t hist2 = 0;

    // Decoded sample buffer (16 samples per 16-byte VAG block)
    std::array<int16_t, 28> block_samples{};
    uint8_t sample_index = 28; // 28 means need decode next block

    bool key_on = false;
    bool is_looping = false;
};

class SpuEmulator {
public:
    SpuEmulator();
    ~SpuEmulator() = default;

    void Reset();

    // SPU RAM Access (0x00000 to 0x7FFFF)
    uint8_t ReadRam(uint32_t address) const;
    void WriteRam(uint32_t address, uint8_t byte);
    void UploadToRam(uint32_t destination, const uint8_t* data, size_t size);

    // Voice triggers
    void KeyOn(uint32_t voice_mask);
    void KeyOff(uint32_t voice_mask);

    // Voice parameter configuration
    void SetVoicePitch(uint32_t voice, uint16_t pitch);
    void SetVoiceVolume(uint32_t voice, int16_t vol_left, int16_t vol_right);
    void SetVoiceStartAddress(uint32_t voice, uint32_t address);

    // Master volume
    void SetMasterVolume(int16_t left, int16_t right) {
        m_master_volume_left = left;
        m_master_volume_right = right;
    }

    // Audio rendering: Mixes all active voices into 16-bit stereo PCM buffer
    void GenerateSamples(std::vector<int16_t>& out_stereo_pcm, size_t num_frames);

    // Voice state inspection
    const SpuVoice& GetVoice(uint32_t index) const { return m_voices[index < SPU_NUM_VOICES ? index : 0]; }
    uint32_t GetActiveVoiceMask() const;

private:
    void DecodeNextBlock(SpuVoice& voice);
    void UpdateEnvelope(SpuVoice& voice);

    // 512 KB SPU Sound RAM
    std::vector<uint8_t> m_spu_ram;

    // 24 hardware voices
    std::array<SpuVoice, SPU_NUM_VOICES> m_voices;

    int16_t m_master_volume_left = 0x3FFF;
    int16_t m_master_volume_right = 0x3FFF;
};

} // namespace personarm

