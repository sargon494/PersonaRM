#pragma once
// Display-only frame interpolation (docs/research/FRAME_INTERPOLATION.md, local): the GP0 packets of
// two consecutive game frames (Ps1Gpu::SetCommandLog) are matched primitive by primitive and the
// latest frame is re-rasterized with each matched vertex moved part of the way back to where it was
// in the previous one. Emulation never sees any of it: replays run on a scratch Ps1Gpu.

#include <cstdint>
#include <vector>

#include "rendering/ps1_gpu.h"

namespace personarm::interp {

/// Per packet of `cur` (in order), the word offset of its packet in `prev`, or -1. Only primitives
/// with fixed vertex slots take part (polygons, single lines, rectangles). Key: opcode plus the
/// texture words (UV / CLUT / texpage, rectangle size) for textured primitives, plus the first colour
/// for untextured ones; among the unused `prev` packets with the same key, the one whose first vertex
/// is nearest pairs (repeated tiles and glyphs, depth-sorted OTs that reorder primitives).
/// shortcut: greedy in cur's draw order over at most 256 candidates per key; a real matcher would
/// solve the assignment and use the camera motion as a prior.
struct Match {
    std::vector<int32_t> prev_of;  // per packet of cur
    uint32_t prims = 0, matched = 0;
};
Match MatchPrimitives(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur);

/// `cur` with every matched primitive's vertices at prev + (cur - prev) * t (rounded), unless one of
/// them moved more than `max_jump` pixels on an axis (teleport / wrong pair: drawn as in cur).
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
