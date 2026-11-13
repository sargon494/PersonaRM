// SDL3 host pure logic: 4:3 letterbox, 15/24bpp display conversion, key/gamepad -> pad bits.
#undef NDEBUG
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

    std::puts("test_host_logic: OK");
    return 0;
}
