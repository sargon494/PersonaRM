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

int main() {
    TestFillRule();
    TestGouraud();
    TestTextures();
    TestSemiModes();
    TestClipOffsetMask();
    TestLines();
    TestSprites();
    TestGpuRead();
    std::printf("GPU rasterizer: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
