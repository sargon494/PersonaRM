#pragma once
// Pure host logic for the SDL3 front end (no SDL / OS headers): unit-tested in tests/unit/test_host_logic.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace personarm::host {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

/// Largest 4:3 rectangle centred in a win_w x win_h window (letterbox / pillarbox bars).
Rect Letterbox4x3(int win_w, int win_h);

/// Where the picture goes: whole window if `stretch`, else the 4:3 box; with `integer` the box is the
/// largest whole multiple of (tex_h*4/3) x tex_h that fits (falls back to the plain fit if even 1x does not).
Rect PresentRect(int win_w, int win_h, bool stretch, bool integer, int tex_h);

/// Display area of a 1024x512 16-bit VRAM to 0xAABBGGRR (bytes R,G,B,A in memory). 15bpp: BGR555 words;
/// 24bpp: packed RGB888 bytes starting at VRAM column x. Wraps at the VRAM edges like the GPU.
/// scale > 1 (15bpp only): `vram` is Ps1Gpu::GetScaledVram() and the output is (w*scale)x(h*scale).
void DisplayToRgba(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   bool is_24bit, std::vector<uint32_t>& out, uint32_t scale = 1);

struct NamedBit { const char* name; uint16_t bit; };
/// Digital pad bit (psx-spx "Controllers", bit set = pressed) per SDL key name (SDL_GetScancodeName
/// spelling, e.g. "Return") and per SDL gamepad button/axis name (SDL_GetGamepadStringForButton/Axis
/// spelling, e.g. "a", "lefttrigger"). The host resolves the names to SDL codes once at start.
const std::vector<NamedBit>& KeyboardMap();
const std::vector<NamedBit>& GamepadMap();
/// Analog stick (SDL axis values, y down) -> d-pad bits, with a half-travel dead zone.
uint16_t StickToDpad(int x, int y);
/// Action names (the keys of the settings file's input maps) -> pad bit: up down left right cross circle
/// square triangle start select l1 r1 l2 r2. Usable with PadBitForName.
const std::vector<NamedBit>& ActionNames();
/// Pad bit for one name of the tables above, 0 = unmapped.
uint16_t PadBitForName(const std::vector<NamedBit>& map, const std::string& name);

/// personarm_sdl command line.
struct HostOptions {
    std::string disc, screenshot, input_script, dump_dir;
    std::string texture_dump, mods; // --texture-dump D (dump used VRAM textures as PNG), --mods D (HD replacement)
    std::string text_speed;         // --text-speed normal|fast|instant (empty = the mods manifests decide)
    std::string memcard[2];         // --memcard1/2: raw .mcd per slot, empty = no card
    uint64_t max_frames = 0, dump_from = 0, dump_to = 0;
    int interp_hz = 0;
    uint32_t resolution_scale = 1;  // --resolution-scale 1-4 (display-only internal resolution)
    bool recomp = false;            // --recomp: run the recompiled code linked into this build
    uint32_t recomp_quantum = 1;    // --recomp-quantum
    // Settings file (src/host/settings.h): "unset" = empty / 0 / -1, so the file's value stays.
    std::string settings_path;      // --settings F (default: per-user config dir)
    std::string window_mode;        // --window-mode windowed|fullscreen|borderless
    int window_scale = 0;           // --window-scale 1..8
    int volume = -1;                // --volume 0..100
    bool mute = false;              // --mute
    bool interp_given = false;      // --interp was on the command line (even --interp 0)
    bool resolution_scale_given = false;  // --resolution-scale was on the command line
    bool save_settings = false;     // --save-settings: write the effective settings back to the file
    std::string record_input;       // --record-input F: write the played pad presses as an --input-script
    bool stats = false, unthrottled = false, hidden = false;
};
/// False on an unknown option, a missing value, no disc, --interp not a multiple of 60, a resolution scale
/// outside 1-4, or --interp with a resolution scale above 1 (the interpolator replays at 1x only).
bool ParseHostArgs(int argc, const char* const* argv, HostOptions& out);

/// --record-input: turns the pad mask of each emulated frame into --input-script lines.
class InputRecorder {
public:
    /// Pad mask in force on emulated frame `frame` (call once per frame, frames increasing).
    void Frame(uint64_t frame, uint16_t mask);
    /// "<frame> <button> <hold>" lines, ordered by frame then bit; buttons still held at `end_frame` are
    /// closed there (non-destructive, so it can be called again later).
    std::string Script(uint64_t end_frame) const;
private:
    struct Press { uint64_t frame, hold; int bit; };
    std::vector<Press> done_;
    uint64_t start_[16] = {};
    uint16_t held_ = 0;
};

} // namespace personarm::host
