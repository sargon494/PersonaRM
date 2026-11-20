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
    const auto pu = BuildGamePatches("SLUS_011.58", fast);   // US disc 1: same code, shifted
    CHECK(pu.size() == 1 && pu[0].trigger == 0x8007E634u && pu[0].expect[0].address == 0x8001C734u);
    CHECK(BuildGamePatches("SLUS-01339", fast).empty());

    // Encounter rate: parse, and the field overlay code patch (trigger = last guarded word).
    EncounterRate r = EncounterRate::Normal;
    CHECK(ParseEncounterRate("Reduced", r) && r == EncounterRate::Reduced);
    CHECK(ParseEncounterRate("none", r) && r == EncounterRate::None);
    CHECK(!ParseEncounterRate("half", r) && r == EncounterRate::None);
    CHECK(std::string(EncounterRateName(EncounterRate::Reduced)) == "reduced");
    GameplayOptions none, reduced;
    none.encounter_rate = EncounterRate::None;
    reduced.encounter_rate = EncounterRate::Reduced;
    CHECK(BuildGamePatches("SLPS-02786", none).empty());
    CHECK(BuildGamePatches("", reduced).empty());
    const auto en = BuildGamePatches("SLPS-02785", none), er = BuildGamePatches("SLPS-02785", reduced);
    CHECK(en.size() == 1 && er.size() == 1);
    CHECK(en[0].trigger == 0x800ABA94u && en[0].expect.size() == 1 && en[0].expect[0].address == 0x800ABA5Cu &&
          en[0].expect[0].bytes.size() == 60);
    CHECK(en[0].write.address == 0x800ABA5Cu && (en[0].write.bytes == std::vector<uint8_t>{0x21, 0x80, 0, 0}));  // addu s0,zero,zero
    // reduced: words +0x20..+0x38, three of them changed (sra 18, bne, sra 1), the rest as guarded
    CHECK(er[0].write.address == 0x800ABA7Cu && er[0].write.bytes.size() == 28);
    const auto& eb = er[0].write.bytes;
    const auto& orig = er[0].expect[0].bytes;
    auto word = [](const std::vector<uint8_t>& b, size_t at) { return uint32_t(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24); };
    CHECK(word(eb, 0) == 0x00028483u && word(eb, 12) == 0x1440000Au && word(eb, 24) == 0x00108043u);
    for (size_t i : {4u, 8u, 16u, 20u}) CHECK(word(eb, i) == word(orig, 0x20 + i));
    CHECK(word(orig, 0) == 0x9470A7C8u && word(orig, 0x38) == 0x00108040u);
    const auto eu = BuildGamePatches("SLUS-01158", reduced);   // US: overlay linked at 0x800AF000
    CHECK(eu.size() == 1 && eu[0].trigger == 0x800AFA94u && eu[0].write.address == 0x800AFA7Cu);
    GameplayOptions both = fast;
    both.encounter_rate = EncounterRate::None;
    CHECK(BuildGamePatches("SLPS-02785", both).size() == 2);

    // Skip FMV: F0000's "no skip" jump (mode 0) retargeted to the Start check.
    GameplayOptions skip;
    skip.skip_fmv = true;
    CHECK(BuildGamePatches("SLPS-02786", skip).empty());
    const auto sj = BuildGamePatches("SLPS-02785", skip), su = BuildGamePatches("SLUS-01158", skip);
    CHECK(sj.size() == 1 && su.size() == 1 && sj[0].name == "skip_fmv");
    CHECK(sj[0].trigger == 0x80093BA4u && sj[0].expect[0].address == 0x80093B48u && sj[0].expect[0].bytes.size() == 96);
    CHECK(sj[0].write.address == 0x80093B64u && word(sj[0].write.bytes, 0) == 0x08024EE2u);   // j 0x80093B88
    CHECK(word(sj[0].expect[0].bytes, 7 * 4) == 0x08024F0Cu);                                   // was j 0x80093C30
    CHECK(su[0].write.address == 0x80097B64u && word(su[0].write.bytes, 0) == 0x08025EE2u);   // US: slot 0x80097000

    std::printf("test_gameplay_patches: %d failures\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
