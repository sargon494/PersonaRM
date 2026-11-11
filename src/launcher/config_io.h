#pragma once

// (De)serialization of LauncherConfig <-> personarm_config.json.
// Format (version 2):
//   { "version": 2, "auto_launch": false,
//     "video": {...}, "audio": {...}, "game": {...},
//     "input": { "device_preference": "auto", "modern_shortcuts_enabled": true,
//                "active_profile": "...", "profiles": [ ... ] },
//     "texture_overrides": { "enabled": true, "directory": "...", ... } }

#include "core/json_lite.h"
#include "launcher/launcher.h"

#include <string>

namespace personarm {

JsonValue ConfigToJson(const LauncherConfig& config);

/// Fills `out` from JSON. Missing keys keep the values already in `out`; invalid or out-of-range
/// values are skipped (or clamped). Returns the number of ignored entries.
size_t ConfigFromJson(const JsonValue& json, LauncherConfig& out);

/// Parses the pre-JSON "key=value" format. Unknown keys and unparsable numbers are ignored.
void ParseLegacyConfig(const std::string& text, LauncherConfig& out);

} // namespace personarm
