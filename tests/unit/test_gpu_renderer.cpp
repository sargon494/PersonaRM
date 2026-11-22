// Host GPU renderer (needs a D3D12 device; skipped without one): the same primitives drawn into two
// display buffers must give the same scaled picture, and the picture of the CPU's scaled pass.
// - Fine samples of a 1:1 textured polygon and of a gouraud gradient land exactly on texel / colour-step
//   boundaries at even scales; rounded either way by the fp32 interpolation, the two buffers differed and
//   a static menu alternated every game frame.
// - A rotated, mirrored quad (P2 JP area 36, a field object) samples a UV 1/512 + 1.5e-6 below a texel
//   boundary at 4x: the fp32 interpolation plus the old tie bias landed on either side per buffer (stray
//   pixels toggling every other frame).
// - B/2+F/2 of an odd sum: the target held c5/31, so the blend rounded the half up; the CPU floors.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "rendering/gpu_renderer.h"
#include "rendering/ps1_gpu.h"

using personarm::GpuRenderer;
using personarm::Ps1Gpu;

static uint32_t XY(int32_t x, int32_t y) { return (uint32_t(x) & 0xFFFF) | uint32_t(y) << 16; }
static void Send(Ps1Gpu& g, const std::vector<uint32_t>& w) {
    for (uint32_t x : w) g.WriteGP0(x);
}
static void Begin(Ps1Gpu& g, int32_t ox, int32_t oy, uint32_t e1) {  // clear the buffer, draw into it
    Send(g, {0xE1000000 | e1, 0xE3000000 | uint32_t(ox) | uint32_t(oy) << 10,
             0xE4000000 | uint32_t(ox + 319) | uint32_t(oy + 239) << 10, 0x02000000, XY(ox, oy), XY(320, 240)});
    Send(g, {0xE5000000 | (uint32_t(ox) & 0x7FF) | (uint32_t(oy) & 0x7FF) << 11});
}

// A P2 menu frame drawn into the buffer at (ox, oy), as the game does it (disc 1 JP, f3600): three 54x67
// portrait quads sampled 1:1, each at its own drawing offset, and a gouraud gradient bar. The texture
// (15bpp, a different colour per texel, 128x256) is at 768,0, outside the buffers.
static void DrawMenu(Ps1Gpu& g, int32_t ox, int32_t oy) {
    const uint32_t tp = 12 | 2 << 7;  // texpage x 768, 15bpp
    Begin(g, ox, oy, 0);
    for (int32_t i = 0; i < 3; ++i) {
        const int32_t u = 54 * (i % 2), v = 67 * (i / 2);
        Send(g, {0xE5000000 | (uint32_t(ox + 12 + 60 * i) & 0x7FF) | (uint32_t(oy + 104) & 0x7FF) << 11});
        Send(g, {0x2C808080, XY(3, 3), XY(u, v), XY(57, 3), XY(u + 54, v) | tp << 16, XY(3, 70), XY(u, v + 67), XY(57, 70),
                 XY(u + 54, v + 67)});
    }
    Send(g, {0xE5000000 | (uint32_t(ox) & 0x7FF) | (uint32_t(oy) & 0x7FF) << 11});
    Send(g, {0x38000000, XY(200, 30), 0x0000FF00, XY(310, 30), 0x00000000, XY(200, 230), 0x00FF0000, XY(310, 230)});
}

// The area-36 quad (same vertices and UVs, over the test texture), dithered as there.
static void DrawNearTie(Ps1Gpu& g, int32_t ox, int32_t oy) {
    const uint32_t tp = 12 | 2 << 7 | 1 << 9;
    Begin(g, ox, oy, 1 << 9);
    Send(g, {0x2C808080, XY(-7, -8), XY(62, 129), XY(42, -25), XY(1, 129) | tp << 16, XY(-2, 49), XY(62, 190), XY(45, 31),
             XY(1, 190)});
}

