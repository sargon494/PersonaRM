// Phase 7 backend tests: hashing, JSON, PNG, texture overrides, input profiles, mouse navigation, config IO.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "core/hash.h"
#include "core/json_lite.h"
#include "input/input_manager.h"
#include "input/input_profile.h"
#include "input/mouse_navigation.h"
#include "launcher/config_io.h"
#include "loader/png_codec.h"
#include "rendering/texture_override.h"

using namespace personarm;
namespace fs = std::filesystem;

static int g_tests = 0;
#define CHECK(c) do { ++g_tests; if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static std::vector<uint8_t> MakeTim16(uint16_t w, uint16_t h, uint8_t fill, size_t padding = 0) {
    std::vector<uint8_t> t;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) t.push_back(uint8_t(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { t.push_back(uint8_t(v)); t.push_back(uint8_t(v >> 8)); };
    u32(0x10); u32(2);
    u32(12 + w * 2u * h); u16(0); u16(0); u16(w); u16(h);
    for (size_t i = 0; i < size_t(w) * 2 * h; ++i) t.push_back(uint8_t(fill + i));
    t.insert(t.end(), padding, 0);
    return t;
}

static void TestHash() {
    const char* s = "123456789";
    auto p = reinterpret_cast<const uint8_t*>(s);
    CHECK(Crc32(p, 9) == 0xCBF43926u);
    CHECK(Adler32(p, 9) == 0x091E01DEu);
    CHECK(Fnv1a32(p, 9) == 0xBB86B11Cu);
    CHECK(Fnv1a32(nullptr, 0) == 0x811C9DC5u);
}

static void TestJson() {
    std::string err;
    auto j = JsonValue::Parse(R"({"a":[1,2.5,true,null,"x\u00e9\ud83d\ude00"],"b":{"c":-3e2}})", &err);
    CHECK(j && j->IsObject());
    CHECK(j->Find("a")->Items().size() == 5);
    CHECK(j->Find("a")->Items()[4].AsString() == "x\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(j->Find("b")->NumberOr("c", 0) == -300.0);
    auto again = JsonValue::Parse(j->Dump(2));
    CHECK(again && again->Dump(-1) == j->Dump(-1));
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "\"\\x\"", "{\"a\":1} x", "01x", "\"\\ud800\"", "nul"}) {
        CHECK(!JsonValue::Parse(bad));
    }
    CHECK(!JsonValue::Parse(std::string(200, '[')));
}

static void TestPng() {
    std::vector<uint8_t> px(5 * 3 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7);
    auto png = PngCodec::Encode(5, 3, px.data());
    auto img = PngCodec::Decode(png.data(), png.size());
    CHECK(img && img->width == 5 && img->height == 3 && img->rgba == px);
    auto bad = png; bad[bad.size() / 2] ^= 0xFF;
    CHECK(!PngCodec::Decode(bad.data(), bad.size()));
    CHECK(!PngCodec::Decode(png.data(), png.size() - 30));
    CHECK(!PngCodec::Decode(png.data(), 4));
    // Compressed fixture produced by PIL (2x2 RGBA, filtered + deflated), see tests/unit/fixtures.
    static const uint8_t k[] = {
#include "fixtures/png_2x2_rgba.inc"
    };
    auto f = PngCodec::Decode(k, sizeof(k));
    CHECK(f && f->width == 2 && f->height == 2);
    CHECK(f->rgba[0] == 255 && f->rgba[3] == 255 && f->rgba[4 + 1] == 255 && f->rgba[8 + 2] == 255 && f->rgba[12 + 3] == 128);
}

