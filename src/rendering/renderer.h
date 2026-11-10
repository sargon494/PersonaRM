#pragma once

#include <cstdint>
#include <string>
#include <memory>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────
// Render Configuration & Modes
// ───────────────────────────────────────────────────────────────────────

enum class AspectRatioMode {
    Original4x3,
    Widescreen16x9,
    Ultrawide21x9,
    StretchToWindow
};

struct Viewport {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 1920;
    int32_t height = 1080;
};

struct RendererConfig {
    uint32_t window_width = 1920;
    uint32_t window_height = 1080;
    bool vsync = true;
    uint32_t target_fps = 60;
    AspectRatioMode aspect_ratio = AspectRatioMode::Widescreen16x9;
    uint32_t internal_scale = 2; // 1x = 320x240 / 640x480, 2x = 1080p, 4x = 4K
    bool fullscreen = false;
};

// ───────────────────────────────────────────────────────────────────────
// Renderer Class
// ───────────────────────────────────────────────────────────────────────

class Renderer {
public:
    static Renderer& Instance();

    bool Initialize(const RendererConfig& config = RendererConfig{});
    void Shutdown();

    // Frame Lifecycle
    void BeginFrame();
    void EndFrame();

    // Viewport & Resolution
    void SetWindowSize(uint32_t width, uint32_t height);
    Viewport CalculateLetterboxViewport(uint32_t win_w, uint32_t win_h, AspectRatioMode mode) const;
    const Viewport& GetCurrentViewport() const { return m_current_viewport; }

    // Settings
    void SetVSync(bool enabled);
    void SetTargetFPS(uint32_t fps);
    uint32_t GetTargetFPS() const { return m_config.target_fps; }
    bool IsVSyncEnabled() const { return m_config.vsync; }

    // Framerate & Delta Time
    float GetDeltaTime() const { return m_delta_time; }
    float GetCurrentFPS() const { return m_current_fps; }

    bool IsInitialized() const { return m_initialized; }

private:
    Renderer() = default;
    ~Renderer() = default;

    RendererConfig m_config;
    Viewport m_current_viewport;

    bool m_initialized = false;
    float m_delta_time = 0.0166f;
    float m_current_fps = 60.0f;
    uint64_t m_frame_count = 0;
};

} // namespace personarm

