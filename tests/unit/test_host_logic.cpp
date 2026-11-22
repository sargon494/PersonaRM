// SDL3 host pure logic: 4:3 letterbox, 15/24bpp display conversion, key/gamepad -> pad bits.
#undef NDEBUG
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "host/host_logic.h"

using namespace personarm::host;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

int main() {
    // Letterbox: pillarbox on 16:9, letterbox on tall windows, exact fit on 4:3, degenerate sizes.
    Rect r = Letterbox4x3(1920, 1080);
    CHECK(r.x == 240 && r.y == 0 && r.w == 1440 && r.h == 1080);
    r = Letterbox4x3(800, 1000);
    CHECK(r.x == 0 && r.y == 200 && r.w == 800 && r.h == 600);
    r = Letterbox4x3(640, 480);
    CHECK(r.x == 0 && r.y == 0 && r.w == 640 && r.h == 480);
    r = Letterbox4x3(0, 480);
    CHECK(r.w == 0 && r.h == 0);
    // Left stick -> d-pad: centre and small tilts press nothing; diagonals press two directions.
    CHECK(StickToDpad(0, 0) == 0 && StickToDpad(10000, -10000) == 0);
    CHECK(StickToDpad(-32768, 0) == (1u << 7) && StickToDpad(32767, 0) == (1u << 5));
    CHECK(StickToDpad(0, -32768) == (1u << 4) && StickToDpad(0, 32767) == (1u << 6));
    CHECK(StickToDpad(32767, 32767) == ((1u << 5) | (1u << 6)));

    std::vector<uint16_t> vram(1024 * 512, 0);
    std::vector<uint32_t> out;
    // 15bpp: pure red, green, blue (BGR555) -> 0xAABBGGRR.
    vram[10 * 1024 + 4] = 0x001F;
    vram[10 * 1024 + 5] = 0x03E0;
    vram[10 * 1024 + 6] = 0x7C00;
    DisplayToRgba(vram, 4, 10, 3, 1, false, out);
    CHECK(out.size() == 3);
    CHECK(out[0] == 0xFF0000F8u && out[1] == 0xFF00F800u && out[2] == 0xFFF80000u);
    // 15bpp wraps at the right VRAM edge.
    vram[0] = 0x001F;
    DisplayToRgba(vram, 1023, 0, 2, 1, false, out);
    CHECK(out[0] == 0xFF000000u && out[1] == 0xFF0000F8u);
    // 24bpp: bytes R0 G0 B0 R1 G1 B1 packed little-endian over 3 halfwords.
    vram[20 * 1024 + 100] = 0x2211;  // R0=11 G0=22
    vram[20 * 1024 + 101] = 0x4433;  // B0=33 R1=44
    vram[20 * 1024 + 102] = 0x6655;  // G1=55 B1=66
    DisplayToRgba(vram, 100, 20, 2, 1, true, out);
    CHECK(out[0] == 0xFF332211u && out[1] == 0xFF665544u);
    // Short VRAM: black, no crash.
    DisplayToRgba(std::vector<uint16_t>(16), 0, 0, 2, 2, false, out);
    CHECK(out.size() == 4 && out[3] == 0xFF000000u);

    // Pad mapping (psx-spx bits): keyboard and gamepad agree on every button, no duplicates per bit.
    CHECK(PadBitForName(KeyboardMap(), "X") == (1u << 14));       // Cross
    CHECK(PadBitForName(KeyboardMap(), "Return") == (1u << 3));   // Start
    CHECK(PadBitForName(KeyboardMap(), "Up") == (1u << 4));
    CHECK(PadBitForName(KeyboardMap(), "F12") == 0);
    CHECK(PadBitForName(GamepadMap(), "a") == (1u << 14));        // south = Cross
    CHECK(PadBitForName(GamepadMap(), "b") == (1u << 13));        // east = Circle
    CHECK(PadBitForName(GamepadMap(), "lefttrigger") == (1u << 8));
    CHECK(PadBitForName(GamepadMap(), "back") == (1u << 0));
    uint32_t kb = 0, gp = 0;
    for (const NamedBit& m : KeyboardMap()) kb |= m.bit;
    for (const NamedBit& m : GamepadMap()) gp |= m.bit;
    const uint32_t all = 0xFFF9u;  // all 14 digital buttons (bits 1-2 are unused on a digital pad)
    CHECK(kb == all && gp == all);

    // Command line: cards, recomp, values; errors on unknown options, missing values, no disc, bad --interp.
    HostOptions o;
    const char* full[] = {"personarm_sdl", "d.cue", "--memcard1", "a.mcd", "--memcard2", "b.mcd", "--recomp",
                          "--recomp-quantum", "64", "--interp", "120", "--frames", "100", "--hidden"};
    CHECK(ParseHostArgs(14, full, o));
    CHECK(o.disc == "d.cue" && o.memcard[0] == "a.mcd" && o.memcard[1] == "b.mcd" && o.recomp &&
          o.recomp_quantum == 64 && o.interp_hz == 120 && o.max_frames == 100 && o.hidden && !o.stats);
    const char* plain[] = {"personarm_sdl", "d.cue"};
    CHECK(ParseHostArgs(2, plain, o) && o.memcard[0].empty() && o.memcard[1].empty() && !o.recomp && o.recomp_quantum == 1);
    const char* tex[] = {"personarm_sdl", "d.cue", "--texture-dump", "dd", "--mods", "mm"};
    CHECK(ParseHostArgs(6, tex, o) && o.texture_dump == "dd" && o.mods == "mm");
    CHECK(ParseHostArgs(2, plain, o) && o.texture_dump.empty() && o.mods.empty());
    const char* ts[] = {"personarm_sdl", "d.cue", "--text-speed", "fast"};
    CHECK(ParseHostArgs(4, ts, o) && o.text_speed == "fast");
    const char* tsbad[] = {"personarm_sdl", "d.cue", "--text-speed", "warp"};
    CHECK(!ParseHostArgs(4, tsbad, o));
    CHECK(ParseHostArgs(2, plain, o) && o.text_speed.empty());
    const char* enc[] = {"personarm_sdl", "d.cue", "--encounters", "none"};
    CHECK(ParseHostArgs(4, enc, o) && o.encounters == "none");
    const char* encbad[] = {"personarm_sdl", "d.cue", "--encounters", "half"};
    CHECK(!ParseHostArgs(4, encbad, o));
    const char* bs[] = {"personarm_sdl", "d.cue", "--battle-speed", "3"};
    CHECK(ParseHostArgs(4, bs, o) && o.battle_speed == 3);
    const char* bsbad[] = {"personarm_sdl", "d.cue", "--battle-speed", "4"};
    CHECK(!ParseHostArgs(4, bsbad, o));
    CHECK(ParseHostArgs(2, plain, o) && o.battle_speed == 0);
    // Battle-speed scheduling: fast-forward wins, battle_speed only in battle, field always 1.
    CHECK(FrameSpeed(false, 4, false, 3) == 1);
    CHECK(FrameSpeed(false, 4, true, 3) == 3 && FrameSpeed(false, 4, true, 2) == 2 && FrameSpeed(false, 4, true, 1) == 1);
    CHECK(FrameSpeed(true, 4, false, 1) == 4 && FrameSpeed(true, 8, true, 3) == 8);
    CHECK(FrameSpeed(false, 4, true, 0) == 1 && FrameSpeed(false, 4, true, 9) == 3);  // out of range clamped
    const char* sk[] = {"personarm_sdl", "d.cue", "--skip-fmv"};
    CHECK(ParseHostArgs(3, sk, o) && o.skip_fmv);
    CHECK(ParseHostArgs(2, plain, o) && !o.skip_fmv);
    const char* missing[] = {"personarm_sdl", "d.cue", "--memcard1"};
    CHECK(!ParseHostArgs(3, missing, o));
    const char* unknown[] = {"personarm_sdl", "d.cue", "--bogus", "1"};
    CHECK(!ParseHostArgs(4, unknown, o));
    const char* nodisc[] = {"personarm_sdl", "--recomp"};
    CHECK(!ParseHostArgs(2, nodisc, o));
    const char* interp[] = {"personarm_sdl", "d.cue", "--interp", "100"};
    CHECK(!ParseHostArgs(4, interp, o));
    for (const char* hz : {"30", "90", "120", "0"}) {
        const char* ok[] = {"personarm_sdl", "d.cue", "--interp", hz};
        CHECK(ParseHostArgs(4, ok, o) && o.interp_hz == std::atoi(hz));
    }
    // Present schedule: which game frame (time = frame + phase) each picture shows, per 60 game frames.
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    for (int hz : {30, 60, 90, 120}) {
        size_t total = 0;
        for (uint64_t f = 0; f < 60; ++f) total += PresentPhases(hz, f).size();
        CHECK(total == size_t(hz));  // exactly hz pictures per second, any start frame
    }
    CHECK(PresentPhases(60, 7).size() == 1 && near(PresentPhases(60, 7)[0], 0));
    CHECK(PresentPhases(30, 6).size() == 1 && near(PresentPhases(30, 6)[0], 0) && PresentPhases(30, 7).empty());
    for (uint64_t f : {0, 2, 100}) {  // 90: {0, 2/3} on the even frame, {1/3} on the odd one
        const auto e = PresentPhases(90, f), od = PresentPhases(90, f + 1);
        CHECK(e.size() == 2 && near(e[0], 0) && near(e[1], 2.0 / 3) && od.size() == 1 && near(od[0], 1.0 / 3));
    }
    CHECK(PresentPhases(120, 5).size() == 2 && near(PresentPhases(120, 5)[1], 0.5));
    const char* gpu9[] = {"personarm_sdl", "d.cue", "--renderer", "gpu", "--resolution-scale", "9"};
    CHECK(ParseHostArgs(6, gpu9, o) && o.renderer == "gpu" && o.resolution_scale == 9);
    const char* gpuauto[] = {"personarm_sdl", "d.cue", "--resolution-scale", "auto", "--renderer", "gpu"};
    CHECK(ParseHostArgs(6, gpuauto, o) && o.resolution_scale == 0);
    const char* sw9[] = {"personarm_sdl", "d.cue", "--renderer", "software", "--resolution-scale", "9"};
    CHECK(!ParseHostArgs(6, sw9, o));
    const char* swauto[] = {"personarm_sdl", "d.cue", "--renderer", "software", "--resolution-scale", "auto"};
    CHECK(!ParseHostArgs(6, swauto, o));
    const char* file9[] = {"personarm_sdl", "d.cue", "--resolution-scale", "9"};  // renderer from the settings file
    CHECK(ParseHostArgs(4, file9, o) && o.resolution_scale == 9 && o.renderer.empty());
    const char* sw10[] = {"personarm_sdl", "d.cue", "--resolution-scale", "10"};
    CHECK(!ParseHostArgs(4, sw10, o));
    const char* badr[] = {"personarm_sdl", "d.cue", "--renderer", "vulkan"};
    CHECK(!ParseHostArgs(4, badr, o));
    CHECK(ParseHostArgs(2, plain, o) && o.renderer.empty());
    const char* two[] = {"personarm_sdl", "d.cue", "e.cue"};
    CHECK(!ParseHostArgs(3, two, o));

    // Recorder: simultaneous presses, a re-press, a button still held at the end, an idle gap.
    {
        InputRecorder rec;
        const uint16_t cross = 1u << 14, up = 1u << 4, start = 1u << 3;
        const uint16_t seq[] = {0, cross | up, cross | up, up, 0, 0, start, 0, cross, cross};
        for (uint64_t f = 0; f < 10; ++f) rec.Frame(f, seq[f]);
        CHECK(rec.Script(10) == "1 up 3\n1 cross 2\n6 start 1\n8 cross 2\n");
        CHECK(rec.Script(12) == "1 up 3\n1 cross 2\n6 start 1\n8 cross 4\n");
        CHECK(InputRecorder().Script(5).empty());
    }

    // Status badges and toast fade.
    CHECK(StatusBadges("normal", 1, false, "normal", false).empty());
    CHECK(StatusBadges("", 0, false, "", false).empty());
    {
        const auto b = StatusBadges("none", 3, true, "instant", true);
        CHECK(b.size() == 5 && b[0] == "ENC OFF" && b[1] == "BTL x3" && b[2] == "FMV >>" && b[3] == "TXT >>>" && b[4] == "FF >>");
        const auto c = StatusBadges("reduced", 2, false, "fast", false);
        CHECK(c.size() == 3 && c[0] == "ENC 1/2" && c[1] == "BTL x2" && c[2] == "TXT >>");
    }
    CHECK(ToastAlpha(2000) == 1.f && ToastAlpha(500) == 1.f && ToastAlpha(250) == 0.5f && ToastAlpha(0) == 0.f && ToastAlpha(-5) == 0.f);

    // Key chords.
    {
        std::string k; unsigned m;
        CHECK(ParseKeyChord("F5", k, m) && k == "F5" && m == 0);
        CHECK(ParseKeyChord("Ctrl+S", k, m) && k == "S" && m == kModCtrl);
        CHECK(ParseKeyChord("shift+CTRL+Alt+Return", k, m) && k == "Return" && m == (kModCtrl | kModShift | kModAlt));
        CHECK(ParseKeyChord("Keypad +", k, m) && k == "Keypad +" && m == 0);
        CHECK(ParseKeyChord("Ctrl++", k, m) && k == "+" && m == kModCtrl);
        CHECK(ParseKeyChord("Cmd+Q", k, m) && m == kModGui);
        CHECK(!ParseKeyChord("", k, m));
        CHECK(FormatKeyChord("S", kModShift | kModCtrl) == "Ctrl+Shift+S" && FormatKeyChord("F5", 0) == "F5");
    }
    // Pad buttons by name; the table matches SDL's enum values (south=0 ... touchpad=20).
    CHECK(PadButtonId("a") == 0 && PadButtonId("guide") == 5 && PadButtonId("touchpad") == 20 && PadButtonId("righttrigger") == 31 &&
          PadButtonId("nope") == -1 && std::string(PadButtonName(10)) == "rightshoulder");
    // Pad combos: modifier (guide) + R1 saves once; the combo buttons are held back from the game while the modifier is
    // held and until they are released; without the modifier nothing fires and nothing is swallowed.
    {
        const int G = PadButtonId("guide"), R1 = PadButtonId("rightshoulder"), R2 = PadButtonId("righttrigger"), X = PadButtonId("a");
        PadHotkeys pk;
        pk.Configure(G, {{"save_state", R1}, {"fast_forward", R2}, {"text_speed", X}, {"quit", -1}});
        auto bit = [](int id) { return 1u << id; };
        auto r = pk.Step(bit(R1));  // no modifier: the game gets R1
        CHECK(r.pressed.empty() && r.swallow == 0 && !r.fast_forward);
        r = pk.Step(0);
        r = pk.Step(bit(G));
        CHECK(r.pressed.empty() && r.swallow == 0);
        r = pk.Step(bit(G) | bit(R1));
        CHECK(r.pressed == std::vector<std::string>{"save_state"} && r.swallow == bit(R1));
        r = pk.Step(bit(G) | bit(R1));  // held: no repeat
        CHECK(r.pressed.empty() && r.swallow == bit(R1));
        r = pk.Step(bit(R1));  // modifier let go first: R1 stays held back until released
        CHECK(r.pressed.empty() && r.swallow == bit(R1));
        r = pk.Step(0);
        CHECK(r.swallow == 0);
        r = pk.Step(bit(G) | bit(R2) | bit(X));  // two at once; fast-forward is a hold
        CHECK(r.fast_forward && r.swallow == (bit(R2) | bit(X)) && r.pressed.size() == 2);
        r = pk.Step(bit(G));
        CHECK(!r.fast_forward && r.swallow == 0);
        // A button already held before the modifier is not taken over.
        pk.Configure(G, {{"save_state", R1}});
        r = pk.Step(bit(R1));
        r = pk.Step(bit(R1) | bit(G));
        CHECK(r.pressed.empty() && r.swallow == 0);
        pk.Configure(-1, {{"save_state", R1}});  // no modifier configured: inert
        CHECK(pk.Step(bit(R1)).pressed.empty());
    }
    CHECK(AgoText(0) == "just now" && AgoText(30) == "30 s ago" && AgoText(120) == "2 min ago" && AgoText(7300) == "2 h ago" &&
          AgoText(200000) == "2 d ago");

    std::puts("test_host_logic: OK");
    return 0;
}
