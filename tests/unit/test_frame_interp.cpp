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
#include "rendering/widescreen_2d.h"

using namespace personarm;
using namespace personarm::interp;

static uint32_t XY(int32_t x, int32_t y) { return (uint32_t(x) & 0xFFFF) | uint32_t(y) << 16; }

static void Packet(std::vector<uint32_t>& cmds, std::initializer_list<uint32_t> words) {
    cmds.push_back(uint32_t(words.size()));
    cmds.insert(cmds.end(), words);
}

// The GPU's 3D tag (GTE vertices) before a polygon packet: only tagged polygons are interpolated.
static void Tag(std::vector<uint32_t>& c) { Packet(c, {Ps1Gpu::kLog3D}); }

// Textured flat quad (2Ch) at (x, y), 16x16, UV origin (u, 0); CLUT 0, texpage 0; 3D-tagged.
static void TexQuad(std::vector<uint32_t>& c, int x, int y, uint32_t u) {
    Tag(c);
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
    const Match m = MatchPrimitives(prev, cur, 48);
    assert(m.prims == 3 && m.matched == 2);
    assert(m.prev_of.size() == 7 && m.prev_of[0] == -1 && m.prev_of[6] == -1);
    LerpCommands(prev, cur, m, 0.5, 48, out);
    assert(out.size() == cur.size());
    assert(X0(out, 4) == 80);   // B: 100 -> 60
    assert(X0(out, 16) == 20);  // A: 10 -> 30
    assert(X0(out, 28) == 200); // C: unmatched, as in cur
    assert(out[28 + 1 + 1] == cur[28 + 1 + 1] && out[0] == cur[0]);
    // t = 1 is cur; t = 0 is prev's geometry with cur's draw order.
    LerpCommands(prev, cur, m, 1.0, 48, out);
    assert(out == cur);
    LerpCommands(prev, cur, m, 0.0, 48, out);
    assert(X0(out, 4) == 100 && X0(out, 16) == 10);
    // A jump beyond the limit (teleport) is not lerped.
    LerpCommands(prev, cur, m, 0.5, 30, out);
    assert(X0(out, 4) == 60 && X0(out, 16) == 20);
    // Negative coordinates survive the 11-bit round trip.
    std::vector<uint32_t> p2, c2;
    TexQuad(p2, -30, -10, 0);
    TexQuad(c2, -10, -10, 0);
    const Match m2 = MatchPrimitives(p2, c2, 48);
    LerpCommands(p2, c2, m2, 0.25, 48, out);
    assert(X0(out, 2) == -25);
    // Same key (repeated tile) drawn in another order: the nearest one pairs, not the first.
    std::vector<uint32_t> p3, c3;
    TexQuad(p3, 10, 0, 32);
    TexQuad(p3, 50, 0, 32);
    TexQuad(c3, 52, 0, 32);
    TexQuad(c3, 12, 0, 32);
    const Match m3 = MatchPrimitives(p3, c3, 48);
    assert(m3.matched == 2 && m3.prev_of[1] == 14 && m3.prev_of[3] == 2);
    std::printf("  match/lerp ok\n");
}

// Untextured 3D triangles pair by colour; 2D (rectangles, lines, untagged polygons) is drawn as in cur.
static void TestUntexturedAndRects() {
    std::vector<uint32_t> prev, cur, out;
    Tag(prev), Packet(prev, {0x200000FF, XY(0, 0), XY(10, 0), XY(0, 10)});  // red flat triangle
    Tag(prev), Packet(prev, {0x2000FF00, XY(50, 0), XY(60, 0), XY(50, 10)}); // green
    Packet(prev, {0x60FFFFFF, XY(5, 5), XY(8, 4)});                          // 8x4 white rect
    Packet(prev, {0x40FFFFFF, XY(0, 30), XY(20, 30)});                       // line
    Packet(prev, {0x20FFFFFF, XY(0, 50), XY(10, 50), XY(0, 60)});            // 2D triangle
    Tag(cur), Packet(cur, {0x2000FF00, XY(54, 0), XY(64, 0), XY(54, 10)});
    Tag(cur), Packet(cur, {0x200000FF, XY(4, 0), XY(14, 0), XY(4, 10)});
    Packet(cur, {0x60FFFFFF, XY(9, 5), XY(8, 4)});
    Packet(cur, {0x40FFFFFF, XY(4, 30), XY(24, 30)});
    Packet(cur, {0x20FFFFFF, XY(4, 50), XY(14, 50), XY(4, 60)});
    const Match m = MatchPrimitives(prev, cur, 48);
    assert(m.prims == 2 && m.matched == 2);
    LerpCommands(prev, cur, m, 0.5, 48, out);
    assert(X0(out, 2) == 52 && X0(out, 9) == 2);
    for (size_t i = 14; i < cur.size(); ++i) assert(out[i] == cur[i]);
    // A polyline continuation word that looks like a polygon opcode is not a primitive.
    std::vector<uint32_t> poly_tail;
    Packet(poly_tail, {0x3C00F00F});
    assert(MatchPrimitives(poly_tail, poly_tail, 48).prims == 0);
    std::printf("  untextured/rect ok\n");
}

