#include "config_io.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace personarm {

namespace {

const char* WindowModeToString(WindowMode m) {
    switch (m) {
        case WindowMode::Windowed: return "windowed";
        case WindowMode::BorderlessFullscreen: return "borderless";
        case WindowMode::ExclusiveFullscreen: return "exclusive";
    }
    return "borderless";
}

std::optional<WindowMode> WindowModeFromString(const std::string& s) {
    if (s == "windowed") return WindowMode::Windowed;
    if (s == "borderless") return WindowMode::BorderlessFullscreen;
    if (s == "exclusive") return WindowMode::ExclusiveFullscreen;
    return std::nullopt;
}

const char* DevicePrefToString(InputDevicePreference p) {
    switch (p) {
        case InputDevicePreference::AutoDetect: return "auto";
        case InputDevicePreference::KeyboardMouse: return "keyboard_mouse";
        case InputDevicePreference::GamepadXbox: return "xbox";
        case InputDevicePreference::GamepadPlayStation: return "playstation";
        case InputDevicePreference::GamepadNintendo: return "nintendo";
    }
    return "auto";
}

std::optional<InputDevicePreference> DevicePrefFromString(const std::string& s) {
    if (s == "auto") return InputDevicePreference::AutoDetect;
    if (s == "keyboard_mouse") return InputDevicePreference::KeyboardMouse;
    if (s == "xbox") return InputDevicePreference::GamepadXbox;
    if (s == "playstation") return InputDevicePreference::GamepadPlayStation;
    if (s == "nintendo") return InputDevicePreference::GamepadNintendo;
    return std::nullopt;
}

uint32_t ClampU32(double v, uint32_t lo, uint32_t hi) {
    if (!(v >= lo)) return lo; // also handles NaN
    if (v > hi) return hi;
    return static_cast<uint32_t>(v);
}

float ClampF(double v, float lo, float hi) {
    if (!(v >= lo)) return lo;
    if (v > hi) return hi;
    return static_cast<float>(v);
}

bool ParseU32(const std::string& s, uint32_t& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 9) return false;
    out = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    return true;
}

bool ParseBool(const std::string& s) { return s == "1" || s == "true"; }

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

JsonValue ConfigToJson(const LauncherConfig& c) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", 2);
    root.Set("auto_launch", c.auto_launch);

    JsonValue video = JsonValue::MakeObject();
    video.Set("width", c.video.width);
    video.Set("height", c.video.height);
    video.Set("window_mode", WindowModeToString(c.video.window_mode));
    video.Set("vsync", c.video.vsync);
    video.Set("target_fps", c.video.target_fps);
    video.Set("widescreen", c.video.enable_widescreen_patch);
    video.Set("internal_resolution_scale", c.video.internal_resolution_scale);
    root.Set("video", std::move(video));

    JsonValue audio = JsonValue::MakeObject();
    audio.Set("master_volume", static_cast<double>(c.audio.master_volume));
    audio.Set("bgm_volume", static_cast<double>(c.audio.bgm_volume));
    audio.Set("sfx_volume", static_cast<double>(c.audio.sfx_volume));
    audio.Set("voice_volume", static_cast<double>(c.audio.voice_volume));
    audio.Set("high_quality_resampling", c.audio.high_quality_resampling);
    root.Set("audio", std::move(audio));

    JsonValue game = JsonValue::MakeObject();
    game.Set("id", c.game.id);
    game.Set("title", c.game.title);
    game.Set("version", c.game.version);
    game.Set("rom_path", c.game.legal_rom_path);
    game.Set("disc1_path", c.game.disc1_path);
    game.Set("disc2_path", c.game.disc2_path);
    game.Set("active_disc", c.game.active_disc);
    game.Set("verified", c.game.is_verified);
    root.Set("game", std::move(game));

    JsonValue input = c.input_profiles.ToJson(); // active_profile + profiles
    input.Set("device_preference", DevicePrefToString(c.input.device_preference));
    input.Set("modern_shortcuts_enabled", c.input.modern_shortcuts_enabled);
    root.Set("input", std::move(input));

    JsonValue tex = JsonValue::MakeObject();
    tex.Set("enabled", c.texture_overrides.enabled);
    tex.Set("directory", c.texture_overrides.directory);
    tex.Set("hash_algorithm", c.texture_overrides.hash_algorithm);
    tex.Set("hash_scope", c.texture_overrides.hash_scope);
    tex.Set("cache_mb", c.texture_overrides.cache_mb);
    root.Set("texture_overrides", std::move(tex));
    return root;
}

