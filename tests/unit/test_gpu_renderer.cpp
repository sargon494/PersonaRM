// Host GPU renderer (needs a D3D12 device; skipped without one): the same primitives drawn into two
// display buffers must give the same scaled picture. Fine samples of a 1:1 textured polygon and of a
// gouraud gradient land exactly on texel / colour-step boundaries at even scales; rounded either way by
// the fp32 interpolation, the two buffers differed and a static menu alternated every game frame.
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

// A P2 menu frame drawn into the buffer at (ox, oy), as the game does it (disc 1 JP, f3600): three 54x67
// portrait quads sampled 1:1, each at its own drawing offset, and a gouraud gradient bar. The texture
// (15bpp, a different colour per texel) is at 768,0, outside the buffers.
static void DrawFrame(Ps1Gpu& g, int32_t ox, int32_t oy) {
    const uint32_t tp = 12 | 2 << 7;  // texpage x 768, 15bpp
    Send(g, {0xE3000000, 0xE4000000 | 1023 | (511 << 10), 0x02000000, XY(ox, oy), XY(320, 240)});  // clear the buffer
    for (int32_t i = 0; i < 3; ++i) {
        const int32_t u = 54 * (i % 2), v = 67 * (i / 2);
        Send(g, {0xE5000000 | (uint32_t(ox + 12 + 60 * i) & 0x7FF) | (uint32_t(oy + 104) & 0x7FF) << 11});
        Send(g, {0x2C808080, XY(3, 3), XY(u, v), XY(57, 3), XY(u + 54, v) | tp << 16, XY(3, 70), XY(u, v + 67), XY(57, 70),
                 XY(u + 54, v + 67)});
    }
    Send(g, {0xE5000000 | (uint32_t(ox) & 0x7FF) | (uint32_t(oy) & 0x7FF) << 11});
    Send(g, {0x38000000, XY(200, 30), 0x0000FF00, XY(310, 30), 0x00000000, XY(200, 230), 0x00FF0000, XY(310, 230)});
}

int main() {
    Ps1Gpu gpu;
    std::vector<uint32_t> tex = {0xA0000000, XY(768, 0), XY(128, 136)};
    for (uint32_t i = 0; i < 128 * 136 / 2; ++i) {
        const uint32_t a = (i * 2 * 2654435761u) >> 17 & 0x7FFF, b = ((i * 2 + 1) * 2654435761u) >> 17 & 0x7FFF;
        tex.push_back((a | 1) | (b | 1) << 16);
    }
    Send(gpu, tex);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("test_gpu_renderer: skipped (%s)\n", SDL_GetError());
        return 0;
    }
    // Double buffers as games place them: P2's (0,0) / (0,240), and others.
    const int32_t buffers[][2] = {{0, 0}, {0, 240}, {320, 0}, {0, 256}};
    for (uint32_t scale : {2u, 3u, 4u, 6u, 8u, 9u}) {
        GpuRenderer r;
        std::string err;
        if (!r.Init(gpu, scale, &err)) {
            std::printf("test_gpu_renderer: skipped (no GPU device: %s)\n", err.c_str());
            return 0;
        }
        std::vector<std::vector<uint32_t>> pics;
        for (const auto& b : buffers) {  // each picture read before the next buffer (overlapping) is drawn
            DrawFrame(gpu, b[0], b[1]);
            personarm::GpuDisplayArea d;
            d.x = uint16_t(b[0]), d.y = uint16_t(b[1]);
            std::vector<uint32_t> rgba;
            assert(r.Present(d, true, &rgba));
            pics.push_back(rgba);
        }
        for (size_t i = 1; i < pics.size(); ++i) {
            size_t diff = 0;
            for (size_t k = 0; k < pics[0].size(); ++k) diff += pics[i][k] != pics[0][k];
            std::fprintf(stderr, "%ux, buffer %d,%d: %zu px differ from buffer 0,0\n", scale, buffers[i][0], buffers[i][1], diff);
            assert(diff == 0);
        }
    }
    SDL_Quit();
    std::printf("test_gpu_renderer: all passed\n");
    return 0;
}
