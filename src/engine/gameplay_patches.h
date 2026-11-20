#pragma once
// Per-game gameplay options (text speed, ...) as GamePatch lists for Engine::SetGamePatches.
// Options come from mod manifests ("gameplay": {"text_speed": "fast"}) and from the host command line.

#include <string>
#include <vector>

#include "engine/engine.h"

namespace personarm {

/// Delay per glyph of the message windows: the game's own value, 1, or 0 (whole message at once).
enum class TextSpeed { Normal, Fast, Instant };

/// Random field encounters: the game's rate, about half as many, or none (scripted and boss battles stay).
enum class EncounterRate { Normal, Reduced, None };

struct GameplayOptions {
    TextSpeed text_speed = TextSpeed::Normal;
    EncounterRate encounter_rate = EncounterRate::Normal;
};

/// "normal" | "fast" | "instant" (case-insensitive). False for anything else (`out` untouched).
bool ParseTextSpeed(const std::string& text, TextSpeed& out);
const char* TextSpeedName(TextSpeed s);
/// "normal" | "reduced" | "none" (case-insensitive). False for anything else (`out` untouched).
bool ParseEncounterRate(const std::string& text, EncounterRate& out);
const char* EncounterRateName(EncounterRate r);

/// Reads "gameplay" of one manifest.json text into `out`; fields that are absent stay as they are.
/// False (and `err`) on invalid JSON or an invalid value.
bool ParseGameplayManifest(const std::string& manifest_json, GameplayOptions& out, std::string* err = nullptr);

/// Gameplay options of the mods under `mods_dir` (<dir>/<mod>/manifest.json, as VramTextureHook::LoadMods):
/// mods whose "serial" list excludes `serial` are ignored; for each option the mod with the highest
/// "priority" that sets it wins, ties by folder name.
GameplayOptions LoadGameplayMods(const std::string& mods_dir, const std::string& serial);

/// Patches implementing `options` for the disc `serial` ("SLPS-02785", any punctuation). Empty when
/// every option is at its default or the game has no patch for it: the engine then runs untouched.
/// An option the disc has no patch for is logged and left out.
std::vector<GamePatch> BuildGamePatches(const std::string& serial, const GameplayOptions& options);

} // namespace personarm
