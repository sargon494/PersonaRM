// GPU rasterizer conformance: synthetic GP0 streams with hand-derived expected VRAM pixels.
// Rules come from psx-spx "GPU" sections (Render Polygon/Line/Rectangle Commands, Rendering
// Attributes, Semi-Transparency, Dithering, Mask Bit, Memory Transfer Commands).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "rendering/ps1_gpu.h"
#include "rendering/widescreen_2d.h"

using personarm::Ps1Gpu;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_EQ(name, got, want)                                                          \
    do {                                                                                   \
        ++g_checks;                                                                        \
        uint32_t g_ = (got), w_ = (want);                                                  \
        if (g_ != w_) {                                                                    \
            ++g_failures;                                                                  \
            std::printf("  FAIL %s: %s = 0x%08X, want 0x%08X\n", name, #got, g_, w_);      \
        }                                                                                  \
    } while (0)

static void Send(Ps1Gpu& gpu, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) gpu.WriteGP0(w);
}

static uint32_t XY(int32_t x, int32_t y) {
    return (static_cast<uint32_t>(x) & 0xFFFF) | (static_cast<uint32_t>(y) << 16);
}

// Full-VRAM drawing area, zero offset, E1 = 0.
static void FullArea(Ps1Gpu& gpu) {
    Send(gpu, {0xE3000000, 0xE4000000 | 1023 | (511 << 10), 0xE5000000, 0xE1000000});
}

static uint32_t CountSet(const Ps1Gpu& gpu, int x0, int y0, int w, int h) {
    uint32_t n = 0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            if (gpu.ReadVram(x, y) != 0) ++n;
    return n;
}

static uint16_t Gray(int v5) { return static_cast<uint16_t>(v5 * 0x421); }

static void TestFillRule() {
    Ps1Gpu gpu;
    FullArea(gpu);
    // Flat red triangle (0,0),(4,0),(0,4): top/left edges in, hypotenuse (right) out -> x+y<=3.
    Send(gpu, {0x200000FF, XY(0, 0), XY(4, 0), XY(0, 4)});
    CHECK_EQ("tri count", CountSet(gpu, 0, 0, 8, 8), 10);
    CHECK_EQ("tri (0,0)", gpu.ReadVram(0, 0), 0x001F);
    CHECK_EQ("tri (3,0)", gpu.ReadVram(3, 0), 0x001F);
    CHECK_EQ("tri (1,2)", gpu.ReadVram(1, 2), 0x001F);
    CHECK_EQ("tri (4,0)", gpu.ReadVram(4, 0), 0);
    CHECK_EQ("tri (0,4)", gpu.ReadVram(0, 4), 0);
    CHECK_EQ("tri (2,2)", gpu.ReadVram(2, 2), 0);

    // Additive quad 4x4: shared diagonal must not be drawn twice (8 + 8 would give 16).
    Send(gpu, {0xE1000020}); // semi mode 1
    Send(gpu, {0x2A404040, XY(100, 0), XY(104, 0), XY(100, 4), XY(104, 4)});
    CHECK_EQ("quad count", CountSet(gpu, 100, 0, 8, 8), 16);
    for (int y = 0; y < 4; ++y)
        for (int x = 100; x < 104; ++x)
            CHECK_EQ("quad pixel", gpu.ReadVram(x, y), Gray(8));

    // Oversized: width 1024 is skipped, width 1023 is drawn.
    Send(gpu, {0xE1000000});
    Send(gpu, {0x20FFFFFF, XY(-512, 200), XY(512, 200), XY(0, 204)});
    CHECK_EQ("oversized", CountSet(gpu, 0, 200, 16, 4), 0);
    Send(gpu, {0x20FFFFFF, XY(-511, 200), XY(512, 200), XY(0, 204)});
    CHECK_EQ("1023 wide drawn", gpu.ReadVram(0, 201), 0x7FFF);
}

