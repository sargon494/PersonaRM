#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace personarm {

/// Widescreen 2D/3D split (emulator-side, not hardware; docs/WIDESCREEN.md). With the GTE widescreen hack
/// the 320-px frame is shown at W:H, so 2D drawn on a 3D scene (dialogue boxes, text, portraits) would be
/// stretched. The GTE notes every screen XY that RTPS/RTPT produce; the GPU asks whether a primitive's
/// vertices are among them (as PGXP / DuckStation match GTE output to GP0 vertices). A primitive with no
/// GTE vertex is 2D, and the order of the GP0 stream tells what kind:
///  - background: 2D before the frame's first 3D primitive (the battle panorama wall) stays full width;
///  - scene sprite: textured 2D right after a 3D primitive in the ordering table (field and battle
///    characters: flat quads placed at a projected point) is squeezed by num/den around its own centre
///    (the first primitive of a run of touching ones, so a character made of several quads stays whole);
///  - HUD: any other 2D (after a non-drawing GP0 command, a line or an untextured primitive) is squeezed
///    toward the screen centre, so the 4:3 layout sits in the middle of the wide picture.
/// 2D spanning the whole drawing width (full-screen fades) is left alone. num/den = the GTE's 4H/3W, so
/// squeezed 2D keeps 4:3 proportions after the host's horizontal stretch.
class Widescreen2D {
public:
    Widescreen2D(int32_t num, int32_t den) : m_num(num), m_den(den > 0 ? den : 1), m_seen(1u << 22, 0) {}

    /// A GTE screen XY word (SXY2 after RTPS/RTPT: X in bits 0-15, Y in bits 16-31).
    void NoteProjected(uint32_t sxy) {
        m_seen[Key(sxy)] = m_gen;
        m_last_projection = m_gen;
        m_any = true;
    }
    /// Once per emulated frame (VBlank).
    void NextFrame() {
        if (++m_gen == 0) {  // generation counter wrapped (~18 min): forget everything
            std::fill(m_seen.begin(), m_seen.end(), uint16_t(0));
            m_gen = 1, m_any = false;
        }
    }
    /// GP0 vertex word (signed 11-bit X / Y, drawing offset not applied) produced by the GTE in the last
    /// kKeepFrames frames. The game draws its ordering table a frame or two after projecting.
    bool Projected(uint32_t xy) const {
        const uint16_t e = m_seen[Key(xy)];
        return e != 0 && uint16_t(m_gen - e) <= kKeepFrames;
    }
    /// A 3D scene is up (the GTE projected in the last 30 frames): the host presents W:H and 2D is
    /// squeezed. 2D-only screens (title, menus without a scene) keep 4:3 and are left alone.
    bool Wide() const { return m_any && uint16_t(m_gen - m_last_projection) < 30; }

    /// X around centre cx, squeezed by num/den (symmetric truncation).
    int32_t Squeeze(int32_t x, int32_t cx) const { return cx + (x - cx) * m_num / m_den; }

    /// Any GP0 command that is not a polygon, line or rectangle: what follows is HUD 2D. A drawing area or
    /// offset (E3h-E5h, PutDrawEnv) starts a new frame: 2D before its first 3D is background.
    void OtherCommand(bool draw_env) {
        m_scene = false;
        if (draw_env) m_frame_3d = false;
    }

    /// Squeeze a primitive's X coordinates (VRAM space, drawing offset applied) if it is scene-sprite or
    /// HUD 2D; [x1, x2] is the drawing area. xy = the GP0 vertex words (3D needs 2 or more of them
    /// projected), null for rectangles, which are always 2D. Returns the centre it squeezed around, or
    /// kNone (3D, background, full width).
    int32_t Squeeze2D(const uint32_t* xy, int32_t* x, int n, int32_t x1, int32_t x2, bool textured) {
        int32_t lo = x[0], hi = x[0], hits = 0;
        for (int i = 0; i < n; ++i) {
            hits += xy && Projected(xy[i]);
            lo = std::min(lo, x[i]), hi = std::max(hi, x[i]);
        }
        if (hits >= 2) {  // one match can be chance (a glyph on a stale projected point); GTE polygons match fully
            m_frame_3d = m_scene = true, m_run = false;
            return kNone;
        }
        if (!m_frame_3d || (lo <= x1 && hi >= x2)) return kNone;
        if (!textured) m_scene = false;
        int32_t cx = (x1 + x2 + 1) / 2;
        if (m_scene) {
            if (!m_run || lo > m_run_hi + kTouch || hi < m_run_lo - kTouch)
                m_run = true, m_anchor = (lo + hi + 1) / 2, m_run_lo = lo, m_run_hi = hi;
            m_run_lo = std::min(m_run_lo, lo), m_run_hi = std::max(m_run_hi, hi);
            cx = m_anchor;
        }
        for (int i = 0; i < n; ++i) x[i] = Squeeze(x[i], cx);
        return cx;
    }

    static constexpr int32_t kNone = INT32_MIN;
    static constexpr uint16_t kKeepFrames = 2;
    static constexpr int32_t kTouch = 4;  // px between two 2D sprites of one run (one character's parts)

private:
    static uint32_t Key(uint32_t xy) { return (xy & 0x7FF) | ((xy >> 16) & 0x7FF) << 11; }

    int32_t m_num, m_den;
    std::vector<uint16_t> m_seen;  // 2048 x 2048 screen positions -> generation last projected (0 = never)
    uint16_t m_gen = 1, m_last_projection = 0;
    bool m_any = false;
    bool m_frame_3d = false;  // a 3D primitive was drawn since the last drawing environment
    bool m_scene = false;     // the last drawing command was 3D or a textured 2D sprite following one
    bool m_run = false;
    int32_t m_anchor = 0, m_run_lo = 0, m_run_hi = 0;
};

} // namespace personarm
