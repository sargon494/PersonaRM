#pragma once
// Display-only frame interpolation (docs/research/FRAME_INTERPOLATION.md, local): the GP0 packets of
// two consecutive game frames (Ps1Gpu::SetCommandLog) are matched primitive by primitive and the
// latest frame is re-rasterized with each matched vertex moved part of the way back to where it was
// in the previous one. Emulation never sees any of it: replays run on a scratch Ps1Gpu.

#include <cstdint>
#include <vector>

#include "rendering/ps1_gpu.h"

namespace personarm::interp {

/// Per packet of `cur` (in order), the word offset of its packet in `prev`, or -1 (drawn as in cur).
/// Only 3D polygons take part (tagged [1, Ps1Gpu::kLog3D] in the log); everything else is drawn as in
/// cur. Key: opcode
/// plus the texture words (UV / CLUT / texpage, rectangle size) for textured primitives, plus the first
/// colour for untextured ones. Among same-key packets whose every vertex moved at most `max_jump` px
/// per axis, pairs are taken cheapest total vertex travel first (a global greedy assignment), so
/// primitives inserted or removed anywhere in the list (tiles entering the view, depth-sorted OTs that
/// reorder) cannot shift the pairing of the others.
/// shortcut: greedy, not an optimal assignment; same-key candidates are all scanned (quadratic in the
/// size of one key group, fine for a few hundred identical tiles or glyphs).
struct Match {
    std::vector<int32_t> prev_of;  // per packet of cur
    uint32_t prims = 0, matched = 0;
};
Match MatchPrimitives(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur, int32_t max_jump);

/// `cur` with its vertices moved back towards prev by (cur - prev) * (1 - t), unless a pair moved more
/// than `max_jump` pixels on an axis (teleport: drawn as in cur). Lines and rectangles are lerped on their
/// own. Polygons move per vertex position, as meshes: one motion per shared vertex (wrong same-key
/// partners outvoted), unpaired polygons carried by the vertices they share, each mesh sliding as one
/// and switching its small shape changes once at t = 0.5 (no edge grows and shrinks between two frames).
void LerpCommands(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur, const Match& m, double t,
                  int32_t max_jump, std::vector<uint32_t>& out);

/// Feed packets in SetCommandLog format ([n, words...], no flip markers) to a GPU.
void Replay(Ps1Gpu& gpu, const std::vector<uint32_t>& cmds);

class FrameInterpolator {
public:
    struct Frame {
        std::vector<uint32_t> cmds;  // packets drawn since the previous flip
        std::vector<uint16_t> vram;  // VRAM after the emulated frame that flipped to it
        GpuDisplayArea disp;
        uint64_t flip_frame = 0;     // engine frame count after the flip
    };
    /// Drain `log` after each emulated frame (gpu = the emulated GPU, frame = engine frame count).
    /// Returns true when a flip completed a new game frame.
    bool Consume(std::vector<uint32_t>& log, const Ps1Gpu& gpu, uint64_t frame);
    /// Two game frames are known and the latest flip is recent (else the game is not double-buffering).
    bool Active(uint64_t frame) const { return m_count >= 2 && frame - m_cur.flip_frame <= kStaleFrames; }
    /// Picture at fraction t of the way from the previous game frame to the latest (one game frame of
    /// latency): t <= 0 the previous frame as emulated, t >= 1 the latest, else a replay of the latest
    /// with lerped vertices, or the previous frame when the pair is not interpolable (scene cut, FMV,
    /// 24-bit display, too few matches). `out` = VRAM to read the display area `disp` from.
    void Render(double t, const std::vector<uint16_t>*& out, GpuDisplayArea& disp);
    /// t for a host tick at fractional engine time `time` (frames): elapsed since the latest flip over
    /// the interval between the last two flips.
    double Fraction(double time) const;
    /// Pair usable for interpolation (computed once per new game frame).
    bool PairOk() const { return m_pair_ok; }
    const Match& LastMatch() const { return m_match; }
    /// The GPU the replays run on (a host GPU renderer listens to it while drawing the replay at s x).
    Ps1Gpu& Scratch() { return m_scratch; }

    static constexpr uint64_t kStaleFrames = 8;
    static constexpr int32_t kMaxJump = 48;      // px per game frame
    static constexpr double kMinMatched = 0.5;   // fraction of cur primitives paired

private:
    Frame m_prev, m_cur;
    std::vector<uint32_t> m_pending, m_lerped;
    uint32_t m_count = 0;
    Match m_match;
    bool m_pair_ok = false;
    Ps1Gpu m_scratch;
};

} // namespace personarm::interp