static void TestGouraud() {
    Ps1Gpu gpu;
    FullArea(gpu);
    // Red ramps 0 -> 248 along the top edge, green constant 128 marks drawn pixels.
    Send(gpu, {0x30008000, XY(0, 200), 0x0080F8, XY(8, 200), 0x008000, XY(0, 208)});
    CHECK_EQ("gouraud v0", gpu.ReadVram(0, 200), 0x0200);
    CHECK_EQ("gouraud mid", gpu.ReadVram(4, 200), 0x020F);  // 248*4/8 = 124 -> 15
    CHECK_EQ("gouraud 7/8", gpu.ReadVram(7, 200), 0x021B);  // 248*7/8 = 217 -> 27
    CHECK_EQ("gouraud v1 excluded", gpu.ReadVram(8, 200), 0);
    // Three distinct vertex colors: the included vertex keeps its exact color.
    Send(gpu, {0x300000FF, XY(20, 200), 0x00FF00, XY(28, 200), 0xFF0000, XY(20, 208)});
    CHECK_EQ("gouraud red vertex", gpu.ReadVram(20, 200), 0x001F);

    // Dithering (E1 bit 9) on gouraud: 0x40 + table offset, then >>3.
    Send(gpu, {0xE1000200});
    Send(gpu, {0x30404040, XY(0, 300), 0x404040, XY(8, 300), 0x404040, XY(0, 308)});
    CHECK_EQ("dither -4", gpu.ReadVram(0, 300), Gray(7));
    CHECK_EQ("dither +0", gpu.ReadVram(1, 300), Gray(8));
    CHECK_EQ("dither +3", gpu.ReadVram(2, 301), Gray(8));
    CHECK_EQ("dither -3", gpu.ReadVram(0, 302), Gray(7));
    // Flat untextured polygons are not dithered.
    Send(gpu, {0x20404040, XY(0, 312), XY(8, 312), XY(0, 320)});
    CHECK_EQ("flat no dither", gpu.ReadVram(0, 312), Gray(8));
}

static void TestTextures() {
    Ps1Gpu gpu;
    FullArea(gpu);
    const uint32_t clut = (480u << 6); // CLUT at (0,480)
    // CLUT: 16 entries 0x1000+i via CPU->VRAM.
    Send(gpu, {0xA0000000, XY(0, 480), XY(16, 1)});
    for (uint32_t k = 0; k < 8; ++k) gpu.WriteGP0((0x1000 + 2 * k) | ((0x1000 + 2 * k + 1) << 16));
    // 4-bit texture at page (640,0): row0 indices 0,1,2,3; row1 C,D,E,F.
    Send(gpu, {0xA0000000, XY(640, 0), XY(1, 2), 0x3210 | (0xFEDCu << 16)});

    Send(gpu, {0xE100000A}); // page x=640, 4-bit
    Send(gpu, {0x65000000, XY(100, 100), clut << 16, XY(4, 2)});
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ("clut rect row0", gpu.ReadVram(100 + i, 100), 0x1000 + i);
        CHECK_EQ("clut rect row1", gpu.ReadVram(100 + i, 101), 0x100C + i);
    }

    // Raw textured quad: texpage comes from the command (E1 reset to 0 first).
    Send(gpu, {0xE1000000});
    Send(gpu, {0x2D000000, XY(200, 100), clut << 16, XY(204, 100), 4u | (0x000Au << 16),
               XY(200, 102), 0x0200, XY(204, 102), 0x0204});
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ("clut quad row0", gpu.ReadVram(200 + i, 100), 0x1000 + i);
        CHECK_EQ("clut quad row1", gpu.ReadVram(200 + i, 101), 0x100C + i);
    }
    CHECK_EQ("texpage -> GPUSTAT", gpu.ReadGPUSTAT() & 0x1FF, 0x00A);

    // 15-bit texture modulation: texel 31 * color 0x40 / 128 -> 15; 0x80 is identity.
    gpu.WriteVram(640, 10, 0x7FFF);
    gpu.WriteVram(641, 10, 0x8000 | Gray(8));
    Send(gpu, {0xE100010A}); // 15-bit, page x=640
    Send(gpu, {0x64404040, XY(300, 100), 10u << 8, XY(1, 1)});
    CHECK_EQ("modulate half", gpu.ReadVram(300, 100), Gray(15));
    Send(gpu, {0x64808080, XY(301, 100), 10u << 8, XY(1, 1)});
    CHECK_EQ("modulate identity", gpu.ReadVram(301, 100), 0x7FFF);
    // Textured semi: texel without bit 15 is opaque, with bit 15 is blended (mode 1, bg 16).
    Send(gpu, {0x02808080, XY(304, 100), XY(16, 1)});
    Send(gpu, {0xE100012A}); // 15-bit, mode 1
    Send(gpu, {0x66808080, XY(304, 100), 10u << 8, XY(2, 1)});
    CHECK_EQ("tex semi opaque texel", gpu.ReadVram(304, 100), 0x7FFF);
    CHECK_EQ("tex semi stp texel", gpu.ReadVram(305, 100), 0x8000 | Gray(24));
    // Texel 0000h is transparent.
    gpu.WriteVram(642, 10, 0);
    Send(gpu, {0x65000000, XY(320, 100), (10u << 8) | 2, XY(1, 1)});
    CHECK_EQ("transparent texel", gpu.ReadVram(320, 100), 0);
}

