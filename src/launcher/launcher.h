#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include "input/input_profile.h"

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
    bool modern_shortcuts_enabled = true; // 1-6 keys for battle commands
    // Sensitivity, deadzone, turbo speed and key/button bindings live in
    // LauncherConfig::input_profiles (one set of values per profile).
};

// ───────────────────────────────────────────────────────────────────────
// HD Texture Override Settings (see rendering/texture_override.h)
// ───────────────────────────────────────────────────────────────────────

struct TextureOverrideSettings {
    bool enabled = true;
    std::string directory = "texture_packs/hd";
    std::string hash_algorithm = "fnv1a"; // "fnv1a" | "crc32"
    std::string hash_scope = "full";      // "full" | "pixels"
    uint32_t cache_mb = 256;
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
    bool is_verified = false;
};

// ───────────────────────────────────────────────────────────────────────
// Launcher Configuration Hub
// ───────────────────────────────────────────────────────────────────────

struct LauncherConfig {
    VideoSettings video;
    InputSettings input;
    InputProfileManager input_profiles;
    TextureOverrideSettings texture_overrides;
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