size_t ConfigFromJson(const JsonValue& json, LauncherConfig& out) {
    size_t skipped = 0;
    if (!json.IsObject()) return 1;

    out.auto_launch = json.BoolOr("auto_launch", out.auto_launch);

    if (const JsonValue* v = json.Find("video"); v && v->IsObject()) {
        out.video.width = ClampU32(v->NumberOr("width", out.video.width), 320, 16384);
        out.video.height = ClampU32(v->NumberOr("height", out.video.height), 240, 16384);
        if (const JsonValue* m = v->Find("window_mode"); m && m->IsString()) {
            if (auto mode = WindowModeFromString(m->AsString())) out.video.window_mode = *mode; else ++skipped;
        }
        out.video.vsync = v->BoolOr("vsync", out.video.vsync);
        out.video.target_fps = ClampU32(v->NumberOr("target_fps", out.video.target_fps), 15, 1000);
        out.video.enable_widescreen_patch = v->BoolOr("widescreen", out.video.enable_widescreen_patch);
        out.video.internal_resolution_scale = ClampU32(v->NumberOr("internal_resolution_scale", out.video.internal_resolution_scale), 1, 8);
    }

    if (const JsonValue* a = json.Find("audio"); a && a->IsObject()) {
        out.audio.master_volume = ClampF(a->NumberOr("master_volume", out.audio.master_volume), 0.0f, 1.0f);
        out.audio.bgm_volume = ClampF(a->NumberOr("bgm_volume", out.audio.bgm_volume), 0.0f, 1.0f);
        out.audio.sfx_volume = ClampF(a->NumberOr("sfx_volume", out.audio.sfx_volume), 0.0f, 1.0f);
        out.audio.voice_volume = ClampF(a->NumberOr("voice_volume", out.audio.voice_volume), 0.0f, 1.0f);
        out.audio.high_quality_resampling = a->BoolOr("high_quality_resampling", out.audio.high_quality_resampling);
    }

    if (const JsonValue* g = json.Find("game"); g && g->IsObject()) {
        out.game.id = g->StringOr("id", out.game.id);
        out.game.title = g->StringOr("title", out.game.title);
        out.game.version = g->StringOr("version", out.game.version);
        out.game.legal_rom_path = g->StringOr("rom_path", out.game.legal_rom_path);
        out.game.disc1_path = g->StringOr("disc1_path", out.game.disc1_path);
        out.game.disc2_path = g->StringOr("disc2_path", out.game.disc2_path);
        out.game.active_disc = static_cast<int>(ClampF(g->NumberOr("active_disc", out.game.active_disc), 1.0f, 2.0f));
        if (out.game.disc1_path.empty()) out.game.disc1_path = out.game.legal_rom_path;
        if (out.game.legal_rom_path.empty()) out.game.legal_rom_path = out.game.disc1_path;
        out.game.is_verified = g->BoolOr("verified", out.game.is_verified);
    }

    if (const JsonValue* in = json.Find("input"); in && in->IsObject()) {
        if (const JsonValue* d = in->Find("device_preference"); d && d->IsString()) {
            if (auto pref = DevicePrefFromString(d->AsString())) out.input.device_preference = *pref; else ++skipped;
        }
        out.input.modern_shortcuts_enabled = in->BoolOr("modern_shortcuts_enabled", out.input.modern_shortcuts_enabled);
        skipped += out.input_profiles.FromJson(*in);
    }

    if (const JsonValue* t = json.Find("texture_overrides"); t && t->IsObject()) {
        out.texture_overrides.enabled = t->BoolOr("enabled", out.texture_overrides.enabled);
        out.texture_overrides.directory = t->StringOr("directory", out.texture_overrides.directory);
        std::string algo = t->StringOr("hash_algorithm", out.texture_overrides.hash_algorithm);
        std::string scope = t->StringOr("hash_scope", out.texture_overrides.hash_scope);
        if (algo == "fnv1a" || algo == "crc32") out.texture_overrides.hash_algorithm = algo; else ++skipped;
        if (scope == "full" || scope == "pixels") out.texture_overrides.hash_scope = scope; else ++skipped;
        out.texture_overrides.cache_mb = ClampU32(t->NumberOr("cache_mb", out.texture_overrides.cache_mb), 16, 4096);
    }
    return skipped;
}

void ParseLegacyConfig(const std::string& text, LauncherConfig& out) {
    std::istringstream in(text);
    std::string line;
    InputProfile& profile = out.input_profiles.Active();
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        uint32_t u = 0;
        if (key == "width" && ParseU32(val, u)) out.video.width = ClampU32(u, 320, 16384);
        else if (key == "height" && ParseU32(val, u)) out.video.height = ClampU32(u, 240, 16384);
        else if (key == "vsync") out.video.vsync = ParseBool(val);
        else if (key == "target_fps" && ParseU32(val, u)) out.video.target_fps = ClampU32(u, 15, 1000);
        else if (key == "widescreen") out.video.enable_widescreen_patch = ParseBool(val);
        else if (key == "rom_path") { out.game.legal_rom_path = val; if (out.game.disc1_path.empty()) out.game.disc1_path = val; }
        else if (key == "disc1_path") { out.game.disc1_path = val; if (out.game.legal_rom_path.empty()) out.game.legal_rom_path = val; }
        else if (key == "disc2_path") out.game.disc2_path = val;
        else if (key == "active_disc" && ParseU32(val, u)) out.game.active_disc = static_cast<int>(ClampU32(u, 1, 2));
        else if (key == "turbo_multiplier" && ParseU32(val, u)) profile.turbo_speed = ClampU32(u, 2, 4);
        else if (key == "stick_deadzone") profile.stick_deadzone = ClampF(std::atof(val.c_str()), 0.0f, 0.9f);
        else if (key == "mouse_sensitivity") profile.mouse_sensitivity = ClampF(std::atof(val.c_str()), 0.1f, 10.0f);
    }
}

} // namespace personarm