static void TestSemiModes() {
    Ps1Gpu gpu;
    FullArea(gpu);
    Send(gpu, {0x02808080, XY(300, 0), XY(16, 1)}); // background 16 per channel
    const int want[4] = {12, 24, 8, 18};            // B/2+F/2, B+F, B-F, B+F/4 with F=8
    for (uint32_t m = 0; m < 4; ++m) {
        Send(gpu, {0xE1000000 | (m << 5)});
        Send(gpu, {0x62404040, XY(300 + m, 0), XY(1, 1)});
        CHECK_EQ("semi mode", gpu.ReadVram(300 + m, 0), Gray(want[m]));
    }
    CHECK_EQ("GPUSTAT semi bits", (gpu.ReadGPUSTAT() >> 5) & 3, 3);
}

static void TestClipOffsetMask() {
    Ps1Gpu gpu;
    FullArea(gpu);
    Send(gpu, {0xE3000000 | 10 | (10 << 10), 0xE4000000 | 13 | (13 << 10)});
    Send(gpu, {0x60FFFFFF, XY(0, 0), XY(32, 32)});
    CHECK_EQ("clip count", CountSet(gpu, 0, 0, 32, 32), 16);
    CHECK_EQ("clip (13,13)", gpu.ReadVram(13, 13), 0x7FFF);
    CHECK_EQ("clip (9,10)", gpu.ReadVram(9, 10), 0);

    FullArea(gpu);
    Send(gpu, {0xE5000000 | 5 | (5 << 11)});
    Send(gpu, {0x68FFFFFF, XY(1, 1)});
    CHECK_EQ("offset +5", gpu.ReadVram(6, 6), 0x7FFF);
    Send(gpu, {0xE5000000 | 0x7FF | (2 << 11)}); // x offset -1
    Send(gpu, {0x68FFFFFF, XY(3, 3)});
    CHECK_EQ("offset -1", gpu.ReadVram(2, 5), 0x7FFF);

    FullArea(gpu);
    Send(gpu, {0xE6000002}); // check mask
    gpu.WriteVram(50, 50, 0x8000);
    Send(gpu, {0x60FFFFFF, XY(50, 50), XY(2, 1)});
    CHECK_EQ("mask protected", gpu.ReadVram(50, 50), 0x8000);
    CHECK_EQ("mask unprotected", gpu.ReadVram(51, 50), 0x7FFF);
    Send(gpu, {0xE6000001}); // set mask
    Send(gpu, {0x68FFFFFF, XY(52, 50)});
    CHECK_EQ("mask set", gpu.ReadVram(52, 50), 0xFFFF);
    CHECK_EQ("GPUSTAT mask bits", (gpu.ReadGPUSTAT() >> 11) & 3, 1);
}

