#include "engine/gameplay_patches.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json_lite.h"
#include "core/logger.h"

namespace fs = std::filesystem;

namespace personarm {
namespace {

std::string Normalize(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::vector<uint8_t> Le32(uint32_t v) {
    return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
}

} // namespace

bool ParseTextSpeed(const std::string& text, TextSpeed& out) {
    std::string t;
    for (char c : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "normal") out = TextSpeed::Normal;
    else if (t == "fast") out = TextSpeed::Fast;
    else if (t == "instant") out = TextSpeed::Instant;
    else return false;
    return true;
}

const char* TextSpeedName(TextSpeed s) {
    return s == TextSpeed::Fast ? "fast" : s == TextSpeed::Instant ? "instant" : "normal";
}

bool ParseGameplayManifest(const std::string& manifest_json, GameplayOptions& out, std::string* err) {
    std::string e;
    auto json = JsonValue::Parse(manifest_json, &e);
    if (!json || !json->IsObject()) {
        if (err) *err = e.empty() ? "manifest is not a JSON object" : e;
        return false;
    }
    const JsonValue* gp = json->Find("gameplay");
    if (!gp) return true;
    if (!gp->IsObject()) { if (err) *err = "\"gameplay\" must be an object"; return false; }
    if (const JsonValue* ts = gp->Find("text_speed")) {
        if (!ts->IsString() || !ParseTextSpeed(ts->AsString(), out.text_speed)) {
            if (err) *err = "gameplay.text_speed must be \"normal\", \"fast\" or \"instant\"";
            return false;
        }
    }
    return true;
}

GameplayOptions LoadGameplayMods(const std::string& mods_dir, const std::string& serial) {
    struct Found { int priority; TextSpeed speed; };
    std::vector<Found> speeds;  // in folder-name order
    std::error_code ec;
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(mods_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());
    for (const fs::path& f : folders) {
        std::ifstream in(f / "manifest.json");
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        auto json = JsonValue::Parse(ss.str(), &err);
        if (!json || !json->IsObject()) continue;  // VramTextureHook::LoadMods already warns
        const JsonValue* list = json->Find("serial");
        if (list && list->IsArray() && !serial.empty()) {
            bool match = false;
            for (const auto& s : list->Items()) match = match || Normalize(s.AsString()) == Normalize(serial);
            if (!match) continue;
        }
        GameplayOptions o;
        if (!ParseGameplayManifest(ss.str(), o, &err)) {
            PRM_LOG_WARN("GAMEPLAY", "mod " + f.filename().string() + ": " + err);
            continue;
        }
        const JsonValue* gp = json->Find("gameplay");
        if (gp && gp->Find("text_speed")) speeds.push_back({int(json->NumberOr("priority", 0)), o.text_speed});
    }
    GameplayOptions result;
    // max_element keeps the first of equal priorities: the first folder name.
    if (!speeds.empty())
        result.text_speed = std::max_element(speeds.begin(), speeds.end(),
                                             [](const Found& a, const Found& b) { return a.priority < b.priority; })->speed;
    return result;
}

std::vector<GamePatch> BuildGamePatches(const std::string& serial, const GameplayOptions& options) {
    std::vector<GamePatch> patches;
    // Disc 1 (JP Deluxe Pack): text_init (0x8001C678) stores the per-glyph delay with `sw a2,0x74(a3)` at
    // 0x8001C6A0 into the global 0x8007CF14, which msg_win_create copies into each window (+54) and
    // that into each text node (docs/research/TEXT_SYSTEM.md TS-012). Overwrite it after every store.
    if (Normalize(serial) == "SLPS02785" && options.text_speed != TextSpeed::Normal) {
        GamePatch g;
        g.name = std::string("text_speed=") + TextSpeedName(options.text_speed);
        g.trigger = 0x8007CF14u;
        g.only_if_nonzero = true;   // crt0 clears it to 0: leave that alone
        g.expect.push_back({0x8001C6A0u, {0x74, 0x00, 0xE6, 0xAC}});  // sw a2,0x74(a3) of this EXE
        g.write = {0x8007CF14u, Le32(options.text_speed == TextSpeed::Fast ? 1u : 0u)};
        patches.push_back(std::move(g));
    }
    return patches;
}

} // namespace personarm
