#include "audio/spu_emulator.h"
#include "audio/vag_decoder.h"
#include "core/logger.h"
#include <algorithm>
#include <cstring>

namespace personarm {

// Standard Sony PS1 SPU ADPCM Coefficients (fixed-point Q.6)
static const int32_t SPU_COEFFS[5][2] = {
    {   0,    0 },
    {  60,    0 },
    { 115,  -52 },
    {  98,  -55 },
    { 122,  -60 }
};

SpuEmulator::SpuEmulator() {
    m_spu_ram.resize(SPU_RAM_SIZE, 0);
    Reset();
}

void SpuEmulator::Reset() {
    std::fill(m_spu_ram.begin(), m_spu_ram.end(), 0);
    for (auto& voice : m_voices) {
        voice = SpuVoice{};
        voice.volume_left = 0x3FFF;
        voice.volume_right = 0x3FFF;
        voice.sample_rate_pitch = 4096; // 44.1 kHz default
        voice.adsr_level = 0;
        voice.adsr_phase = AdsrPhase::Off;
        voice.sample_index = 28;
    }
    m_master_volume_left = 0x3FFF;
    m_master_volume_right = 0x3FFF;
}

uint8_t SpuEmulator::ReadRam(uint32_t address) const {
    return m_spu_ram[address & (SPU_RAM_SIZE - 1)];
}

void SpuEmulator::WriteRam(uint32_t address, uint8_t byte) {
    m_spu_ram[address & (SPU_RAM_SIZE - 1)] = byte;
}

void SpuEmulator::UploadToRam(uint32_t destination, const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    destination &= (SPU_RAM_SIZE - 1);
    size_t copy_len = std::min<size_t>(size, SPU_RAM_SIZE - destination);
    std::memcpy(m_spu_ram.data() + destination, data, copy_len);
}

void SpuEmulator::KeyOn(uint32_t voice_mask) {
    for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i) {
        if ((voice_mask & (1 << i)) != 0) {
            auto& v = m_voices[i];
            v.key_on = true;
            v.current_address = v.start_address;
            v.hist1 = 0;
            v.hist2 = 0;
            v.adsr_level = 0;
            v.adsr_phase = AdsrPhase::Attack;
            v.sample_index = 28; // Force decode first block
        }
    }
}

void SpuEmulator::KeyOff(uint32_t voice_mask) {
    for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i) {
        if ((voice_mask & (1 << i)) != 0) {
            auto& v = m_voices[i];
            v.key_on = false;
            v.adsr_phase = AdsrPhase::Release;
        }
    }
}

void SpuEmulator::SetVoicePitch(uint32_t voice, uint16_t pitch) {
    if (voice < SPU_NUM_VOICES) {
        m_voices[voice].sample_rate_pitch = pitch;
    }
}

void SpuEmulator::SetVoiceVolume(uint32_t voice, int16_t vol_left, int16_t vol_right) {
    if (voice < SPU_NUM_VOICES) {
        m_voices[voice].volume_left = vol_left;
        m_voices[voice].volume_right = vol_right;
    }
}

void SpuEmulator::SetVoiceStartAddress(uint32_t voice, uint32_t address) {
    if (voice < SPU_NUM_VOICES) {
        m_voices[voice].start_address = address & (SPU_RAM_SIZE - 1);
        m_voices[voice].loop_address = m_voices[voice].start_address;
    }
}

uint32_t SpuEmulator::GetActiveVoiceMask() const {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i) {
        if (m_voices[i].adsr_phase != AdsrPhase::Off) {
            mask |= (1 << i);
        }
    }
    return mask;
}