static void TestLines() {
    Ps1Gpu gpu;
    FullArea(gpu);
    Send(gpu, {0x40FFFFFF, XY(0, 20), XY(3, 23)});
    for (int i = 0; i < 4; ++i) CHECK_EQ("diag line", gpu.ReadVram(i, 20 + i), 0x7FFF);
    CHECK_EQ("diag count", CountSet(gpu, 0, 20, 8, 8), 4);
    Send(gpu, {0x40FFFFFF, XY(0, 30), XY(5, 30)});
    CHECK_EQ("hline count", CountSet(gpu, 0, 30, 16, 1), 6);

    // Polyline with terminator, then a dot proves the parser resynchronised.
    Send(gpu, {0x48FFFFFF, XY(0, 40), XY(3, 40), XY(3, 43), 0x55555555});
    Send(gpu, {0x680000F8, XY(10, 40)});
    for (int i = 0; i < 4; ++i) CHECK_EQ("poly seg0", gpu.ReadVram(i, 40), 0x7FFF);
    for (int i = 41; i < 44; ++i) CHECK_EQ("poly seg1", gpu.ReadVram(3, i), 0x7FFF);
    CHECK_EQ("poly count", CountSet(gpu, 0, 40, 8, 8), 7);
    CHECK_EQ("after terminator", gpu.ReadVram(10, 40), 0x001F);

    // Gouraud line: red -> blue, endpoints exact, middle 124 -> 15.
    Send(gpu, {0x500000F8, XY(0, 50), 0xF80000, XY(8, 50)});
    CHECK_EQ("gline start", gpu.ReadVram(0, 50), 0x001F);
    CHECK_EQ("gline end", gpu.ReadVram(8, 50), 0x7C00);
    CHECK_EQ("gline mid", gpu.ReadVram(4, 50), 0x3C0F);

    // Gouraud polyline: color words precede each vertex.
    Send(gpu, {0x58FFFFFF, XY(0, 70), 0xFFFFFF, XY(2, 70), 0xFFFFFF, XY(2, 72), 0x55555555});
    CHECK_EQ("gpoly count", CountSet(gpu, 0, 70, 8, 8), 5);

    // Semi-transparent line, mode 1 over background 16.
    Send(gpu, {0x02808080, XY(0, 60), XY(16, 1), 0xE1000020});
    Send(gpu, {0x42404040, XY(0, 60), XY(3, 60)});
    CHECK_EQ("semi line", gpu.ReadVram(2, 60), Gray(24));
}

static void TestSprites() {
    Ps1Gpu gpu;
    FullArea(gpu);
    Send(gpu, {0x70FFFFFF, XY(0, 0)});
    CHECK_EQ("8x8", CountSet(gpu, 0, 0, 32, 32), 64);
    Send(gpu, {0x78FFFFFF, XY(100, 0)});
    CHECK_EQ("16x16", CountSet(gpu, 100, 0, 32, 32), 256);
    Send(gpu, {0x68FFFFFF, XY(200, 0)});
    CHECK_EQ("1x1", CountSet(gpu, 200, 0, 8, 8), 1);
    Send(gpu, {0x60FFFFFF, XY(250, 0), XY(5, 3)});
    CHECK_EQ("5x3", CountSet(gpu, 250, 0, 16, 16), 15);

    for (int i = 0; i < 4; ++i) gpu.WriteVram(640 + i, 0, static_cast<uint16_t>(i + 1));
    Send(gpu, {0xE100010A}); // 15-bit page x=640
    Send(gpu, {0x65000000, XY(300, 1), 0, XY(4, 1)});
    Send(gpu, {0xE100110A}); // + X flip
    Send(gpu, {0x65000000, XY(300, 0), 3, XY(4, 1)});
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ("sprite plain", gpu.ReadVram(300 + i, 1), i + 1);
        CHECK_EQ("sprite xflip", gpu.ReadVram(300 + i, 0), 4 - i);
    }
}