// Flat B/2+F/2 quads (E1h semi mode 0) of every 5-bit level over bands of every 5-bit level.
static void DrawHalfBlend(Ps1Gpu& g, int32_t ox, int32_t oy) {
    Begin(g, ox, oy, 0);
    for (int32_t b = 0; b < 32; ++b) Send(g, {0x02000000 | uint32_t(b << 3) * 0x010101u, XY(ox, oy + 7 * b), XY(320, 7)});
    for (int32_t c = 0; c < 32; ++c)
        Send(g, {0x2A000000 | uint32_t(c << 3) * 0x010101u, XY(c * 10, 0), XY(c * 10 + 10, 0), XY(c * 10, 240), XY(c * 10 + 10, 240)});
}

// The CPU's scaled pass of the display area (BGR555 -> 0xAABBGGRR, as host::DisplayToRgba).
static std::vector<uint32_t> CpuPicture(const Ps1Gpu& g, int32_t x, int32_t y, uint32_t s) {
    const std::vector<uint16_t>& hi = g.GetScaledVram();
    std::vector<uint32_t> out(size_t(320 * s) * 240 * s);
    for (uint32_t r = 0; r < 240 * s; ++r)
        for (uint32_t c = 0; c < 320 * s; ++c) {
            const uint16_t p = hi[size_t(y * s + r) * 1024 * s + x * s + c];
            out[size_t(r) * 320 * s + c] = 0xFF000000u | (p & 31u) << 3 | ((p >> 5) & 31u) << 11 | ((p >> 10) & 31u) << 19;
        }
    return out;
}

static size_t Diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t n = 0;
    for (size_t k = 0; k < a.size(); ++k) n += a[k] != b[k];
    return n;
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("test_gpu_renderer: skipped (%s)\n", SDL_GetError());
        return 0;
    }
    std::vector<uint32_t> tex = {0xA0000000, XY(768, 0), XY(128, 256)};
    for (uint32_t i = 0; i < 128 * 256 / 2; ++i) {
        const uint32_t a = (i * 2 * 2654435761u) >> 17 & 0x7FFF, b = ((i * 2 + 1) * 2654435761u) >> 17 & 0x7FFF;
        tex.push_back((a | 1) | (b | 1) << 16);
    }
    // Double buffers as games place them: P2's (0,0) / (0,240), and others.
    const int32_t buffers[][2] = {{0, 0}, {0, 240}, {320, 0}, {0, 256}};
    const struct { const char* name; void (*draw)(Ps1Gpu&, int32_t, int32_t); } scenes[] = {
        {"menu", DrawMenu}, {"near tie", DrawNearTie}, {"B/2+F/2", DrawHalfBlend}};
    for (uint32_t scale : {2u, 3u, 4u, 6u, 8u, 9u}) {
        Ps1Gpu gpu;
        if (scale <= 4) gpu.SetResolutionScale(scale);  // the CPU's scaled pass (2-4) as the reference
        Send(gpu, tex);
        GpuRenderer r;
        std::string err;
        if (!r.Init(gpu, scale, &err)) {
            std::printf("test_gpu_renderer: skipped (no GPU device: %s)\n", err.c_str());
            return 0;
        }
        for (const auto& sc : scenes) {
            std::vector<uint32_t> first;
            for (const auto& b : buffers) {  // each picture read before the next buffer (overlapping) is drawn
                sc.draw(gpu, b[0], b[1]);
                personarm::GpuDisplayArea d;
                d.x = uint16_t(b[0]), d.y = uint16_t(b[1]);
                std::vector<uint32_t> rgba;
                assert(r.Present(d, true, &rgba));
                const size_t cpu = scale <= 4 ? Diff(rgba, CpuPicture(gpu, b[0], b[1], scale)) : 0;
                const size_t buf = first.empty() ? 0 : Diff(rgba, first);
                if (first.empty()) first = rgba;
                std::fprintf(stderr, "%ux %s, buffer %d,%d: %zu px differ from buffer 0,0, %zu from the CPU\n", scale, sc.name,
                             b[0], b[1], buf, cpu);
                assert(buf == 0 && cpu == 0);
            }
        }
    }
    SDL_Quit();
    std::printf("test_gpu_renderer: all passed\n");
    return 0;
}
