#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "rendering/renderer.h"
#include "rendering/window_context.h"

namespace personarm {

struct ViewportRect {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class GlRenderer {
public:
    GlRenderer() = default;
    ~GlRenderer() = default;

    bool Initialize(WindowContext& window);
    void Shutdown();

    /// Uploads an RGBA8888 frame buffer (from PS1 GPU / Engine) into texture
    void UploadFrame(const uint32_t* rgba_data, uint32_t width, uint32_t height);

    /// Renders the frame with aspect ratio preservation (letterbox)
    void Render(uint32_t window_width, uint32_t window_height);

    ViewportRect GetCurrentViewport() const { return m_viewport; }

    void SetAspectRatioMode(AspectRatioMode mode) { m_aspect_mode = mode; }
    AspectRatioMode GetAspectRatioMode() const { return m_aspect_mode; }

private:
    void CalculateViewport(uint32_t win_w, uint32_t win_h, uint32_t frame_w, uint32_t frame_h);

    bool m_initialized = false;
    AspectRatioMode m_aspect_mode = AspectRatioMode::Original4x3;
    uint32_t m_current_frame_w = 320;
    uint32_t m_current_frame_h = 240;
    ViewportRect m_viewport;

    // Local cached frame texture pixels
    std::vector<uint32_t> m_texture_buffer;
};

} // namespace personarm