static void TestGpuRead() {
    Ps1Gpu gpu;
    Send(gpu, {0xA0000000, XY(500, 300), XY(3, 1), 0x56781234, 0x00009ABC});
    CHECK_EQ("stat27 idle", (gpu.ReadGPUSTAT() >> 27) & 1, 0);
    Send(gpu, {0xC0000000, XY(500, 300), XY(3, 1)});
    CHECK_EQ("stat27 ready", (gpu.ReadGPUSTAT() >> 27) & 1, 1);
    CHECK_EQ("gpuread w0", gpu.ReadGP0(), 0x56781234);
    CHECK_EQ("gpuread w1", gpu.ReadGP0() & 0xFFFF, 0x9ABC);
    CHECK_EQ("stat27 done", (gpu.ReadGPUSTAT() >> 27) & 1, 0);
    CHECK_EQ("gpuread latch", gpu.ReadGP0() & 0xFFFF, 0x9ABC);
    // Mirror opcode C1h behaves like C0h.
    Send(gpu, {0xC1000000, XY(501, 300), XY(2, 1)});
    CHECK_EQ("gpuread mirror", gpu.ReadGP0(), 0x9ABC5678);
    // E1h bits 0-10 mirror into GPUSTAT.
    Send(gpu, {0xE10002AA});
    CHECK_EQ("GPUSTAT E1", gpu.ReadGPUSTAT() & 0x7FF, 0x2AA);
    // GP1(10h) Get GPU Info latches E2-E5 / GPU type into GPUREAD; index 0 keeps the old value.
    Send(gpu, {0xE2012345, 0xE3000000 | 0x0400 | 0x10, 0xE4000000 | (239 << 10) | 319, 0xE5000000 | (0x7FF << 11) | 8});
    gpu.WriteGP1(0x10000002);
    CHECK_EQ("info E2", gpu.ReadGP0(), 0x12345);
    gpu.WriteGP1(0x10000003);
    CHECK_EQ("info E3", gpu.ReadGP0(), 0x0410);
    gpu.WriteGP1(0x10000004);
    CHECK_EQ("info E4", gpu.ReadGP0(), 0x03BD3F);
    gpu.WriteGP1(0x10000005);
    CHECK_EQ("info E5", gpu.ReadGP0(), 0x3FF808);
    gpu.WriteGP1(0x10000000);
    CHECK_EQ("info keep", gpu.ReadGP0(), 0x3FF808);
    gpu.WriteGP1(0x1F000007);
    CHECK_EQ("info type", gpu.ReadGP0(), 2);
}

// psx-spx GP1(00h): the GPU reset clears registers, not VRAM (P2 JP's ResetGraph(0) after its system
// texture upload left the menu cursor with no texture when the reset wiped VRAM).
static void TestGp1ResetKeepsVram() {
    Ps1Gpu gpu;
    Send(gpu, {0xA0000000, XY(960, 256), XY(2, 1), 0x22221111, 0xE1000123});
    gpu.WriteGP1(0x00000000);
    CHECK_EQ("gp1 reset vram", gpu.ReadVram(960, 256), 0x1111);
    CHECK_EQ("gp1 reset E1", gpu.ReadGPUSTAT() & 0x7FF, 0);
    gpu.Reset();
    CHECK_EQ("power-on vram", gpu.ReadVram(960, 256), 0);
}