static void TestTimAndOverrides() {
    auto tim = MakeTim16(4, 4, 1);
    auto layout = ParseTimLayout(tim.data(), tim.size());
    CHECK(layout && layout->width_pixels == 4 && layout->height == 4 && !layout->has_clut);
    CHECK(!ParseTimLayout(tim.data(), 10));
    auto trunc = tim; trunc.resize(tim.size() - 1);
    CHECK(!ParseTimLayout(trunc.data(), trunc.size()));

    TextureOverrideManager m;
    auto h1 = m.ComputeHash(tim.data(), tim.size());
    auto padded = MakeTim16(4, 4, 1, 100);
    CHECK(h1 && m.ComputeHash(padded.data(), padded.size()) == h1);   // trailing sector padding ignored
    m.SetHashAlgorithm(TextureHashAlgo::Crc32);
    CHECK(m.ComputeHash(tim.data(), tim.size()) != h1);
    m.SetHashAlgorithm(TextureHashAlgo::Fnv1a32);
    m.SetHashScope(TextureHashScope::PixelsOnly);
    auto hp = m.ComputeHash(tim.data(), tim.size());
    CHECK(hp && *hp != *h1);
    m.SetHashScope(TextureHashScope::Full);
    CHECK(TextureOverrideManager::ParseHashString(TextureOverrideManager::HashToHex(0xAB12u)) == 0xAB12u);
    CHECK(!TextureOverrideManager::ParseHashString("zz"));

    fs::path dir = fs::temp_directory_path() / "prm_tex_test";
    fs::remove_all(dir); fs::create_directories(dir);
    std::vector<uint8_t> hd(8 * 8 * 4, 200);
    PngCodec::EncodeToFile((dir / (TextureOverrideManager::HashToHex(*h1) + "_hero.png")).string(), 8, 8, hd.data());
    { std::ofstream(dir / "broken.png") << "nope"; }
    CHECK(m.LoadDirectory(dir.string()) == 1);
    auto r = m.LookupForTim(tim.data(), tim.size());
    CHECK(r && r.scale_x == 2.0f && r.scale_y == 2.0f);
    auto r2 = m.LookupForTim(tim.data(), tim.size());
    CHECK(r2.texture == r.texture && m.Stats().hits >= 2);
    auto other = MakeTim16(4, 4, 9);
    CHECK(!m.LookupForTim(other.data(), other.size()));
    m.SetEnabled(false);
    CHECK(!m.LookupForTim(tim.data(), tim.size()));
    m.SetEnabled(true);

    // Corrupt PNG referenced by hash: failure is cached, not retried.
    auto h2 = *m.ComputeHash(other.data(), other.size());
    { std::ofstream(dir / (TextureOverrideManager::HashToHex(h2) + ".png")) << "bad"; }
    CHECK(m.LoadDirectory(dir.string()) == 2);
    CHECK(!m.LookupForTim(other.data(), other.size()));
    auto fails = m.Stats().load_failures;
    CHECK(!m.LookupForTim(other.data(), other.size()) && m.Stats().load_failures == fails);

    // Manifest: path traversal rejected.
    fs::remove_all(dir); fs::create_directories(dir);
    { std::ofstream f(dir / "overrides.json");
      f << "{\"textures\":[{\"hash\":\"00000001\",\"file\":\"../evil.png\"},{\"hash\":\"00000002\",\"file\":\"ok.png\"}]}"; }
    PngCodec::EncodeToFile((dir / "ok.png").string(), 1, 1, hd.data());
    CHECK(m.LoadDirectory(dir.string()) == 1 && m.HasOverride(2) && !m.HasOverride(1));

    // LRU: budget for one 8x8 texture (256 bytes).
    fs::remove_all(dir); fs::create_directories(dir);
    for (uint32_t h = 1; h <= 3; ++h)
        PngCodec::EncodeToFile((dir / (TextureOverrideManager::HashToHex(h) + ".png")).string(), 8, 8, hd.data());
    TextureOverrideManager lru; lru.SetCacheBudget(300);
    CHECK(lru.LoadDirectory(dir.string()) == 3);
    for (uint32_t h = 1; h <= 3; ++h) CHECK(lru.FindOverride(h));
    CHECK(lru.Stats().evictions >= 2 && lru.Stats().cached_bytes <= 300);

    // Export original.
    std::string out;
    CHECK(m.ExportOriginal(tim.data(), tim.size(), (dir / "exp").string(), &out) && fs::exists(out));
    fs::remove_all(dir);
}