// A floor of identical tiles scrolls 4 px while a tile is inserted mid-list (drawn first, next to an old
// one) and another is removed: every surviving tile pairs with itself, the new one is drawn as in cur.
static void TestInsertedAndRemovedTiles() {
    std::vector<uint32_t> prev, cur, out;
    for (int x : {0, 20, 40, 60, 200}) TexQuad(prev, x, 0, 32);  // 200: leaves the view
    TexQuad(cur, 4, 0, 32);
    TexQuad(cur, 12, 0, 32);  // inserted: drawn before 24, nearest old tile is 20
    for (int x : {24, 44, 64}) TexQuad(cur, x, 0, 32);
    const Match m = MatchPrimitives(prev, cur, 48);
    assert(m.prims == 5 && m.matched == 4);
    assert(m.prev_of[1] == 2 && m.prev_of[3] == -1 && m.prev_of[5] == 14 && m.prev_of[7] == 26 && m.prev_of[9] == 38);
    LerpCommands(prev, cur, m, 0.5, 48, out);
    assert(out.size() == cur.size());
    assert(X0(out, 2) == 2 && X0(out, 14) == 12 && X0(out, 26) == 22 && X0(out, 38) == 42 && X0(out, 50) == 62);
    for (size_t i = 12; i < 24; ++i) assert(out[i] == cur[i]);  // the new tile is untouched, not dropped
    std::printf("  inserted/removed tiles ok\n");
}

// Textured quad spanning xl..xr, y 0..16, own UV origin u (its own key).
static void Strip(std::vector<uint32_t>& c, int xl, int xr, uint32_t u) {
    Tag(c);
    Packet(c, {0x2C808080, XY(xl, 0), u, XY(xr, 0), u + 16, XY(xl, 16), u | 16 << 8, XY(xr, 16), (u + 16) | 16 << 8});
}
static int32_t Xv(const std::vector<uint32_t>& c, size_t packet_word, int k) {  // vertex k of a 2Ch quad
    return int16_t(c[packet_word + 2 + 2 * k] & 0xFFFF);
}

