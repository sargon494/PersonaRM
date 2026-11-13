// Frame interpolation (rendering/frame_interp.h) on synthetic GP0 streams: primitive matching, vertex
// lerp, capture completeness (replaying a captured frame redraws it exactly) and that the capture hook
// leaves emulation byte-identical.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "rendering/frame_interp.h"

using namespace personarm;
using namespace personarm::interp;

static uint32_t XY(int32_t x, int32_t y) { return (uint32_t(x) & 0xFFFF) | uint32_t(y) << 16; }

static void Packet(std::vector<uint32_t>& cmds, std::initializer_list<uint32_t> words) {
    cmds.push_back(uint32_t(words.size()));
    cmds.insert(cmds.end(), words);
}

// Textured flat quad (2Ch) at (x, y), 16x16, UV origin (u, 0); CLUT 0, texpage 0.
static void TexQuad(std::vector<uint32_t>& c, int x, int y, uint32_t u) {
    Packet(c, {0x2C808080, XY(x, y), u, XY(x + 16, y), 0x00000000 | (u + 16), XY(x, y + 16), u | 16 << 8,
               XY(x + 16, y + 16), (u + 16) | 16 << 8});
}

static int32_t X0(const std::vector<uint32_t>& c, size_t packet_word) { return int16_t(c[packet_word + 2] & 0xFFFF); }

static void TestMatchAndLerp() {
    std::vector<uint32_t> prev, cur, out;
    Packet(prev, {0xE1000000});
    TexQuad(prev, 10, 20, 0);    // A
    TexQuad(prev, 100, 20, 64);  // B
    // cur: OT order swapped, A moved +20 x, B moved -40 x, plus a new primitive C with no partner.
    Packet(cur, {0xE1000000});
    TexQuad(cur, 60, 20, 64);   // B
    TexQuad(cur, 30, 20, 0);    // A
    TexQuad(cur, 200, 50, 128); // C
    const Match m = MatchPrimitives(prev, cur);
    assert(m.prims == 3 && m.matched == 2);
    assert(m.prev_of.size() == 4 && m.prev_of[0] == -1 && m.prev_of[3] == -1);
    LerpCommands(prev, cur, m, 0.5, 48, out);
    assert(out.size() == cur.size());
    assert(X0(out, 2) == 80);   // B: 100 -> 60
    assert(X0(out, 12) == 20);  // A: 10 -> 30
    assert(X0(out, 22) == 200); // C: unmatched, as in cur
    assert(out[22 + 1 + 1] == cur[22 + 1 + 1] && out[0] == cur[0]);
    // t = 1 is cur; t = 0 is prev's geometry with cur's draw order.
    LerpCommands(prev, cur, m, 1.0, 48, out);
    assert(out == cur);
    LerpCommands(prev, cur, m, 0.0, 48, out);
    assert(X0(out, 2) == 100 && X0(out, 12) == 10);
    // A jump beyond the limit (teleport) is not lerped.
    LerpCommands(prev, cur, m, 0.5, 30, out);
    assert(X0(out, 2) == 60 && X0(out, 12) == 20);
    // Negative coordinates survive the 11-bit round trip.
    std::vector<uint32_t> p2, c2;
    TexQuad(p2, -30, -10, 0);
    TexQuad(c2, -10, -10, 0);
    const Match m2 = MatchPrimitives(p2, c2);
    LerpCommands(p2, c2, m2, 0.25, 48, out);
    assert(X0(out, 0) == -25);
    // Same key (repeated tile) drawn in another order: the nearest one pairs, not the first.
    std::vector<uint32_t> p3, c3;
    TexQuad(p3, 10, 0, 32);
    TexQuad(p3, 50, 0, 32);
    TexQuad(c3, 52, 0, 32);
    TexQuad(c3, 12, 0, 32);
    const Match m3 = MatchPrimitives(p3, c3);
    assert(m3.matched == 2 && m3.prev_of[0] == 10 && m3.prev_of[1] == 0);
    std::printf("  match/lerp ok\n");
}

static void TestUntexturedAndRects() {
    std::vector<uint32_t> prev, cur, out;
    Packet(prev, {0x200000FF, XY(0, 0), XY(10, 0), XY(0, 10)});  // red flat triangle
    Packet(prev, {0x2000FF00, XY(50, 0), XY(60, 0), XY(50, 10)}); // green
    Packet(prev, {0x60FFFFFF, XY(5, 5), XY(8, 4)});               // 8x4 white rect
    Packet(cur, {0x2000FF00, XY(54, 0), XY(64, 0), XY(54, 10)});
    Packet(cur, {0x200000FF, XY(4, 0), XY(14, 0), XY(4, 10)});
    Packet(cur, {0x60FFFFFF, XY(9, 5), XY(8, 4)});
    Packet(cur, {0x60FFFFFF, XY(9, 5), XY(9, 4)});  // different size: no partner
    const Match m = MatchPrimitives(prev, cur);
    assert(m.prims == 4 && m.matched == 3);
    LerpCommands(prev, cur, m, 0.5, 48, out);
    assert(X0(out, 0) == 52 && X0(out, 5) == 2 && X0(out, 10) == 7 && X0(out, 14) == 9);
    // A polyline continuation word that looks like a polygon opcode is not a primitive.
    std::vector<uint32_t> poly_tail;
    Packet(poly_tail, {0x3C00F00F});
    assert(MatchPrimitives(poly_tail, poly_tail).prims == 0);
    std::printf("  untextured/rect ok\n");
}

