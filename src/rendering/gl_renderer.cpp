#include "rendering/gl_renderer.h"
#include "core/logger.h"
#include <algorithm>
#include <cstring>

namespace personarm {

bool GlRenderer::Initialize(WindowContext& window) {
    (void)window;
    m_initialized = true;
    m_texture_buffer.resize(320 * 240, 0xFF000000);
    PRM_LOG_INFO("RENDERER", "GL presentation pipeline initialized.");
    return true;
}

void GlRenderer::Shutdown() {
    m_initialized = false;
    m_texture_buffer.clear();
    PRM_LOG_INFO("RENDERER", "GL presentation pipeline shutdown.");
}

void GlRenderer::UploadFrame(const uint32_t* rgba_data, uint32_t width, uint32_t height) {
    if (!rgba_data || width == 0 || height == 0) return;

    m_current_frame_w = width;
    m_current_frame_h = height;

    size_t num_pixels = static_cast<size_t>(width) * height;
    m_texture_buffer.resize(num_pixels);
    std::memcpy(m_texture_buffer.data(), rgba_data, num_pixels * sizeof(uint32_t));
}

void GlRenderer::CalculateViewport(uint32_t win_w, uint32_t win_h, uint32_t frame_w, uint32_t frame_h) {
    if (win_w == 0 || win_h == 0 || frame_w == 0 || frame_h == 0) return;

    if (m_aspect_mode == AspectRatioMode::FillWindow) {
        m_viewport.x = 0;
        m_viewport.y = 0;
        m_viewport.width = win_w;
        m_viewport.height = win_h;
        return;
    }

    // Determine target aspect ratio based on user configuration
    float target_aspect = 4.0f / 3.0f; // Default authentic PS1 4:3
    if (m_aspect_mode == AspectRatioMode::Widescreen16_9) {
        target_aspect = 16.0f / 9.0f;
    } else if (m_aspect_mode == AspectRatioMode::UltraWide21_9) {
        target_aspect = 21.0f / 9.0f;
    }

    float window_aspect = static_cast<float>(win_w) / static_cast<float>(win_h);

    if (window_aspect > target_aspect) {
        // Window is wider than target -> Pillarboxing (side bars)
        m_viewport.height = win_h;
        m_viewport.width = static_cast<uint32_t>(win_h * target_aspect);
        m_viewport.x = (win_w - m_viewport.width) / 2;
        m_viewport.y = 0;
    } else {
        // Window is taller than target -> Letterboxing (top/bottom bars)
        m_viewport.width = win_w;
        m_viewport.height = static_cast<uint32_t>(win_w / target_aspect);
        m_viewport.x = 0;
        m_viewport.y = (win_h - m_viewport.height) / 2;
    }
}

void GlRenderer::Render(uint32_t window_width, uint32_t window_height) {
    if (!m_initialized) return;

    CalculateViewport(window_width, window_height, m_current_frame_w, m_current_frame_h);
}

} // namespace personarm
