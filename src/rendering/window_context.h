#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "rendering/renderer.h"

namespace personarm {

struct WindowConfig {
    std::string title = "Persona 2: Eternal Punishment - PersonaRM";
    uint32_t width = 1280;
    uint32_t height = 960;
    bool fullscreen = false;
    bool vsync = true;
    bool headless = false; // Headless mode for CI and automated testing
};

class WindowContext {
public:
    WindowContext();
    ~WindowContext();

    bool Initialize(const WindowConfig& config = WindowConfig{});
    void Shutdown();

    /// Polls system OS events (returns false if quit requested)
    bool PollEvents();

    /// Swaps front/back framebuffers (present to display)
    void SwapBuffers();

    /// Updates window title (e.g., FPS / Frame counter)
    void SetTitle(const std::string& title);

    bool IsOpen() const { return m_is_open; }
    uint32_t GetWidth() const { return m_config.width; }
    uint32_t GetHeight() const { return m_config.height; }
    bool IsHeadless() const { return m_config.headless; }

private:
    WindowConfig m_config;
    bool m_is_open = false;
    void* m_window_handle = nullptr;
    void* m_gl_context = nullptr;
};

} // namespace personarm
