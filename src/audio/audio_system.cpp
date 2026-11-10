#include "audio_system.h"
#include <iostream>
#include <algorithm>

namespace personarm {

AudioSystem& AudioSystem::Instance() {
    static AudioSystem instance;
    return instance;
}

bool AudioSystem::Initialize(const AudioConfig& config) {
    m_config = config;
    m_initialized = true;
    return true;
}

void AudioSystem::Shutdown() {
    StopBGM();
    m_initialized = false;
}

void AudioSystem::SetChannelVolume(AudioChannel channel, float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    switch (channel) {
        case AudioChannel::Master: m_config.master_volume = volume; break;
        case AudioChannel::BGM:    m_config.bgm_volume = volume;    break;
        case AudioChannel::SFX:    m_config.sfx_volume = volume;    break;
        case AudioChannel::Voice:  m_config.voice_volume = volume;  break;
    }
}

float AudioSystem::GetChannelVolume(AudioChannel channel) const {
    switch (channel) {
        case AudioChannel::Master: return m_config.master_volume;
        case AudioChannel::BGM:    return m_config.bgm_volume;
        case AudioChannel::SFX:    return m_config.sfx_volume;
        case AudioChannel::Voice:  return m_config.voice_volume;
        default:                   return 1.0f;
    }
}

float AudioSystem::GetEffectiveVolume(AudioChannel channel) const {
    if (channel == AudioChannel::Master) {
        return m_config.master_volume;
    }
    return m_config.master_volume * GetChannelVolume(channel);
}

void AudioSystem::PlayBGM(const std::string& track_name, bool loop) {
    (void)loop;
    m_current_bgm = track_name;
    m_bgm_playing = true;
}

void AudioSystem::StopBGM() {
    m_bgm_playing = false;
    m_current_bgm.clear();
}

void AudioSystem::PlaySFX(const std::string& effect_name, float pitch) {
    (void)effect_name;
    (void)pitch;
}

void AudioSystem::PlayVoice(const std::string& voice_line) {
    (void)voice_line;
}

} // namespace personarm