void SpuEmulator::DecodeNextBlock(SpuVoice& voice) {
    uint32_t addr = voice.current_address & (SPU_RAM_SIZE - 1);
    if (addr + 16 > SPU_RAM_SIZE) {
        voice.adsr_phase = AdsrPhase::Off;
        return;
    }

    const uint8_t* block = m_spu_ram.data() + addr;
    uint8_t shift = block[0] & 0x0F;
    uint8_t filter = (block[0] >> 4) & 0x0F;
    uint8_t flags = block[1];

    if (filter > 4) filter = 0;

    int32_t f0 = SPU_COEFFS[filter][0];
    int32_t f1 = SPU_COEFFS[filter][1];

    int32_t s_1 = voice.hist1;
    int32_t s_2 = voice.hist2;

    int out_idx = 0;
    for (int i = 0; i < 14; ++i) {
        uint8_t byte = block[2 + i];
        int8_t nibble0 = static_cast<int8_t>((byte & 0x0F) << 4) >> 4;
        int8_t nibble1 = static_cast<int8_t>(byte & 0xF0) >> 4;

        int8_t nibbles[2] = { nibble0, nibble1 };
        for (int n = 0; n < 2; ++n) {
            int32_t raw = static_cast<int32_t>(nibbles[n]) << (12 - shift);
            int32_t pred = (s_1 * f0 + s_2 * f1 + 32) >> 6;
            int32_t sample = std::clamp<int32_t>(raw + pred, -32768, 32767);

            s_2 = s_1;
            s_1 = sample;
            voice.block_samples[out_idx++] = static_cast<int16_t>(sample);
        }
    }

    voice.hist1 = static_cast<int16_t>(s_1);
    voice.hist2 = static_cast<int16_t>(s_2);
    voice.sample_index = 0;

    // Check block flags: 0x01 = Loop End, 0x02 = Loop Repeat, 0x04 = Loop Start
    if ((flags & 0x04) != 0) {
        voice.loop_address = addr;
    }

    if ((flags & 0x01) != 0) { // End of sound
        if ((flags & 0x02) != 0) { // Repeat
            voice.current_address = voice.loop_address;
        } else {
            voice.adsr_phase = AdsrPhase::Release;
        }
    } else {
        voice.current_address = (addr + 16) & (SPU_RAM_SIZE - 1);
    }
}

void SpuEmulator::UpdateEnvelope(SpuVoice& voice) {
    switch (voice.adsr_phase) {
        case AdsrPhase::Attack:
            if (voice.adsr_level < 0x7FFF - 512) {
                voice.adsr_level += 512;
            } else {
                voice.adsr_level = 0x7FFF;
                voice.adsr_phase = AdsrPhase::Decay;
            }
            break;
        case AdsrPhase::Decay:
            if (voice.adsr_level > 0x4000 + 256) {
                voice.adsr_level -= 256;
            } else {
                voice.adsr_level = 0x4000;
                voice.adsr_phase = AdsrPhase::Sustain;
            }
            break;
        case AdsrPhase::Sustain:
            // Sustain holds level unless pitch or key off
            break;
        case AdsrPhase::Release:
            if (voice.adsr_level > 256) {
                voice.adsr_level -= 256;
            } else {
                voice.adsr_level = 0;
                voice.adsr_phase = AdsrPhase::Off;
            }
            break;
        case AdsrPhase::Off:
        default:
            voice.adsr_level = 0;
            break;
    }
}

void SpuEmulator::GenerateSamples(std::vector<int16_t>& out_stereo_pcm, size_t num_frames) {
    out_stereo_pcm.resize(num_frames * 2, 0);

    for (size_t frame = 0; frame < num_frames; ++frame) {
        int32_t mix_left = 0;
        int32_t mix_right = 0;

        for (uint32_t v = 0; v < SPU_NUM_VOICES; ++v) {
            auto& voice = m_voices[v];
            if (voice.adsr_phase == AdsrPhase::Off) continue;

            if (voice.sample_index >= 28) {
                DecodeNextBlock(voice);
                if (voice.adsr_phase == AdsrPhase::Off) continue;
            }

            int16_t raw_sample = voice.block_samples[voice.sample_index++];
            UpdateEnvelope(voice);

            // Apply ADSR envelope (0..0x7FFF)
            int32_t env_sample = (static_cast<int32_t>(raw_sample) * voice.adsr_level) >> 15;

            // Apply voice volume and pan
            int32_t v_left = (env_sample * voice.volume_left) >> 15;
            int32_t v_right = (env_sample * voice.volume_right) >> 15;

            mix_left += v_left;
            mix_right += v_right;
        }

        // Apply master volume and clamp
        int32_t master_l = (mix_left * m_master_volume_left) >> 15;
        int32_t master_r = (mix_right * m_master_volume_right) >> 15;

        out_stereo_pcm[frame * 2 + 0] = static_cast<int16_t>(std::clamp<int32_t>(master_l, -32768, 32767));
        out_stereo_pcm[frame * 2 + 1] = static_cast<int16_t>(std::clamp<int32_t>(master_r, -32768, 32767));
    }
}

} // namespace personarm