static void TestProfiles() {
    InputProfile p = InputProfile::DefaultKeyboardMouse();
    CHECK(p.Rebind(VirtualAction::Cancel, {InputDeviceKind::Keyboard, "Space"}).result == RebindResult::Conflict);
    CHECK(p.Rebind(VirtualAction::Cancel, {InputDeviceKind::Keyboard, "Bogus??"}).result == RebindResult::InvalidBinding);
    CHECK(p.Rebind(VirtualAction::Cancel, {InputDeviceKind::Keyboard, "Space"}, ConflictPolicy::Steal).result == RebindResult::Ok);
    auto acts = p.ActionsForCode(InputDeviceKind::Keyboard, "Space");
    CHECK(acts.size() == 1 && acts[0] == VirtualAction::Cancel);
    CHECK(p.Rebind(VirtualAction::Confirm, {InputDeviceKind::Keyboard, "F"}, ConflictPolicy::Swap).result == RebindResult::Ok);

    InputProfileManager mgr;
    CHECK(mgr.Clone(mgr.ActiveName(), "Mine") && mgr.SetActive("Mine"));
    mgr.Active().stick_deadzone = 0.3f;
    mgr.Active().Rebind(VirtualAction::Menu, {InputDeviceKind::Keyboard, "M"}, ConflictPolicy::Steal);
    auto text = mgr.ToJson().Dump(2);
    InputProfileManager back;
    CHECK(back.FromJson(*JsonValue::Parse(text)) == 0);
    CHECK(back.ActiveName() == "Mine" && back.Active().stick_deadzone == 0.3f);
    CHECK(!back.Active().BindingsFor(VirtualAction::Menu).empty() &&
          back.Active().BindingsFor(VirtualAction::Menu)[0].code == "M");

    InputProfileManager junk;
    auto j = JsonValue::Parse(R"({"active_profile":"nope","profiles":[{"name":"X","bindings":{"Confirm":[{"device":"keyboard","code":"???"}],"Nope":[]}},5]})");
    CHECK(junk.FromJson(*j) >= 1);
    CHECK(!junk.Profiles().empty() && !junk.Active().BindingsFor(VirtualAction::Confirm).empty());
}

static void TestInputManager() {
    InputManager& im = InputManager::Instance(); CHECK(im.Initialize());
    InputProfile p = InputProfile::DefaultKeyboardMouse();
    im.ApplyProfile(p);
    CHECK(im.ProcessRawInput(InputDeviceKind::Keyboard, "Space", true));
    im.Update();
    CHECK(im.IsActionPressed(VirtualAction::Confirm) && im.IsActionJustPressed(VirtualAction::Confirm));
    im.ProcessRawInput(InputDeviceKind::Keyboard, "Space", false);
    im.Update();
    CHECK(!im.IsActionPressed(VirtualAction::Confirm));
    CHECK(im.ProcessRawInput(InputDeviceKind::Mouse, "WheelUp", true));
    im.Update();
    CHECK(im.IsActionJustPressed(VirtualAction::DialogueBacklog));
    im.Update();
    CHECK(!im.IsActionPressed(VirtualAction::DialogueBacklog) && im.IsActionJustReleased(VirtualAction::DialogueBacklog)); // one-frame pulse
}

