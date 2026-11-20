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

std::vector<uint8_t> Le32s(const uint32_t* w, size_t n) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < n; ++i) for (uint8_t b : Le32(w[i])) out.push_back(b);
    return out;
}

std::string Lower(const std::string& s) {
    std::string t;
    for (char c : s) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

} // namespace

bool ParseTextSpeed(const std::string& text, TextSpeed& out) {
    const std::string t = Lower(text);
    if (t == "normal") out = TextSpeed::Normal;
    else if (t == "fast") out = TextSpeed::Fast;
    else if (t == "instant") out = TextSpeed::Instant;
    else return false;
    return true;
}

const char* TextSpeedName(TextSpeed s) {
    return s == TextSpeed::Fast ? "fast" : s == TextSpeed::Instant ? "instant" : "normal";
}

bool ParseEncounterRate(const std::string& text, EncounterRate& out) {
    const std::string t = Lower(text);
    if (t == "normal") out = EncounterRate::Normal;
    else if (t == "reduced") out = EncounterRate::Reduced;
    else if (t == "none") out = EncounterRate::None;
    else return false;
    return true;
}

const char* EncounterRateName(EncounterRate r) {
    return r == EncounterRate::Reduced ? "reduced" : r == EncounterRate::None ? "none" : "normal";
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
    // Disc 1: text_init stores the per-glyph delay with `sw a2,0x74(a3)` into a global, which
    // msg_win_create copies into each window (+54) and that into each text node
    // (docs/research/TEXT_SYSTEM.md TS-012). Overwrite it after every store. US addresses are the
    // same code shifted (docs/research/DISC_US_FORENSICS.md).
    struct TextSpeedSite { const char* serial; uint32_t store_pc, global; };
    static const TextSpeedSite kSites[] = {
        {"SLPS02785", 0x8001C6A0u, 0x8007CF14u},   // JP Deluxe Pack, text_init 0x8001C678
        {"SLUS01158", 0x8001C734u, 0x8007E634u},   // US, text_init 0x8001C70C
    };
    const std::string disc = Normalize(serial);
    auto unsupported = [&](const char* option, const char* value) {
        PRM_LOG_WARN("GAMEPLAY", std::string(option) + "=" + value + ": no patch for disc " +
                                     (serial.empty() ? "(unrecognized)" : serial) + ", ignored");
    };
    bool found = false;
    for (const auto& site : kSites) {
        if (disc != site.serial || options.text_speed == TextSpeed::Normal) continue;
        found = true;
        GamePatch g;
        g.name = std::string("text_speed=") + TextSpeedName(options.text_speed);
        g.trigger = site.global;
        g.only_if_nonzero = true;   // crt0 clears it to 0: leave that alone
        g.expect.push_back({site.store_pc, {0x74, 0x00, 0xE6, 0xAC}});  // sw a2,0x74(a3) of this EXE
        g.write = {site.global, Le32(options.text_speed == TextSpeed::Fast ? 1u : 0u)};
        patches.push_back(std::move(g));
    }
    if (options.text_speed != TextSpeed::Normal && !found) unsupported("text_speed", TextSpeedName(options.text_speed));

    // Disc 1 random encounters. The field overlay F0736 (slot 0x800AB000 JP, 0x800AF000 US) runs its encounter step once per
    // frame: s0 = distance walked this frame (word 0 below, lhu), halved when event flag 0xC12 is set
    // and doubled when 0xC13 is (words 3..14), then passed to the resident encounter roll (JP
    // 0x80017A04, US 0x800178BC). That adds it to the step accumulator (game state +0x10FC: JP
    // 0x8007E2E4, US 0x8007FA04) and rolls the zone's battle table each 512 units past 2048.
    // Scripted encounters take another path in the same function (s0 = 0x7FFF, untouched) and the
    // battle overlay F0789 calls the roll itself with constants, so both keep working. The patch
    // rewrites overlay code right after each load (trigger = its last word, so the whole range is in
    // RAM by then; other overlays loaded at that address fail the guard). With --recomp the patched
    // overlay fails its code hash and runs interpreted.
    struct EncounterSite { const char* serial; uint32_t base; uint32_t words[15]; };
    static const EncounterSite kEncounter[] = {
        {"SLPS02785", 0x800ABA5Cu, {0x9470A7C8, 0x00021182, 0x3042007F, 0x0C0091C4, 0xA4A21100, 0x10400003, 0x00101400,
                                    0x0802AEAC, 0x00028443, 0x0C0091C4, 0x24040C13, 0x1040000A, 0x3C02800D, 0x0802AEAD,
                                    0x00108040}},
        {"SLUS01158", 0x800AFA5Cu, {0x9470E838, 0x00021182, 0x3042007F, 0x0C009271, 0xA4A21100, 0x10400003, 0x00101400,
                                    0x0802BEAC, 0x00028443, 0x0C009271, 0x24040C13, 0x1040000A, 0x3C02800D, 0x0802BEAD,
                                    0x00108040}},
    };
    found = false;
    for (const auto& site : kEncounter) {
        if (disc != site.serial || options.encounter_rate == EncounterRate::Normal) continue;
        found = true;
        GamePatch g;
        g.name = std::string("encounter_rate=") + EncounterRateName(options.encounter_rate);
        g.trigger = site.base + 14 * 4;
        g.expect.push_back({site.base, Le32s(site.words, 15)});
        if (options.encounter_rate == EncounterRate::None) {
            const uint32_t w = 0x00008021u;  // addu s0,zero,zero: nothing walked, nothing rolled
            g.write = {site.base, Le32s(&w, 1)};
        } else {
            // Halve on every path: flag 0xC12 >>1 becomes >>2, flag 0xC13 <<1 becomes x1, else >>1.
            uint32_t w[7];
            std::copy(site.words + 8, site.words + 15, w);
            w[0] = 0x00028483u;  // +0x20 sra s0,v0,18 (was 17)
            w[3] = 0x1440000Au;  // +0x2C bne v0,zero (was beq): flag 0xC13 skips the halving at +0x38
            w[6] = 0x00108043u;  // +0x38 sra s0,s0,1 (was sll s0,s0,1)
            g.write = {site.base + 8 * 4, Le32s(w, 7)};
        }
        patches.push_back(std::move(g));
    }
    if (options.encounter_rate != EncounterRate::Normal && !found)
        unsupported("encounter_rate", EncounterRateName(options.encounter_rate));

    // Disc 1 movies: overlay F0000 (slot A: 0x80093000 JP, 0x80097000 US) plays STR movie a0 with skip
    // mode a1 (scene mode 10 passes game state +0x10CC/+0x10D0). Each frame of its loop dispatches on
    // the mode: 1 = Start, 2 = Cross, 3 = any button (pad edge bytes 0x80083860[] JP, 0x80084F80[]
    // US, bit 7 = pressed this frame; [14] = Start), 4 = countdown; 0 = no skip (the new game opening,
    // movie 1). A skip calls the stream stop (0x800935D0 JP) and takes the game's own "skipped" exit
    // (screen cleared, then back to the caller as at the end). The patch sends mode 0 to the Start
    // check: its `j <no check>` becomes `j <mode 1 check>`. Trigger = last guarded word.
    struct FmvSite { const char* serial; uint32_t base; uint32_t words[24]; uint32_t jump_to_start_check; };
    static const FmvSite kFmv[] = {
        {"SLPS02785", 0x80093B48u, {0x24020002, 0x12A20016, 0x2AA20003, 0x10400005, 0x24020001, 0x12A2000A, 0x3C088008, 0x08024F0C,
                                    0x00000000, 0x24020003, 0x12A20016, 0x24020004, 0x12A20028, 0x00000000, 0x08024F0C, 0x00000000,
                                    0x25083860, 0x8102000E, 0x00000000, 0x30420080, 0x10400025, 0x00000000, 0x08024F0A, 0x00000000},
         0x08024EE2u},  // j 0x80093B88
        {"SLUS01158", 0x80097B48u, {0x24020002, 0x12A20016, 0x2AA20003, 0x10400005, 0x24020001, 0x12A2000A, 0x3C088008, 0x08025F0C,
                                    0x00000000, 0x24020003, 0x12A20016, 0x24020004, 0x12A20028, 0x00000000, 0x08025F0C, 0x00000000,
                                    0x25084F80, 0x8102000E, 0x00000000, 0x30420080, 0x10400025, 0x00000000, 0x08025F0A, 0x00000000},
         0x08025EE2u},  // j 0x80097B88
    };
    found = false;
    for (const auto& site : kFmv) {
        if (disc != site.serial || !options.skip_fmv) continue;
        found = true;
        GamePatch g;
        g.name = "skip_fmv";
        g.trigger = site.base + 23 * 4;
        g.expect.push_back({site.base, Le32s(site.words, 24)});
        g.write = {site.base + 7 * 4, Le32s(&site.jump_to_start_check, 1)};
        patches.push_back(std::move(g));
    }
    if (options.skip_fmv && !found) unsupported("skip_fmv", "true");
    return patches;
}

} // namespace personarm
