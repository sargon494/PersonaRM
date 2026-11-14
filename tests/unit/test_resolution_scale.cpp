// Internal resolution (Ps1Gpu::SetResolutionScale): the scaled mirror against independent expectations
// for simple shapes, and the 1x VRAM / GPUREAD / GPUSTAT the game sees unchanged by any scale.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "host/host_logic.h"
#include "rendering/ps1_gpu.h"

using personarm::Ps1Gpu;

static uint32_t XY(int32_t x, int32_t y) { return (uint32_t(x) & 0xFFFF) | uint32_t(y) << 16; }
static void Send(Ps1Gpu& g, const std::vector<uint32_t>& w) {
    for (uint32_t x : w) g.WriteGP0(x);
}
static uint16_t Hi(const Ps1Gpu& g, uint32_t x, uint32_t y) {
    return g.GetScaledVram()[size_t(y) * 1024 * g.GetResolutionScale() + x];
}

// Full-VRAM drawing area, zero offset, E1 = 0.
static const std::vector<uint32_t> kSetup = {0xE3000000, 0xE4000000 | 1023 | (511 << 10), 0xE5000000, 0xE1000000};

// Every scaled pixel equals the 1x pixel it covers.
static bool IsNearestUpscale(const Ps1Gpu& g) {
    const uint32_t s = g.GetResolutionScale();
    for (uint32_t y = 0; y < 512 * s; ++y)
        for (uint32_t x = 0; x < 1024 * s; ++x)
            if (Hi(g, x, y) != g.ReadVram(x / s, y / s)) return false;
    return true;
}

static void TestFlatTriangleEdges() {
    // Right triangle (10,20) + legs of 8 px, red. Samples at integer points, top-left rule: the
    // hypotenuse (a right/bottom edge) is excluded, so 1x covers x'+y' < 8 (36 px) and 2x covers
    // X'+Y' < 16 on the 2x grid (136 px).
    const std::vector<uint32_t> tri = {0x200000F8, XY(10, 20), XY(18, 20), XY(10, 28)};
    Ps1Gpu a, b;
    b.SetResolutionScale(2);
    Send(a, kSetup), Send(a, tri);
    Send(b, kSetup), Send(b, tri);
    assert(a.GetVram() == b.GetVram());
    uint32_t n1 = 0, n2 = 0;
    for (uint32_t y = 0; y < 512; ++y)
        for (uint32_t x = 0; x < 1024; ++x) {
            const bool in = x >= 10 && y >= 20 && (x - 10) + (y - 20) < 8;
            assert((a.ReadVram(x, y) == 0x001F) == in);
            n1 += in;
        }
    for (uint32_t y = 0; y < 1024; ++y)
        for (uint32_t x = 0; x < 2048; ++x) {
            const bool in = x >= 20 && y >= 40 && (x - 20) + (y - 40) < 16;
            assert((Hi(b, x, y) == 0x001F) == in);
            n2 += in;
        }
    assert(n1 == 36 && n2 == 136);
}

static void TestAxisAlignedShapesUpscaleExactly() {
    // Flat quad, gouraud rectangle-ish quad without dithering, textured rectangle (15-bit texels uploaded
    // with A0h), fill, VRAM copy: at 3x the scaled VRAM is exactly the nearest upscale of 1x.
    Ps1Gpu g;
    g.SetResolutionScale(3);
    Send(g, kSetup);
    Send(g, {0x28001F00, XY(4, 4), XY(20, 4), XY(4, 12), XY(20, 12)});        // flat quad
    Send(g, {0x02FF0000, XY(100, 100), XY(16, 4)});                              // fill
    Send(g, {0xA0000000, XY(512, 0), XY(4, 2), 0x7C1F03E0, 0x00011234, 0x55554321, 0x0F0F7FFF});
    Send(g, {0xE1000000 | 8 | (2 << 7)});                                         // texpage x=512, 15-bit
    Send(g, {0x65808080, XY(40, 40), 0x00000000, XY(4, 2)});                      // raw textured rect
    Send(g, {0x80000000, XY(40, 40), XY(60, 60), XY(4, 2)});                      // VRAM copy
    assert(IsNearestUpscale(g));
}

