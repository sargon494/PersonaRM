#include "renderer.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>

namespace personarm {

Renderer& Renderer::Instance() {
    static Renderer instance;
    return instance;
}

bool Renderer::Initialize(const RendererConfig& config) {
    m_config = config;
    m_current_viewport = CalculateLetterboxViewport(config.window_width, config.window_height, config.aspect_ratio);

    m_delta_time = 1.0f / static_cast<float>(config.target_fps > 0 ? config.target_fps : 60);
    m_current_fps = static_cast<float>(config.target_fps > 0 ? config.target_fps : 60);
    m_frame_count = 0;

    m_initialized = true;
    return true;
}

void Renderer::Shutdown() {
    m_initialized = false;
}

Viewport Renderer::CalculateLetterboxViewport(uint32_t win_w, uint32_t win_h, AspectRatioMode mode) const {
    Viewport vp;
    if (win_w == 0 || win_h == 0) return vp;

    if (mode == AspectRatioMode::StretchToWindow) {
        vp.x = 0;
        vp.y = 0;
        vp.width = win_w;
        vp.height = win_h;
        return vp;
    }

    float target_aspect = 16.0f / 9.0f;
    if (mode == AspectRatioMode::Original4x3) {
        target_aspect = 4.0f / 3.0f;
    } else if (mode == AspectRatioMode::Ultrawide21x9) {
        target_aspect = 21.0f / 9.0f;
    }

    float win_aspect = static_cast<float>(win_w) / static_cast<float>(win_h);

    if (win_aspect > target_aspect) {
        // Window is wider than target -> pillarbox (black bars left/right)
        vp.height = win_h;
        vp.width = static_cast<int32_t>(win_h * target_aspect);
        vp.x = static_cast<int32_t>((win_w - vp.width) / 2);
        vp.y = 0;
    } else {
        // Window is taller than target -> letterbox (black bars top/bottom)
        vp.width = win_w;
        vp.height = static_cast<int32_t>(win_w / target_aspect);
        vp.x = 0;
        vp.y = static_cast<int32_t>((win_h - vp.height) / 2);
    }

    return vp;
}

void Renderer::SetWindowSize(uint32_t width, uint32_t height) {
    m_config.window_width = width;
    m_config.window_height = height;
    m_current_viewport = CalculateLetterboxViewport(width, height, m_config.aspect_ratio);
}

void Renderer::SetVSync(bool enabled) {
    m_config.vsync = enabled;
}

void Renderer::SetTargetFPS(uint32_t fps) {
    m_config.target_fps = fps > 0 ? fps : 60;
}

void Renderer::BeginFrame() {
    if (!m_initialized) return;
    // Frame setup / clear buffers
}

void Renderer::EndFrame() {
    if (!m_initialized) return;
    m_frame_count++;
}

} // namespace personarm