// Root cause of the 120 fps jitter: vertices lerped and rounded one by one, per primitive. A mesh whose
// vertices move 2 and 3 px (perspective) had edges growing and shrinking inside one game frame (texture
// swimming), and a shared vertex of a wrong same-key partner or of an unpaired neighbour landed in two
// places (seams opening and closing). Every vertex position of cur must now land in one place, move
// monotonically, and every edge must change monotonically from its old length to its new one.
static void TestMeshMovesAsOne() {
    // Four strips sharing their edges at x = 26, 42, 58; the left column moves 2 px, the others 3 px; the
    // last strip is new (no partner, as entering the view).
    std::vector<uint32_t> prev, cur, out[5];
    Strip(prev, 8, 23, 0), Strip(prev, 23, 39, 64), Strip(prev, 39, 55, 128);
    Strip(cur, 10, 26, 0), Strip(cur, 26, 42, 64), Strip(cur, 42, 58, 128), Strip(cur, 58, 74, 192);
    const Match m = MatchPrimitives(prev, cur, 48);
    assert(m.matched == 3 && m.prev_of[7] == -1);
    const double ts[5] = {0, 0.25, 0.5, 0.75, 1};
    auto check = [&](const Match& mm) {
        for (int j = 0; j < 5; ++j) LerpCommands(prev, cur, mm, ts[j], 48, out[j]);
        assert(out[4] == cur);
        for (size_t p : {size_t(2), size_t(14), size_t(26), size_t(38)})
            for (int j = 0; j < 5; ++j) {
                if (p > 2) assert(Xv(out[j], p, 0) == Xv(out[j], p - 12, 1));  // shared vertices land together
                if (j == 0 && p < 38) assert(Xv(out[0], p, 0) == Xv(prev, p, 0) && Xv(out[0], p, 1) == Xv(prev, p, 1));
                if (j == 4) continue;
                for (int k = 0; k < 2; ++k) assert(Xv(out[j + 1], p, k) >= Xv(out[j], p, k));  // no vertex steps back
                assert(Xv(out[j + 1], p, 1) - Xv(out[j + 1], p, 0) >= Xv(out[j], p, 1) - Xv(out[j], p, 0));  // width 15 -> 16 once
            }
    };
    check(m);
    // Lerped and rounded one by one, x = 10 (2 px) and x = 26 (3 px) snapped at different pictures: the
    // left strip was 15, 15, 16, 15, 16 px wide.
    LerpCommands(prev, cur, m, 0.5, 48, out[2]);
    assert(Xv(out[2], 2, 0) == 8 && Xv(out[2], 2, 1) == 24 && Xv(out[2], 38, 1) == 72);  // the new strip follows
    // The middle strip paired with the right one's old place (a wrong same-key partner in a real scene):
    // it is carried by its neighbours instead of tearing the mesh.
    Match wrong = m;
    wrong.prev_of[3] = wrong.prev_of[5];
    check(wrong);
    std::printf("  mesh moves as one ok\n");
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
    // The upload is captured with its data words (the game reuses texture slots within a frame).
    bool upload = false;
    for (size_t k = 0; k < seg.size(); k += 1 + seg[k]) upload |= seg[k] == 7 && seg[k + 1] >> 24 == 0xA0;
    assert(upload);
    Replay(replay, seg);
    assert(replay.GetVram() == plain.GetVram());

    // The interpolator: two flips give a pair; t=1 shows the latest frame, t=0 the previous.
    FrameInterpolator fi;
    Ps1Gpu g;
    std::vector<uint32_t> l2;
    g.SetCommandLog(&l2);
    Widescreen2D gte(1, 1);  // the triangle is 3D: its vertices came from the GTE
    for (int k = 1; k <= 3; ++k)
        for (uint32_t xy : {XY(10 + 8 * k, 10), XY(40 + 8 * k, 10), XY(10 + 8 * k, 40)}) gte.NoteProjected(xy);
    g.SetProjectionTracker(&gte);
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

// Widescreen: a squeezed 2D primitive is recorded in the log, so a replay without the classifier squeezes
// it the same way (interpolated pictures keep 2D at 4:3).
static void TestWidescreenSqueezeReplays() {
    Widescreen2D ws(3, 4);
    ws.NoteProjected(XY(10, 10));
    ws.NoteProjected(XY(60, 10));
    Ps1Gpu live;
    live.SetWidescreen2D(&ws);
    std::vector<uint32_t> log;
    live.SetCommandLog(&log);
    std::vector<uint32_t> seg;
    for (uint32_t w : {0xE3000000u, 0xE4000000u | 319 | 239 << 10, 0xE5000000u}) live.WriteGP0(w);
    const std::vector<uint16_t> start = live.GetVram();
    const size_t from = log.size();
    for (uint32_t w : {0x20FF0000u, XY(10, 10), XY(60, 10), XY(10, 60)}) live.WriteGP0(w);  // 3D (projected)
    live.WriteGP0(0xE1000000u);                                                              // HUD follows
    for (uint32_t w : {0x2000FF00u, XY(200, 100), XY(300, 100), XY(200, 200)}) live.WriteGP0(w);  // 2D: squeezed
    seg.assign(log.begin() + std::ptrdiff_t(from), log.end());
    size_t recs = 0;
    for (size_t k = 0; k < seg.size(); k += 1 + seg[k]) recs += seg[k] == 3 && seg[k + 1] == 0x00AB0000u;
    assert(recs == 1);
    assert(live.GetVram()[150 * 1024 + 298] == 0);  // squeezed toward the centre (160): not drawn there
    Ps1Gpu replay;
    replay.SetVram(start);
    Replay(replay, seg);
    assert(replay.GetVram() == live.GetVram());
    std::printf("  widescreen squeeze replay ok\n");
}

// 2D (no GTE vertex: sprites, portraits, text) is never moved: with a static 2D quad, a 2D quad that moves
// and changes colour between frames, and a static 3D triangle (so the pair is interpolated), every
// in-between picture is byte-identical to the newer real frame.
static void Test2DIsDrawnAsInCur() {
    FrameInterpolator fi;
    Ps1Gpu g;
    std::vector<uint32_t> log;
    g.SetCommandLog(&log);
    Widescreen2D gte(1, 1);
    for (uint32_t xy : {XY(200, 20), XY(260, 20), XY(200, 80)}) gte.NoteProjected(xy);
    g.SetProjectionTracker(&gte);
    uint64_t frame = 0;
    for (int k = 1; k <= 3; ++k) {
        const uint32_t y = (k & 1) ? 240 : 0;
        for (uint32_t w : {0xE3000000u | y << 10, 0xE4000000u | 319 | (y + 239) << 10, 0xE5000000u | y << 11}) g.WriteGP0(w);
        g.WriteGP0(0x02000000); g.WriteGP0(XY(0, y)); g.WriteGP0(XY(320, 240));
        for (uint32_t w : {0x20808080u, XY(200, 20), XY(260, 20), XY(200, 80)}) g.WriteGP0(w);  // 3D, static
        for (uint32_t w : {0x280000FFu, XY(10, 100), XY(40, 100), XY(10, 130), XY(40, 130)}) g.WriteGP0(w);  // 2D, static
        const int32_t x = 60 + 10 * k;  // 2D, moves 10 px per frame (same key in the last two)
        for (uint32_t w : {k == 1 ? 0x2800FF00u : 0x28FF0000u, XY(x, 150), XY(x + 20, 150), XY(x, 170), XY(x + 20, 170)})
            g.WriteGP0(w);
        g.WriteGP1(0x05000000 | y << 10);
        frame += 2;
        assert(fi.Consume(log, g, frame));
    }
    assert(fi.Active(frame) && fi.PairOk() && fi.LastMatch().prims == 1);
    const std::vector<uint16_t> real = g.GetVram();
    for (double t : {0.25, 0.5, 0.75}) {
        const std::vector<uint16_t>* vram = nullptr;
        GpuDisplayArea d;
        fi.Render(t, vram, d);
        assert(d.y == 240);
        for (uint32_t row = 240; row < 480; ++row)
            for (uint32_t col = 0; col < 320; ++col) assert((*vram)[row * 1024 + col] == real[row * 1024 + col]);
    }
    std::printf("  2D drawn as in cur ok\n");
}

// Still scenery is never moved by what touches it or by GTE rounding: two floor quads share a vertex with a
// moving shadow fan (its rim lands on it) and a third floor quad jitters by a pixel; in-between, the floor
// is exactly cur and the fan slides.
static void TestStillFloorStaysPut() {
    std::vector<uint32_t> prev, cur, out;
    auto floor = [](std::vector<uint32_t>& c, int dy) {
        Tag(c), Packet(c, {0x28404040, XY(0, 0), XY(50, 0), XY(0, 50), XY(50, 50)});
        Tag(c), Packet(c, {0x28404040, XY(50, 0), XY(100, 0), XY(50, 50), XY(100, 50)});
        Tag(c), Packet(c, {0x28404040, XY(0, 60), XY(50, 60), XY(0, 110 + dy), XY(50, 110)});  // 1 px of GTE noise
    };
    auto fan = [](std::vector<uint32_t>& c, int dx) {
        Tag(c), Packet(c, {0x20101010, XY(50 + dx, 50), XY(60 + dx, 40), XY(70 + dx, 50)});
        Tag(c), Packet(c, {0x20101010, XY(50 + dx, 50), XY(70 + dx, 50), XY(60 + dx, 60)});
        Tag(c), Packet(c, {0x20101010, XY(50 + dx, 50), XY(60 + dx, 60), XY(45 + dx, 58)});
    };
    floor(prev, 1), fan(prev, -8);
    floor(cur, 0), fan(cur, 0);
    const Match m = MatchPrimitives(prev, cur, 48);
    assert(m.prims == 6 && m.matched == 6);
    LerpCommands(prev, cur, m, 0.25, 48, out);
    const size_t fan_at = cur.size() - 21;  // the first fan packet
    for (size_t i = 0; i < fan_at; ++i) assert(out[i] == cur[i]);
    assert(X0(out, fan_at + 2) == 44);
    std::printf("  still floor stays put ok\n");
}

static std::vector<uint16_t> Picture(const std::vector<uint16_t>& vram, const GpuDisplayArea& d) {
    std::vector<uint16_t> p;
    for (uint32_t row = 0; row < 240; ++row)
        for (uint32_t col = 0; col < 320; ++col) p.push_back(vram[((d.y + row) & 511) * 1024 + ((d.x + col) & 1023)]);
    return p;
}

// A still scene gives identical pictures at every t, and no picture is older than the one before it. The
// game blends a quad over its 3D scene, copies part of the previous frame (the other buffer) into the new
// one, and starts drawing the next frame there before the host sees the flip: a replay over the VRAM after
// the emulated frame copied that instead. Then a 2D-only frame (no pair to interpolate) is shown at once.
static void TestStillSceneIsExactAndInOrder() {
    FrameInterpolator fi;
    Ps1Gpu g;
    std::vector<uint32_t> log;
    g.SetCommandLog(&log);
    Widescreen2D gte(1, 1);
    for (uint32_t xy : {XY(20, 20), XY(120, 20), XY(20, 120)}) gte.NoteProjected(xy);
    g.SetProjectionTracker(&gte);
    auto area = [&g](uint32_t y) {
        for (uint32_t w : {0xE3000000u | y << 10, 0xE4000000u | 319 | (y + 239) << 10, 0xE5000000u | y << 11}) g.WriteGP0(w);
    };
    uint64_t frame = 0;
    std::vector<uint16_t> shown;  // the latest frame's picture
    for (int k = 1; k <= 4; ++k) {
        const uint32_t y = (k & 1) ? 240 : 0;
        area(y);
        g.WriteGP0(0x02102030); g.WriteGP0(XY(0, y)); g.WriteGP0(XY(320, 240));
        if (k < 4)
            for (uint32_t w : {0x20406080u, XY(20, 20), XY(120, 20), XY(20, 120)}) g.WriteGP0(w);  // 3D, still
        g.WriteGP0(0xE1000000u | 1u << 5);                                                        // B + F
        for (uint32_t w : {0x62303030u, XY(10, 10), XY(200, 150)}) g.WriteGP0(w);                // blended
        // An echo of the previous frame (the other buffer), as reflections and trails do.
        for (uint32_t w : {0x80000000u, XY(20, 240 - y + 20), XY(180, y + 100), XY(100, 100)}) g.WriteGP0(w);
        g.WriteGP1(0x05000000 | y << 10);
        shown = Picture(g.GetVram(), {0, uint16_t(y)});
        area(240 - y);  // the next frame starts in the other buffer before the host drains the log
        g.WriteGP0(0x02FFFFFF); g.WriteGP0(XY(0, 240 - y)); g.WriteGP0(XY(320, 240));
        frame += 2;
        assert(fi.Consume(log, g, frame));
        const std::vector<uint16_t>* vram = nullptr;
        GpuDisplayArea d;
        if (k == 3) {  // still pair: every t is the latest frame
            assert(fi.PairOk());
            for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                fi.Render(t, vram, d);
                assert(Picture(*vram, d) == shown);
            }
        }
        if (k == 4) {  // 2D only: not interpolable, shown at once, never the older frame
            assert(!fi.PairOk());
            fi.Render(0.0, vram, d);
            assert(d.y == y && Picture(*vram, d) == shown);
        }
    }
    std::printf("  still scene exact, in order ok\n");
}

int main() {
    std::printf("frame_interp tests\n");
    TestMatchAndLerp();
    TestUntexturedAndRects();
    TestInsertedAndRemovedTiles();
    TestMeshMovesAsOne();
    TestCaptureIsInvisibleAndComplete();
    TestWidescreenSqueezeReplays();
    Test2DIsDrawnAsInCur();
    TestStillSceneIsExactAndInOrder();
    TestStillFloorStaysPut();
    std::printf("PASS\n");
    return 0;
}