static void TestGameSeesTheSame() {
    // A mixed list (gouraud dithered triangle, semi-transparent quad, polyline, flipped textured rect,
    // mask bit) must leave the 1x VRAM, GPUSTAT and a C0h readback identical at every scale.
    std::vector<uint32_t> cmds = kSetup;
    const std::vector<uint32_t> more = {
        0xA0000000, XY(512, 0), XY(4, 2), 0x7C1F03E0, 0x80011234, 0x55554321, 0x0F0F7FFF,
        0xE1000200 | 8 | (2 << 7),                                                       // dither on
        0x300000FF, XY(0, 0), 0x0000FF00, XY(60, 5), 0x00FF0000, XY(15, 50),            // gouraud tri
        0x2A404040, XY(10, 10), XY(50, 12), XY(8, 40), XY(52, 44),                      // semi quad
        0x34808080, XY(80, 0), 0x00000000, 0x00404040, XY(150, 20), 0x01080003, 0x00FF8080, XY(90, 60), 0x00000103,  // tex gouraud tri
        0x4800FFFF, XY(0, 60), XY(30, 70), XY(5, 90), 0x55555555,                       // polyline
        0xE1000000 | 8 | (2 << 7) | (1 << 12) | (1 << 13), 0x64808080, XY(70, 70), 0x00000301, XY(4, 2),
        0xE6000003, 0x60FFFFFF, XY(0, 0), XY(30, 30),                                   // mask set+check
        0xC0000000, XY(0, 0), XY(64, 64)};
    cmds.insert(cmds.end(), more.begin(), more.end());
    Ps1Gpu ref;
    Send(ref, cmds);
    const uint32_t ref_stat = ref.ReadGPUSTAT();
    std::vector<uint32_t> ref_read;
    for (int i = 0; i < 64 * 64 / 2; ++i) ref_read.push_back(ref.ReadGP0());
    for (uint32_t s = 1; s <= 4; ++s) {
        Ps1Gpu g;
        g.SetResolutionScale(s);
        assert(g.GetResolutionScale() == s && g.GetScaledVram().size() == (s > 1 ? 1024u * 512 * s * s : 0u));
        Send(g, cmds);
        assert(g.GetVram() == ref.GetVram());
        assert(g.ReadGPUSTAT() == ref_stat);
        for (uint32_t w : ref_read) assert(g.ReadGP0() == w);
        if (s == 1) continue;
        assert(Hi(g, 2 * s, 2 * s) & 0x8000);  // mask bit written on the scaled target too
        // Every s x s block corner samples where the 1x raster did: same pixel (shading, texel, blend).
        for (uint32_t y = 0; y < 512; ++y)
            for (uint32_t x = 0; x < 1024; ++x) assert(Hi(g, x * s, y * s) == g.ReadVram(x, y));
    }
}

static void TestHostDisplay() {
    Ps1Gpu g;
    g.SetResolutionScale(2);
    Send(g, kSetup);
    Send(g, {0x200000F8, XY(0, 0), XY(4, 0), XY(0, 4)});
    std::vector<uint32_t> out;
    personarm::host::DisplayToRgba(g.GetScaledVram(), 0, 0, 4, 4, false, out, 2);
    assert(out.size() == 64);
    assert(out[0] == 0xFF0000F8u && out[7] == 0xFF0000F8u);   // row 0: X < 8
    assert(out[8 + 6] == 0xFF0000F8u && out[8 + 7] == 0xFF000000u);  // row 1: X < 7

    personarm::host::HostOptions o;
    const char* ok[] = {"x", "d.cue", "--resolution-scale", "4"};
    assert(personarm::host::ParseHostArgs(4, ok, o) && o.resolution_scale == 4);
    const char* dflt[] = {"x", "d.cue"};
    assert(personarm::host::ParseHostArgs(2, dflt, o) && o.resolution_scale == 1);
    const char* bad[] = {"x", "d.cue", "--resolution-scale", "5"};
    assert(!personarm::host::ParseHostArgs(4, bad, o));
    const char* interp[] = {"x", "d.cue", "--resolution-scale", "2", "--interp", "120"};
    assert(!personarm::host::ParseHostArgs(6, interp, o));
}

int main() {
    TestFlatTriangleEdges();
    TestAxisAlignedShapesUpscaleExactly();
    TestGameSeesTheSame();
    TestHostDisplay();
    std::printf("resolution scale: all tests passed\n");
    return 0;
}
