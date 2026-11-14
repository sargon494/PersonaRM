// Gameplay options: manifest parsing (gameplay.text_speed), mod folder priority/serial, and the
// per-disc patch list. The engine side (store-triggered patches, guards, recompiled equivalence) is
// in test_recomp_engine.cpp.
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "engine/gameplay_patches.h"

using namespace personarm;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void WriteManifest(const fs::path& dir, const std::string& name, const std::string& json) {
    fs::create_directories(dir / name);
    std::ofstream(dir / name / "manifest.json") << json;
}

int main() {
    TextSpeed s = TextSpeed::Normal;
    CHECK(ParseTextSpeed("Fast", s) && s == TextSpeed::Fast);
    CHECK(ParseTextSpeed("instant", s) && s == TextSpeed::Instant);
    CHECK(!ParseTextSpeed("1.5", s) && s == TextSpeed::Instant);

    GameplayOptions o;
    std::string err;
    CHECK(ParseGameplayManifest(R"({"format":1,"gameplay":{"text_speed":"fast"}})", o, &err) && o.text_speed == TextSpeed::Fast);
    o = GameplayOptions{};
    CHECK(ParseGameplayManifest(R"({"priority":3})", o) && o.text_speed == TextSpeed::Normal);   // absent: untouched
    CHECK(!ParseGameplayManifest(R"({"gameplay":{"text_speed":"warp"}})", o, &err) && !err.empty());
    CHECK(!ParseGameplayManifest(R"({"gameplay":{"text_speed":1.5}})", o));
    CHECK(!ParseGameplayManifest(R"({"gameplay":7})", o));
    CHECK(!ParseGameplayManifest("{not json", o));

    // Mod folders: serial filter (any punctuation), highest priority wins, ties by folder name, bad ones skipped.
    const fs::path dir = fs::temp_directory_path() / "prm-gameplay-test";
    fs::remove_all(dir);
    WriteManifest(dir, "a_low", R"({"priority":1,"gameplay":{"text_speed":"fast"}})");
    WriteManifest(dir, "b_high", R"({"priority":5,"serial":["SLPS_027.85"],"gameplay":{"text_speed":"instant"}})");
    WriteManifest(dir, "c_other_disc", R"({"priority":9,"serial":["SLPS_027.86"],"gameplay":{"text_speed":"normal"}})");
    WriteManifest(dir, "d_bad", R"({"priority":99,"gameplay":{"text_speed":"warp"}})");
    WriteManifest(dir, "e_none", R"({"priority":99})");
    CHECK(LoadGameplayMods(dir.string(), "SLPS-02785").text_speed == TextSpeed::Instant);
    CHECK(LoadGameplayMods(dir.string(), "SLPS-02786").text_speed == TextSpeed::Normal);   // disc 2's mod wins there
    CHECK(LoadGameplayMods((dir / "missing").string(), "SLPS-02785").text_speed == TextSpeed::Normal);
    fs::remove_all(dir);
    WriteManifest(dir, "x", R"({"priority":2,"gameplay":{"text_speed":"fast"}})");
    WriteManifest(dir, "y", R"({"priority":2,"gameplay":{"text_speed":"instant"}})");
    CHECK(LoadGameplayMods(dir.string(), "").text_speed == TextSpeed::Fast);   // tie: first folder name
    fs::remove_all(dir);

    // Patch list: nothing by default or for other discs; disc 1 writes the global after text_init's store.
    GameplayOptions fast, instant;
    fast.text_speed = TextSpeed::Fast;
    instant.text_speed = TextSpeed::Instant;
    CHECK(BuildGamePatches("SLPS-02785", GameplayOptions{}).empty());
    CHECK(BuildGamePatches("SLPS-02786", fast).empty());
    CHECK(BuildGamePatches("", fast).empty());
    const auto pf = BuildGamePatches("SLPS_027.85", fast), pi = BuildGamePatches("SLPS-02785", instant);
    CHECK(pf.size() == 1 && pi.size() == 1);
    CHECK(pf[0].trigger == 0x8007CF14u && pf[0].write.address == 0x8007CF14u && pf[0].only_if_nonzero);
    CHECK((pf[0].write.bytes == std::vector<uint8_t>{1, 0, 0, 0}) && (pi[0].write.bytes == std::vector<uint8_t>{0, 0, 0, 0}));
    CHECK(pf[0].expect.size() == 1 && pf[0].expect[0].address == 0x8001C6A0u);

    std::printf("test_gameplay_patches: %d failures\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
