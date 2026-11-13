#pragma once
// Pure host logic for the SDL3 front end (no SDL / OS headers): unit-tested in tests/unit/test_host_logic.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace personarm::host {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

/// Largest 4:3 rectangle centred in a win_w x win_h window (letterbox / pillarbox bars).
Rect Letterbox4x3(int win_w, int win_h);

/// Display area of a 1024x512 16-bit VRAM to 0xAABBGGRR (bytes R,G,B,A in memory). 15bpp: BGR555 words;
/// 24bpp: packed RGB888 bytes starting at VRAM column x. Wraps at the VRAM edges like the GPU.
void DisplayToRgba(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   bool is_24bit, std::vector<uint32_t>& out);

struct NamedBit { const char* name; uint16_t bit; };
/// Digital pad bit (psx-spx "Controllers", bit set = pressed) per SDL key name (SDL_GetScancodeName
/// spelling, e.g. "Return") and per SDL gamepad button/axis name (SDL_GetGamepadStringForButton/Axis
/// spelling, e.g. "a", "lefttrigger"). The host resolves the names to SDL codes once at start.
const std::vector<NamedBit>& KeyboardMap();
const std::vector<NamedBit>& GamepadMap();
/// Pad bit for one name of the tables above, 0 = unmapped.
uint16_t PadBitForName(const std::vector<NamedBit>& map, const std::string& name);

} // namespace personarm::host
