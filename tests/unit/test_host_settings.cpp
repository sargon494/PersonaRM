// Host settings file: round trip keeping unknown keys, precedence, validation, per-game section,
// atomic write, key remap, present rect. No SDL.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "host/settings.h"

using namespace personarm;
using namespace personarm::host;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static std::string Slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static HostOptions Args(std::vector<const char*> a) {
    a.insert(a.begin(), "prog");
    HostOptions o;
    CHECK(ParseHostArgs(int(a.size()), a.data(), o));
    return o;
}

int main() {
    // Defaults: the old hard-coded behaviour.
    Settings d = DefaultSettings();
    CHECK(d.video.window_mode == "windowed" && d.video.window_scale == 3 && !d.video.vsync && d.pause_on_focus_loss);
    CHECK(d.audio.master_volume == 100 && !d.audio.mute);
    CHECK(d.keyboard["cross"] == std::vector<std::string>{"X"});
    CHECK(d.keyboard["select"].size() == 2);
    CHECK(FlattenBindings(d.keyboard).size() == KeyboardMap().size());
    CHECK(FlattenBindings(d.gamepad).size() == GamepadMap().size());

    // Parse + validation: bad values warn and keep the default, nothing throws.
    std::vector<std::string> w;
    Settings s = ParseSettings(R"({
        "version": 1, "launcher": {"theme": "dark"},
        "video": {"window_mode": "borderless", "window_scale": 99, "vsync": "yes", "aspect": "stretch",
                  "fps_target": 120, "resolution_scale": 2, "future_key": [1, 2]},
        "audio": {"master_volume": 55.5, "mute": true},
        "general": {"pause_on_focus_loss": false},
        "input": {"keyboard": {"cross": ["Space", "X"], "bogus": ["A"], "start": "Return", "up": ["Up", 3]}},
        "game": {"SLPS-02192": {"memcard1": "a.mcd", "text_speed": "fast", "recomp": true, "note": "x"},
                 "SLPS-02193": {"text_speed": "warp"}, "SLPS-1": 5}})", &w);
    CHECK(w.size() == 8);
    CHECK(s.video.window_mode == "borderless" && s.video.window_scale == 3 && !s.video.vsync);
    CHECK(s.video.aspect == "stretch" && InterpHz(s.video) == 120 && s.video.resolution_scale == 2);
    CHECK(s.audio.master_volume == 100 && s.audio.mute && !s.pause_on_focus_loss);
    CHECK((s.keyboard["cross"] == std::vector<std::string>{"Space", "X"}));
    CHECK((s.keyboard["start"] == std::vector<std::string>{"Return"}));  // bad type: default kept
    CHECK((s.keyboard["up"] == std::vector<std::string>{"Up"}));
    CHECK(!s.keyboard.count("bogus"));
    // Key remap -> pad bits.
    bool space_is_cross = false;
    for (const Binding& b : FlattenBindings(s.keyboard)) space_is_cross |= b.name == "Space" && b.bit == (1u << 14);
    CHECK(space_is_cross);
    // Per-game section by serial.
    CHECK(GameFor(s, "SLPS-02192").memcard[0] == "a.mcd" && GameFor(s, "SLPS-02192").recomp == true);
    CHECK(GameFor(s, "SLPS-02192").text_speed == "fast" && GameFor(s, "SLPS-02193").text_speed.empty());
    CHECK(GameFor(s, "SLPS-02194").Empty());

    // Garbage never crashes.
    for (const char* bad : {"", "{", "[1,2]", "null", "{\"video\": 5, \"input\": []}", "{\"version\": \"x\"}"}) {
        w.clear();
        Settings b = ParseSettings(bad, &w);
        CHECK(b.video.window_scale == 3);
        CHECK(w.empty() == (std::string(bad).find("video") != std::string::npos));
    }
    w.clear();
    CHECK(ParseSettings("{\"version\": 7}", &w).version == 7 && w.size() == 1);

    // Round trip keeps unknown keys at every level and the known values.
    const std::string out = DumpSettings(s);
    w.clear();
    Settings r = ParseSettings(out, &w);
    CHECK(w.size() == 3);  // the preserved bad entries: action "bogus", game "SLPS-1", text_speed "warp"
    CHECK(r.raw.Find("launcher")->Find("theme")->AsString() == "dark");
    CHECK(r.raw.Find("video")->Has("future_key"));
    CHECK(r.raw.Find("game")->Find("SLPS-02192")->Has("note"));
    CHECK(r.raw.Find("input")->Find("keyboard")->Has("bogus"));
    CHECK(r.video.window_mode == "borderless" && r.video.fps_target == 120 && r.audio.mute);
    CHECK(GameFor(r, "SLPS-02192").memcard[0] == "a.mcd" && r.keyboard["cross"] == s.keyboard["cross"]);
    // A launcher-style edit on top (modify one value, write): other host keys untouched.
    r.audio.master_volume = 30;
    Settings r2 = ParseSettings(DumpSettings(r), nullptr);
    CHECK(r2.audio.master_volume == 30 && r2.video.window_mode == "borderless" && r2.raw.Has("launcher"));

    // Precedence: defaults < file < CLI.
    Settings file = ParseSettings(R"({"video": {"window_scale": 2, "fps_target": 120},
        "audio": {"master_volume": 40}, "game": {"SLPS-02192": {"memcard1": "file.mcd", "text_speed": "fast", "mods": "m"}}})", nullptr);
    Settings none = ApplyCli(file, Args({"d.cue"}), "SLPS-02192");
    CHECK(none.video.window_scale == 2 && none.video.fps_target == 120 && none.audio.master_volume == 40);
    CHECK(GameFor(none, "SLPS-02192").memcard[0] == "file.mcd");
    CHECK(none.video.window_mode == "windowed");  // default under the file
    Settings cli = ApplyCli(file, Args({"d.cue", "--window-scale", "4", "--interp", "0", "--volume", "0", "--mute",
                                        "--memcard1", "cli.mcd", "--text-speed", "instant", "--window-mode", "fullscreen",
                                        "--recomp"}), "SLPS-02192");
    CHECK(cli.video.window_scale == 4 && cli.video.fps_target == 60 && InterpHz(cli.video) == 0 && cli.audio.master_volume == 0 && cli.audio.mute);
    CHECK(cli.video.window_mode == "fullscreen");
    const GameSettings g = GameFor(cli, "SLPS-02192");
    CHECK(g.memcard[0] == "cli.mcd" && g.text_speed == "instant" && g.mods == "m" && g.recomp == true);
    CHECK(GameFor(cli, "OTHER").memcard[0].empty());  // CLI game options belong to this disc only
    // Empty CLI + unknown disc adds no empty game entry.
    CHECK(ApplyCli(file, Args({"d.cue"}), "NEW").games.count("NEW") == 0);

    // New launcher fields: fps_target, resolution_scale 1..4, save_dir, language.
    w.clear();
    Settings n = ParseSettings(R"({"video": {"fps_target": 45, "resolution_scale": 5}, "general": {"language": "en", "save_dir": "D:/cards"}})", &w);
    CHECK(w.size() == 2 && n.video.fps_target == 60 && n.video.resolution_scale == 1);
    CHECK(n.language == "en" && ParseSettings("{}", nullptr).language == "ja");
    CHECK(ParseSettings("{\"video\": {\"fps_target\": 30}}", nullptr).video.fps_target == 30);
    CHECK(InterpHz(n.video) == 0 && ParseSettings("{\"video\": {\"fps_target\": 90}}", nullptr).video.fps_target == 90);
    CHECK(MemcardFor(n, GameSettings{}, "SLPS-02192", 0).find("SLPS-02192-1.mcd") != std::string::npos);
    CHECK(MemcardFor(n, GameSettings{}, "SLPS-02192", 1).empty() && MemcardFor(n, GameSettings{}, "", 0).empty());
    GameSettings gx; gx.memcard[0] = "mine.mcd";
    CHECK(MemcardFor(n, gx, "SLPS-02192", 0) == "mine.mcd");
    CHECK(ParseSettings(DumpSettings(n), nullptr).save_dir == "D:/cards");

    // CLI validation.
    HostOptions o;
    const char* bad1[] = {"p", "d.cue", "--volume", "101"};
    const char* bad2[] = {"p", "d.cue", "--window-mode", "full"};
    const char* bad3[] = {"p", "d.cue", "--window-scale", "0"};
    CHECK(!ParseHostArgs(4, bad1, o) && !ParseHostArgs(4, bad2, o) && !ParseHostArgs(4, bad3, o));

    // Atomic write: new dir created, no temp left, replaces an existing file, failure leaves it intact.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "prm_settings_test";
    fs::remove_all(dir);
    const fs::path f = dir / "sub" / "settings.json";
    std::string err;
    CHECK(SaveSettings(f.string(), s, &err));
    CHECK(fs::exists(f) && !fs::exists(f.string() + ".tmp"));
    CHECK(Slurp(f) == DumpSettings(s));
    CHECK(SaveSettings(f.string(), r, &err) && Slurp(f) == DumpSettings(r));
    CHECK(LoadSettings(f.string(), nullptr).audio.master_volume == 30);
    CHECK(!SaveSettings((f / "x" / "y.json").string(), s, &err) && !err.empty());  // parent is a file
    CHECK(Slurp(f) == DumpSettings(r));
    CHECK(LoadSettings((dir / "missing.json").string(), &w).video.window_scale == 3);
    fs::remove_all(dir);

    // Present rect: stretch, integer scaling, fallback.
    Rect p = PresentRect(1920, 1080, true, false, 240);
    CHECK(p.x == 0 && p.w == 1920 && p.h == 1080);
    p = PresentRect(1920, 1080, false, true, 240);  // 320x240 -> x4 = 1280x960
    CHECK(p.w == 1280 && p.h == 960 && p.x == 320 && p.y == 60);
    p = PresentRect(300, 200, false, true, 240);    // 1x does not fit: plain 4:3 fit
    CHECK(p.h == 200 && p.w == 266);
    p = PresentRect(1920, 1080, false, false, 240);
    CHECK(p.w == 1440 && p.h == 1080);

    // Widescreen: aspect parsing, settings fallback, presentation at W:H.
    int aw = 0, ah = 0;
    CHECK(ParseWidescreen("16:9", aw, ah) && aw == 16 && ah == 9);
    CHECK(ParseWidescreen("off", aw, ah) && aw == 4 && ah == 3);
    CHECK(!ParseWidescreen("3:4", aw, ah) && !ParseWidescreen("16:9x", aw, ah) && !ParseWidescreen("wide", aw, ah));
    CHECK(DefaultSettings().video.widescreen == "off");
    w.clear();
    CHECK(ParseSettings(R"({"video":{"widescreen":"21:9"}})", &w).video.widescreen == "21:9" && w.empty());
    CHECK(ParseSettings(R"({"video":{"widescreen":"9:16"}})", &w).video.widescreen == "off" && w.size() == 1);
    p = PresentRect(1920, 1080, false, false, 240, 16, 9);
    CHECK(p.x == 0 && p.w == 1920 && p.h == 1080);
    p = PresentRect(1920, 1080, false, true, 240, 16, 9);  // 426x240 x4 = 1704x960
    CHECK(p.w == 1704 && p.h == 960);
    std::printf("host settings tests passed\n");
    return 0;
}
