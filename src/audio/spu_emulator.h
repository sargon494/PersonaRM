#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <string>

namespace personarm {

// PS1 SPU. Reference: psx-spx "Sound Processing Unit (SPU)" sections:
// "SPU Overview", "SPU Voice Registers", "SPU Control Registers", "SPU ADPCM Samples",
// "SPU ADSR Generator", "SPU Interpolation", "SPU Reverb Formulas".

constexpr uint32_t SPU_RAM_SIZE    = 512 * 1024;
constexpr uint32_t SPU_NUM_VOICES  = 24;
constexpr uint32_t SPU_SAMPLE_RATE = 44100;
constexpr uint32_t SPU_REG_BASE    = 0x1F801C00; // registers 0x1F801C00..0x1F801DFF

// 512-entry interpolation kernel (see shortcut note in spu_emulator.cpp)
const std::array<int16_t, 512>& SpuGaussTable();

enum class AdsrPhase : uint8_t { Off = 0, Attack, Decay, Sustain, Release };

struct SpuVoice {
    uint16_t vol_left_reg = 0, vol_right_reg = 0; // raw registers
    int16_t volume_left = 0, volume_right = 0;    // effective (fixed mode: reg<<1)
    uint16_t sample_rate_pitch = 0x1000;          // 0x1000 = 44100 Hz
    uint32_t start_address = 0;                   // byte address
    uint32_t current_address = 0;                 // byte address of current block
    uint32_t loop_address = 0;                    // repeat address (byte)
    uint16_t adsr1 = 0, adsr2 = 0;                // raw ADSR registers
    uint16_t adsr_level = 0;                      // 0..0x7FFF
    AdsrPhase adsr_phase = AdsrPhase::Off;
    uint32_t adsr_counter = 0;

    int16_t hist1 = 0, hist2 = 0;                 // ADPCM predictor history
    std::array<int16_t, 28> block_samples{};
    uint8_t sample_index = 28;                    // 28 = next block must be fetched
    bool block_valid = false;
    uint32_t pitch_counter = 0;                   // 12-bit fraction
    std::array<int16_t, 4> interp{};              // [0]=oldest .. [3]=newest
};

class SpuEmulator {
public:
    SpuEmulator();
    ~SpuEmulator() = default;

    void Reset();

    // Sound RAM
    uint8_t ReadRam(uint32_t address) const;
    void WriteRam(uint32_t address, uint8_t byte);
    void UploadToRam(uint32_t destination, const uint8_t* data, size_t size);
    // DMA4 style write at the current transfer address (advances it)
    void DmaWrite(const uint8_t* data, size_t size);

    // Hardware register file (physical address 0x1F801C00.. or offset 0..0x1FF)
    void Write16(uint32_t addr, uint16_t value);
    uint16_t Read16(uint32_t addr) const;

    void KeyOn(uint32_t voice_mask);
    void KeyOff(uint32_t voice_mask);

    // Convenience setters (values in raw register format; bit15 clear = fixed volume)
    void SetVoicePitch(uint32_t voice, uint16_t pitch);
    void SetVoiceVolume(uint32_t voice, int16_t vol_left, int16_t vol_right);
    void SetVoiceStartAddress(uint32_t voice, uint32_t byte_address);
    void SetMasterVolume(int16_t left, int16_t right);

    // Mixes num_frames of 44.1 kHz stereo (interleaved L,R) into out_stereo_pcm (resized).
    void GenerateSamples(std::vector<int16_t>& out_stereo_pcm, size_t num_frames);

    const SpuVoice& GetVoice(uint32_t index) const { return m_voices[index < SPU_NUM_VOICES ? index : 0]; }
    uint32_t GetActiveVoiceMask() const;
    uint32_t GetEndx() const { return m_endx; }

private:
    void DecodeBlock(SpuVoice& v);
    void PushSample(SpuVoice& v);
    void StepEnvelope(SpuVoice& v);
    void ReverbTick(int32_t in_l, int32_t in_r);
    uint32_t RevAddr(int32_t byte_offset) const;
    int32_t RevRead(int32_t byte_offset) const;
    void RevWrite(int32_t byte_offset, int32_t value);
    uint16_t G(uint32_t off) const { return m_glob[(off - 0x180) >> 1]; }

    std::vector<uint8_t> m_spu_ram;
    std::array<SpuVoice, SPU_NUM_VOICES> m_voices;

    // global registers 0x180..0x1FF (offset from 0x1F801C00), halfword indexed
    std::array<uint16_t, 64> m_glob{};
    int16_t m_main_l = 0, m_main_r = 0;
    uint32_t m_endx = 0;
    uint32_t m_transfer_addr = 0; // byte address
    uint16_t m_spucnt = 0;
    uint32_t m_rev_cur = 0;       // reverb cursor (byte address)
    bool m_rev_phase = false;
    int32_t m_rev_out_l = 0, m_rev_out_r = 0;
};

} // namespace personarm
