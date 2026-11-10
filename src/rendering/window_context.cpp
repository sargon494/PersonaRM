#include "rendering/window_context.h"
#include "core/logger.h"
#include <iostream>

namespace personarm {

WindowContext::WindowContext() = default;

WindowContext::~WindowContext() {
    Shutdown();
}

bool WindowContext::Initialize(const WindowConfig& config) {
    m_config = config;

    if (m_config.headless) {
        PRM_LOG_INFO("WINDOW", "WindowContext initialized in headless mode (virtual surface).");
        m_is_open = true;
        return true;
    }

    // Window surface initialization
    PRM_LOG_INFO("WINDOW", "Initializing display surface: " + std::to_string(m_config.width) +
                           "x" + std::to_string(m_config.height) + " Title: " + m_config.title);
    m_is_open = true;
    return true;
}

void WindowContext::Shutdown() {
    if (m_is_open) {
        PRM_LOG_INFO("WINDOW", "Shutting down WindowContext.");
        m_is_open = false;
        m_window_handle = nullptr;
        m_gl_context = nullptr;
    }
}

bool WindowContext::PollEvents() {
    if (!m_is_open) return false;
    // Event loop stub (returns true to continue running)
    return true;
}

void WindowContext::SwapBuffers() {
    if (!m_is_open) return;
    // Buffer swap stub
}

void WindowContext::SetTitle(const std::string& title) {
    m_config.title = title;
}

} // namespace personarm