static void TestMouseNav() {
    MouseNavigationManager nav;
    nav.SetViewport(100, 0, 800, 600, 320, 240);       // pillarbox: 2.5x
    MouseState ms;
    ms.x = 100 + 10 * 2.5; ms.y = 5 * 2.5 + 55 * 2.5 - 5 * 2.5; // virtual (10, 55)
    nav.SetContext(NavContext::None);
    CHECK(nav.Update(ms).empty());
    nav.SetContext(NavContext::Menu);
    nav.SetItems({{1, 0, 0, 100, 40, true}, {2, 0, 50, 100, 40, true}, {3, 0, 100, 100, 40, false}});
    auto ev = nav.Update(ms);        // item set changed -> hover re-evaluated
    CHECK(!ev.empty() && ev[0].type == NavEventType::Hover && ev[0].item_id == 2);
    CHECK(nav.Update(ms).empty());   // stationary pointer must not steal focus
    ms.left_button_down = true; ev = nav.Update(ms);
    bool conf = false; for (auto& e : ev) conf |= e.type == NavEventType::Confirm && e.item_id == 2;
    CHECK(conf);
    ms.left_button_down = false; nav.Update(ms);
    ms.right_button_down = true; ev = nav.Update(ms);
    bool canc = false; for (auto& e : ev) canc |= e.type == NavEventType::Cancel;
    CHECK(canc);
    ms.right_button_down = false; nav.Update(ms);
    // Disabled item can't be hovered; outside viewport has no hover.
    ms.y = 110 * 2.5; nav.Update(ms); CHECK(nav.GetHoveredItem() == -1);
    // Scroll clamp.
    nav.SetScrollRegion(0, 0, 320, 240, 10, 4);
    CHECK(nav.GetMaxScrollOffset() == 6);
    ms.x = 100 + 50; ms.y = 50; ms.wheel_delta = -20; nav.Update(ms);
    CHECK(nav.GetScrollOffset() == 6);
    ms.wheel_delta = 50; nav.Update(ms);
    CHECK(nav.GetScrollOffset() == 0);
    // Anti click-through: held button while context changes does not confirm.
    ms.wheel_delta = 0; ms.left_button_down = true; ms.x = 100 + 10 * 2.5; ms.y = 55 * 2.5;
    nav.Update(ms);
    nav.SetContext(NavContext::Inventory);
    nav.SetItems({{2, 0, 50, 100, 40, true}});
    ev = nav.Update(ms);
    for (auto& e : ev) CHECK(e.type != NavEventType::Confirm);
    // Cursor hides after idle.
    ms.left_button_down = false; nav.Update(ms);
    CHECK(nav.IsCursorVisible());
    for (int i = 0; i < 300; ++i) nav.Update(ms, 0.016f);
    CHECK(!nav.IsCursorVisible());
}

static void TestConfig() {
    LauncherConfig c;
    c.video.width = 1920; c.texture_overrides.hash_scope = "pixels"; c.input.device_preference = InputDevicePreference::GamepadXbox;
    c.input_profiles.Active().turbo_speed = 4;
    auto j = JsonValue::Parse(ConfigToJson(c).Dump(2));
    CHECK(j);
    LauncherConfig d;
    CHECK(ConfigFromJson(*j, d) == 0);
    CHECK(d.video.width == 1920 && d.texture_overrides.hash_scope == "pixels" &&
          d.input.device_preference == InputDevicePreference::GamepadXbox && d.input_profiles.Active().turbo_speed == 4);
    LauncherConfig e;
    auto bad = JsonValue::Parse(R"({"video":{"width":99999999,"window_mode":"x"},"texture_overrides":{"hash_algorithm":"md5","cache_mb":1}})");
    CHECK(ConfigFromJson(*bad, e) == 2);
    CHECK(e.video.width == 16384 && e.texture_overrides.cache_mb == 16 && e.texture_overrides.hash_algorithm == "fnv1a");
    LauncherConfig l;
    c.game.disc1_path = "a.bin"; c.game.disc2_path = "b.bin"; c.game.active_disc = 2;
    { LauncherConfig r; auto jj = JsonValue::Parse(ConfigToJson(c).Dump(2)); ConfigFromJson(*jj, r);
      CHECK(r.game.disc1_path == "a.bin" && r.game.disc2_path == "b.bin" && r.game.active_disc == 2); }
    ParseLegacyConfig("width=1280\n# c\nturbo_multiplier=2\nstick_deadzone=0.25\nmouse_sensitivity=abc\n", l);
    CHECK(l.video.width == 1280 && l.input_profiles.Active().turbo_speed == 2 && l.input_profiles.Active().stick_deadzone == 0.25f);
}

int main() {
    TestHash(); TestJson(); TestPng(); TestTimAndOverrides();
    TestProfiles(); TestInputManager(); TestMouseNav(); TestConfig();
    std::cout << "Phase 7 tests passed (" << g_tests << " checks)\n";
    return 0;
}