// Widescreen 16:9 (squeeze 3/4), drawing area x 0..319 (centre 160). A primitive with 2+ GTE vertices is
// 3D and drawn as sent; 2D before the frame's first 3D (background) and full-width 2D too. Textured 2D
// right after 3D (a scene sprite) is squeezed around its own centre, other 2D (HUD) toward x=160.
static void TestWidescreen2D() {
    using personarm::Widescreen2D;
    Widescreen2D ws(3, 4);
    CHECK_EQ("ws 2D-only screen", ws.Wide(), 0);
    ws.NoteProjected(XY(10, 10));
    ws.NoteProjected(XY(50, 10));
    CHECK_EQ("ws wide", ws.Wide(), 1);
    CHECK_EQ("ws projected", ws.Projected(XY(10, 10)), 1);
    CHECK_EQ("ws other xy", ws.Projected(XY(11, 10)), 0);
    CHECK_EQ("ws -1 (11-bit)", ws.Projected(XY(-1, 0)), 0);
    ws.NoteProjected(XY(-1, 0));
    CHECK_EQ("ws -1 matches 7FF", ws.Projected(0x000007FF), 1);

    Ps1Gpu gpu;
    gpu.SetWidescreen2D(&ws);
    Send(gpu, {0x02FFFFFF, XY(640, 0), XY(64, 64)});  // white 15-bit texture at (640,0)
    Send(gpu, {0xE100010A, 0xE3000000, 0xE4000000 | 319 | (239 << 10), 0xE5000000});
    Send(gpu, {0x60FFFFFF, XY(0, 0), XY(40, 1)});     // background (no 3D yet): x 0..39 as sent
    CHECK_EQ("background rect", CountSet(gpu, 0, 0, 320, 1), 40);
    Send(gpu, {0x200000FF, XY(10, 10), XY(50, 10), XY(10, 50)});  // 3D, unchanged
    CHECK_EQ("3D tri", gpu.ReadVram(10, 10) != 0 && gpu.ReadVram(48, 11) != 0, 1);
    // Scene sprite right after it: textured x 100..140 around its centre 120 -> 105..135; a touching
    // second part (140..160) keeps the same centre -> 135..150.
    Send(gpu, {0x25000000, XY(100, 100), 0, XY(140, 100), 0x010A0000, XY(100, 140), 0});
    CHECK_EQ("sprite", gpu.ReadVram(104, 101) == 0 && gpu.ReadVram(105, 101) != 0 && gpu.ReadVram(133, 100) != 0, 1);
    CHECK_EQ("sprite right", gpu.ReadVram(135, 100), 0);
    Send(gpu, {0x65000000, XY(140, 150), 0, XY(20, 1)});
    CHECK_EQ("sprite part", CountSet(gpu, 0, 150, 320, 1), 15);
    CHECK_EQ("sprite part left", gpu.ReadVram(135, 150) != 0 && gpu.ReadVram(134, 150) == 0, 1);
    // A piece that does not touch the others (an arm 12 px away) is still the same character: same
    // centre 120, so 172..180 -> 159..165 (around its own centre it would stay at 173..179).
    Send(gpu, {0x65000000, XY(172, 152), 0, XY(8, 1)});
    CHECK_EQ("sprite arm", CountSet(gpu, 0, 152, 320, 1), 6);
    CHECK_EQ("sprite arm left", gpu.ReadVram(159, 152) != 0 && gpu.ReadVram(158, 152) == 0, 1);
    // An untextured primitive ends the scene run: HUD line 10..50 -> 48..78
    Send(gpu, {0x40FFFFFF, XY(10, 220), XY(50, 220)});
    CHECK_EQ("HUD line", gpu.ReadVram(47, 220) == 0 && gpu.ReadVram(48, 220) != 0 && gpu.ReadVram(78, 220) != 0, 1);
    Send(gpu, {0x65000000, XY(10, 2), 0x00010000, XY(40, 1)});  // textured HUD rect 10..49 -> 48..77
    CHECK_EQ("HUD rect", CountSet(gpu, 0, 2, 320, 1), 30);
    CHECK_EQ("HUD rect left", gpu.ReadVram(48, 2) != 0 && gpu.ReadVram(47, 2) == 0, 1);
    Send(gpu, {0x60FFFFFF, XY(0, 1), XY(320, 1)});    // full width (fade), unchanged
    CHECK_EQ("full-width rect", CountSet(gpu, 0, 1, 320, 1), 320);
    // (a state command ends the fade's overlay run) one GTE vertex only: chance, HUD
    Send(gpu, {0xE100010A, 0x200000FF, XY(10, 60), XY(50, 10), XY(10, 100)});
    CHECK_EQ("1-match tri", gpu.ReadVram(10, 61) == 0 && gpu.ReadVram(50, 61) != 0, 1);

    // Screen-anchored overlay (the corridor's darkening vignette): a semi-transparent piece touching the
    // right edge stays as drawn (spans the wide picture), and so does the untextured piece after it.
    Send(gpu, {0x2AFFFFFF, XY(280, 10), XY(320, 10), XY(280, 20), XY(320, 20)});
    CHECK_EQ("overlay edge piece", gpu.ReadVram(282, 12) != 0 && gpu.ReadVram(318, 12) != 0, 1);
    Send(gpu, {0x2AFFFFFF, XY(200, 30), XY(280, 30), XY(200, 40), XY(280, 40)});
    CHECK_EQ("overlay inner piece", gpu.ReadVram(202, 32) != 0 && gpu.ReadVram(270, 32) != 0, 1);
    // A state command ends the overlay run: the same quad is HUD again, 200..280 -> 190..250.
    Send(gpu, {0xE100010A, 0x28FFFFFF, XY(200, 50), XY(280, 50), XY(200, 60), XY(280, 60)});
    CHECK_EQ("HUD after overlay", gpu.ReadVram(195, 52) != 0 && gpu.ReadVram(260, 52) == 0, 1);

    // Anchor: a character's shadow is a GTE triangle fan centred on (240, 120); its sprite pieces are
    // squeezed around x=240: head 236..244 -> 237..243, arm 260..268 (16 px off, same character) -> 255..261.
    for (int32_t px : {240, 230, 250}) ws.NoteProjected(XY(px, px == 240 ? 120 : 125));
    ws.NoteProjected(XY(240, 130));
    Send(gpu, {0x20000040, XY(240, 120), XY(230, 125), XY(240, 130)});
    Send(gpu, {0x20000040, XY(240, 120), XY(240, 130), XY(250, 125)});
    Send(gpu, {0x65000000, XY(236, 100), 0, XY(8, 1)});
    CHECK_EQ("fan head", CountSet(gpu, 200, 100, 120, 1), 6);
    CHECK_EQ("fan head left", gpu.ReadVram(237, 100) != 0 && gpu.ReadVram(236, 100) == 0, 1);
    Send(gpu, {0x65000000, XY(260, 101), 0, XY(8, 1)});
    CHECK_EQ("fan arm", gpu.ReadVram(255, 101) != 0 && gpu.ReadVram(254, 101) == 0 && gpu.ReadVram(261, 101) == 0, 1);

    // Textured HUD rect 4 -> 3 px: nearest texture columns 0, 1, 2 (x 157..159).
    for (int i = 0; i < 4; ++i) gpu.WriteVram(640 + i, 0, static_cast<uint16_t>(i + 1));
    Send(gpu, {0xE100010A, 0x65000000, XY(156, 200), 0x00010000, XY(4, 1)});
    CHECK_EQ("tex rect 0", gpu.ReadVram(157, 200), 1);
    CHECK_EQ("tex rect 1", gpu.ReadVram(158, 200), 2);
    CHECK_EQ("tex rect 2", gpu.ReadVram(159, 200), 3);
    CHECK_EQ("tex rect end", gpu.ReadVram(160, 200), 0);

    Send(gpu, {0xE5000000, 0x60FFFFFF, XY(0, 3), XY(40, 1)});  // new drawing env: background again
    CHECK_EQ("next frame background", CountSet(gpu, 0, 3, 320, 1), 40);
    for (int i = 0; i <= Widescreen2D::kKeepFrames; ++i) ws.NextFrame();
    CHECK_EQ("ws stale", ws.Projected(XY(10, 10)), 0);
    for (int i = 0; i < 30; ++i) ws.NextFrame();
    CHECK_EQ("ws back to 4:3", ws.Wide(), 0);
}

