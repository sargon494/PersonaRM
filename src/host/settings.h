#pragma once
// Host settings file (JSON, see docs/SETTINGS.md). Pure logic, no SDL: unit-tested in
// tests/unit/test_host_settings.cpp. Precedence: defaults < settings file < command line.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json_lite.h"
#include "host/host_logic.h"

namespace personarm::host {

constexpr int kSettingsVersion = 1;

struct VideoSettings {
    std::string window_mode = "windowed";  // windowed | fullscreen | borderless
    int window_scale = 3;                  // windowed client size = 320x240 * scale (1..8)
    bool integer_scaling = false;
    std::string aspect = "4:3";            // 4:3 | stretch
    bool vsync = false;
    int fps_target = 60;                   // 30 | 60 | 90 | 120; >60 = --interp, 30 = every other frame presented
    std::string renderer = "software";     // software | gpu (host GPU, docs/GPU_RENDERER.md)
    int resolution_scale = 1;              // internal resolution (display only): software 1..4, gpu 1..9; 0 = "auto" (gpu)
    int fast_forward_max = 4;              // 2..16: fast-forward speed cap (x real time)
    std::string widescreen = "off";        // off | W:H wider than 4:3 (16:9, 21:9...): GTE widescreen, docs/WIDESCREEN.md
};
/// --interp value behind fps_target: 90, 120... (above 60), else 0 (off).
inline int InterpHz(const VideoSettings& v) { return v.fps_target > 60 ? v.fps_target : 0; }
/// Pictures presented per second: fps_target 30 (every other frame), 60 (native) or the interpolated rate.
inline int PresentHz(const VideoSettings& v) { return v.fps_target == 30 ? 30 : InterpHz(v) ? InterpHz(v) : 60; }
struct AudioSettings { int master_volume = 100; bool mute = false; };
/// One game, keyed by disc serial. Empty string / nullopt = not set.
struct GameSettings {
    std::string memcard[2], text_speed, encounter_rate, mods;
    std::optional<bool> recomp, skip_fmv;
    int battle_speed = 0;  // 1..3, 0 = not set
    bool Empty() const {
        return memcard[0].empty() && memcard[1].empty() && text_speed.empty() && encounter_rate.empty() && mods.empty() &&
               !recomp && !skip_fmv && !battle_speed;
    }
};

struct Settings {
    int version = kSettingsVersion;
    VideoSettings video;
    AudioSettings audio;
    bool pause_on_focus_loss = true;
    std::string language = "ja";  // stored only, no effect yet
    std::string save_dir;         // memory card directory (see MemcardFor)
    /// action -> key/button names (SDL spelling). Always the full set: defaults, overridden per action.
    std::map<std::string, std::vector<std::string>> keyboard, gamepad;
    std::map<std::string, GameSettings> games;
    JsonValue raw = JsonValue::MakeObject();  // the document as read: unknown keys survive a write
};

/// Defaults (keyboard/gamepad filled from KeyboardMap()/GamepadMap()).
Settings DefaultSettings();
/// Parse a document. Never fails: a bad document or value gives a warning and the default.
Settings ParseSettings(const std::string& text, std::vector<std::string>* warnings);
/// Read `path`. A missing file is not an error (defaults, no warning).
Settings LoadSettings(const std::string& path, std::vector<std::string>* warnings);
std::string DumpSettings(const Settings& s);
/// Temp file + rename, creates the directory. False (and *err) if the file could not be replaced.
bool SaveSettings(const std::string& path, const Settings& s, std::string* err = nullptr);

/// Command line over file: returns `s` with every option the user gave folded in (game options under
/// `serial`, "" = not known yet), so it can also be written back (--save-settings).
Settings ApplyCli(Settings s, const HostOptions& o, const std::string& serial);
GameSettings GameFor(const Settings& s, const std::string& serial);

/// Card path for a slot: the game's explicit memcard, else (slot 0, known serial, save_dir set)
/// <save_dir>/<serial>-1.mcd, else "" (no card).
std::string MemcardFor(const Settings& s, const GameSettings& g, const std::string& serial, int slot);

struct Binding { std::string name; uint16_t bit; };
/// Flat name -> pad bit list of a keyboard/gamepad map (several names per action are fine).
std::vector<Binding> FlattenBindings(const std::map<std::string, std::vector<std::string>>& m);

} // namespace personarm::host
