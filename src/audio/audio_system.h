#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace personarm {

enum class AudioChannel {
    Master,
    BGM,
    SFX,
    Voice
};

struct AudioConfig {
    float master_volume = 1.0f;
    float bgm_volume = 0.8f;
    float sfx_volume = 0.9f;
    float voice_volume = 1.0f;
    bool high_quality_resampling = true;
    bool mute_on_focus_loss = true;
};

class AudioSystem {
public:
    static AudioSystem& Instance();

    bool Initialize(const AudioConfig& config = AudioConfig{});
    void Shutdown();

    // Volume Control
    void SetChannelVolume(AudioChannel channel, float volume);
    float GetChannelVolume(AudioChannel channel) const;
    float GetEffectiveVolume(AudioChannel channel) const;

    // Playback API
    void PlayBGM(const std::string& track_name, bool loop = true);
    void StopBGM();
    void PlaySFX(const std::string& effect_name, float pitch = 1.0f);
    void PlayVoice(const std::string& voice_line);

    bool IsInitialized() const { return m_initialized; }

private:
    AudioSystem() = default;
    ~AudioSystem() = default;

    AudioConfig m_config;
    bool m_initialized = false;
    std::string m_current_bgm;
    bool m_bgm_playing = false;
};

} // namespace personarm