// Widescreen classifier rules on their own (drawing area 0..319): sticky per-texture HUD decision and the
// 4:3 safety net for a large translucent layer that is neither centred nor screen-anchored.
static void TestWidescreenRules() {
    using personarm::Widescreen2D;
    Widescreen2D ws(3, 4);
    const uint32_t p3d[3] = {XY(10, 10), XY(50, 10), XY(10, 50)};
    auto frame = [&] { ws.NextFrame(); for (uint32_t p : p3d) ws.NoteProjected(p); };
    frame();
    auto tri3d = [&] { int32_t x[3] = {10, 50, 10}, y[3] = {10, 10, 50}; ws.Squeeze2D(p3d, x, y, 3, 0, 319, false, false, 0); };
    auto rect = [&](int32_t lo, int32_t hi, bool textured, bool semi, uint32_t tex) {
        int32_t x[2] = {lo, hi}, y[2] = {100, 120};
        return ws.Squeeze2D(nullptr, x, y, 2, 0, 319, textured, semi, tex);
    };
    // A portrait texture first seen as HUD stays HUD (centre 160) when one frame draws it right after 3D;
    // after kStickyFrames frames of that it follows the stream (own centre 100).
    tri3d();
    ws.OtherCommand(false);
    CHECK_EQ("sticky first HUD", rect(80, 120, true, false, 7), 160);
    for (int i = 1; i < Widescreen2D::kStickyFrames; ++i) {
        frame();
        tri3d();
        CHECK_EQ("sticky keeps HUD", rect(80, 120, true, false, 7), 160);
    }
    frame();
    tri3d();
    CHECK_EQ("sticky flips", rect(80, 120, true, false, 7), 100);

    // Safety net: a centred translucent box (dialogue window) is HUD; an off-centre translucent layer
    // 10..250 cannot be placed: fallback to 4:3 (nothing squeezed, GTE not widened), sticky for
    // kFallbackFrames frames after the last one.
    ws.OtherCommand(false);
    CHECK_EQ("centred box", rect(16, 306, false, true, 0), 160);
    CHECK_EQ("no fallback", ws.Fallback(), 0);
    CHECK_EQ("uncertain layer", rect(10, 250, false, true, 0), Widescreen2D::kNone);
    CHECK_EQ("fallback", ws.Fallback() && !ws.Wide(), 1);
    CHECK_EQ("fallback: HUD not squeezed", rect(100, 140, false, false, 0), Widescreen2D::kNone);
    for (int i = 1; i < Widescreen2D::kFallbackFrames; ++i) frame();
    CHECK_EQ("fallback sticky", ws.Fallback(), 1);
    frame();
    CHECK_EQ("fallback over", ws.Fallback() == 0 && ws.Wide(), 1);
}