// A frame's worth of GP0 traffic including an upload, a polyline, fills and a flip.
static void DrawScene(Ps1Gpu& g, int shift) {
    for (uint32_t w : {0xE3000000u, 0xE4000000u | 319 | 239 << 10, 0xE5000000u, 0xE1000200u | 0x2})
        g.WriteGP0(w);
    g.WriteGP0(0x02102030); g.WriteGP0(XY(0, 0)); g.WriteGP0(XY(320, 240));  // fill
    g.WriteGP0(0xA0000000); g.WriteGP0(XY(640, 0)); g.WriteGP0(XY(4, 2));   // upload 4x2 15bpp texture
    for (uint32_t i = 0; i < 4; ++i) g.WriteGP0(0x7C1F03E0u + i);
    for (uint32_t w : {0x3000FF00u, XY(10 + shift, 10), 0x00FF0000u, XY(90 + shift, 20), 0x000000FFu, XY(30 + shift, 80)})
        g.WriteGP0(w);  // gouraud triangle
    for (uint32_t w : {0x2D808080u, XY(100 + shift, 100), 0u, XY(104 + shift, 100), 3u | 0x10Au << 16, XY(100 + shift, 102), 1u << 8,
                       XY(104 + shift, 102), 3u | 1u << 8})
        g.WriteGP0(w);  // raw textured quad, texpage 10 (x=640)
    for (uint32_t w : {0x48FFFFFFu, XY(0, 200), XY(50 + shift, 210), XY(80, 230), 0x55555555u}) g.WriteGP0(w);  // polyline
    for (uint32_t w : {0x62804020u, XY(200 + shift, 50), XY(30, 20)}) g.WriteGP0(w);  // semi-transparent rect
}

static void TestCaptureIsInvisibleAndComplete() {
    Ps1Gpu plain, logged;
    std::vector<uint32_t> log;
    logged.SetCommandLog(&log);
    for (Ps1Gpu* g : {&plain, &logged}) {
        DrawScene(*g, 0);
        g->WriteGP1(0x05000000 | 0 | 240 << 10);  // flip to (0,240)
        g->WriteGP1(0x05000000 | 0 | 240 << 10);  // same start: not a flip
        DrawScene(*g, 7);
    }
    assert(plain.GetVram() == logged.GetVram());  // capture on/off: byte-identical VRAM and status
    assert(plain.ReadGPUSTAT() == logged.ReadGPUSTAT());
    size_t markers = 0;
    for (size_t i = 0; i < log.size(); i += log[i] ? 1 + log[i] : 2) markers += log[i] == 0;
    assert(markers == 1);

    // Replaying the second segment (after the flip marker) on the VRAM it started from redraws it exactly.
    Ps1Gpu before;
    DrawScene(before, 0);
    size_t i = 0;
    while (log[i]) i += 1 + log[i];
    std::vector<uint32_t> seg(log.begin() + i + 2, log.end());
    Ps1Gpu replay;
    replay.SetVram(before.GetVram());
    // The upload is not captured (VRAM snapshots carry textures): the snapshot already holds it.
    Replay(replay, seg);
    assert(replay.GetVram() == plain.GetVram());

    // The interpolator: two flips give a pair; t=1 shows the latest frame, t=0 the previous.
    FrameInterpolator fi;
    Ps1Gpu g;
    std::vector<uint32_t> l2;
    g.SetCommandLog(&l2);
    uint64_t frame = 0;
    for (int k = 1; k <= 3; ++k) {
        const uint32_t y = (k & 1) ? 240 : 0;
        for (uint32_t w : {0xE3000000u | y << 10, 0xE4000000u | 319 | (y + 239) << 10, 0xE5000000u | y << 11}) g.WriteGP0(w);
        g.WriteGP0(0x02000000); g.WriteGP0(XY(0, y)); g.WriteGP0(XY(320, 240));
        for (uint32_t w : {0x20FFFFFFu, XY(10 + 8 * k, 10), XY(40 + 8 * k, 10), XY(10 + 8 * k, 40)}) g.WriteGP0(w);
        g.WriteGP1(0x05000000 | y << 10);
        frame += 2;
        const bool flipped = fi.Consume(l2, g, frame);
        assert(flipped && l2.empty());
    }
    assert(fi.Active(frame) && fi.PairOk() && fi.LastMatch().matched == 1);
    assert(fi.Fraction(double(frame)) == 0.0 && fi.Fraction(double(frame) + 1) == 0.5);
    const std::vector<uint16_t>* vram = nullptr;
    GpuDisplayArea d;
    fi.Render(1.0, vram, d);
    assert(*vram == g.GetVram() && d.y == 240);
    fi.Render(0.5, vram, d);
    // Frame 3 (display y=240) drew its left edge at x=34, frame 2 at x=26: halfway it is at 30.
    assert(d.y == 240 && (*vram)[260 * 1024 + 30] != 0 && (*vram)[260 * 1024 + 29] == 0);
    assert(!fi.Active(frame + FrameInterpolator::kStaleFrames + 1));
    std::printf("  capture/replay/interpolator ok\n");
}

int main() {
    std::printf("frame_interp tests\n");
    TestMatchAndLerp();
    TestUntexturedAndRects();
    TestCaptureIsInvisibleAndComplete();
    std::printf("PASS\n");
    return 0;
}
