#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────
// Display & Video Settings
// ───────────────────────────────────────────────────────────────────────

enum class WindowMode {
    Windowed,
    BorderlessFullscreen,
    ExclusiveFullscreen
};

struct VideoSettings {
    uint32_t width = 1920;
    uint32_t height = 1080;
    WindowMode window_mode = WindowMode::BorderlessFullscreen;
    bool vsync = true;
    uint32_t target_fps = 60;
    bool enable_widescreen_patch = true;
    uint32_t internal_resolution_scale = 2; // 1x (Original), 2x (1080p), 4x (4K)
};

// ───────────────────────────────────────────────────────────────────────
// Control & Input Settings
// ───────────────────────────────────────────────────────────────────────

enum class InputDevicePreference {
    AutoDetect,
    KeyboardMouse,
    GamepadXbox,
    GamepadPlayStation,
    GamepadNintendo
};

struct InputSettings {
    InputDevicePreference device_preference = InputDevicePreference::AutoDetect;
    float stick_deadzone = 0.15f;
    float mouse_sensitivity = 1.0f;
    bool invert_camera_y = false;
    bool enable_turbo_hold = true;
    uint32_t turbo_multiplier = 3; // 2x, 3x, 4x
    bool modern_shortcuts_enabled = true; // 1-6 keys for battle commands
};

// ───────────────────────────────────────────────────────────────────────
// Audio Settings
// ───────────────────────────────────────────────────────────────────────

struct AudioSettings {
    float master_volume = 1.0f;
    float bgm_volume = 0.8f;
    float sfx_volume = 0.9f;
    float voice_volume = 1.0f;
    bool high_quality_resampling = true;
};

// ───────────────────────────────────────────────────────────────────────
// Game Target & Mod Configuration
// ───────────────────────────────────────────────────────────────────────

struct GameMetadata {
    std::string id = "P2EP";
    std::string title = "Persona 2: Eternal Punishment";
    std::string version = "1.0.0";
    std::string legal_rom_path = "";
    std::string disc1_path = "";
    std::string disc2_path = "";
    int active_disc = 1;
    bool is_verified = false;
};

// ───────────────────────────────────────────────────────────────────────
// Launcher Configuration Hub
// ───────────────────────────────────────────────────────────────────────

struct LauncherConfig {
    VideoSettings video;
    InputSettings input;
    AudioSettings audio;
    GameMetadata game;
    bool auto_launch = false;
};

// ───────────────────────────────────────────────────────────────────────
// PersonaRM Launcher Class
// ───────────────────────────────────────────────────────────────────────

class Launcher {
public:
    Launcher();
    ~Launcher() = default;

    // Lifecycle
    bool Initialize();
    void RunInteractiveMenu();
    int LaunchGame();

    // Configuration Management
    bool LoadConfig(const std::string& config_path = "personarm_config.json");
    bool SaveConfig(const std::string& config_path = "personarm_config.json") const;
    LauncherConfig& GetConfig() { return m_config; }
    const LauncherConfig& GetConfig() const { return m_config; }

    // Game Asset Detection
    std::vector<std::string> ScanForGameBinaries(const std::string& search_dir = ".");
    bool VerifyGameCopy(const std::string& path);

    // Diagnostics & Self-test
    int RunDiagnostics();

private:
    LauncherConfig m_config;
    bool m_initialized = false;
};

} // namespace personarm