// Running party (field, 16:9): the shadows of two characters are drawn first (GTE fans centred on x 100 and
// 130), then their sprites, each with its own palette pair. Bunched up, Ulala's pieces start 8 px after
// Maya's: they must still be squeezed around her own shadow (130), not Maya's (100). One character's two
// palettes side by side (CLUT 0x3E54 / 0x3E55) stay one character.
static void TestWidescreenParty() {
    using personarm::Widescreen2D;
    Widescreen2D ws(3, 4);
    ws.NextFrame();
    auto fan = [&](int32_t cx) {
        const uint32_t c = XY(cx, 150), a = XY(cx - 10, 155), b = XY(cx, 160), d = XY(cx + 10, 155);
        for (uint32_t p : {c, a, b, d}) ws.NoteProjected(p);
        const uint32_t t1[3] = {c, a, b}, t2[3] = {c, b, d};
        int32_t x1[3] = {cx, cx - 10, cx}, y1[3] = {150, 155, 160}, x2[3] = {cx, cx, cx + 10}, y2[3] = {150, 160, 155};
        ws.Squeeze2D(t1, x1, y1, 3, 0, 319, false, false, 0);
        ws.Squeeze2D(t2, x2, y2, 3, 0, 319, false, false, 0);
    };
    auto piece = [&](int32_t lo, int32_t hi, uint32_t clut) {
        int32_t x[4] = {lo, hi, lo, hi}, y[4] = {100, 100, 130, 130};
        return ws.Squeeze2D(nullptr, x, y, 4, 0, 319, true, false, 0x0005 | clut << 16);
    };
    fan(100);
    fan(130);
    CHECK_EQ("maya body", piece(90, 110, 0x3E14), 100);
    CHECK_EQ("maya arm", piece(104, 112, 0x3E14), 100);
    CHECK_EQ("ulala body (touching maya)", piece(120, 140, 0x3E54), 130);
    CHECK_EQ("ulala second palette", piece(118, 126, 0x3E55), 130);
    CHECK_EQ("ulala legs", piece(122, 138, 0x3E54), 130);
    // A corpse (no shadow) and its blood pool in another palette stay one: both around the corpse's centre.
    CHECK_EQ("corpse", piece(200, 240, 0x3F94), 220);
    CHECK_EQ("blood pool", piece(236, 260, 0x3E24), 220);
}

int main() {
    TestWidescreen2D();
    TestWidescreenRules();
    TestWidescreenParty();
    TestFillRule();
    TestGouraud();
    TestTextures();
    TestSemiModes();
    TestClipOffsetMask();
    TestLines();
    TestSprites();
    TestGpuRead();
    TestGp1ResetKeepsVram();
    std::printf("GPU rasterizer: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
