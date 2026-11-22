#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/logger.h"

namespace personarm {

/// Widescreen 2D/3D split (emulator-side, not hardware; docs/WIDESCREEN.md). With the GTE widescreen hack
/// the 320-px frame is shown at W:H, so 2D drawn on a 3D scene (dialogue boxes, text, portraits) would be
/// stretched. The GTE notes every screen XY that RTPS/RTPT produce; the GPU asks whether a primitive's
/// vertices are among them (as PGXP / DuckStation match GTE output to GP0 vertices). A primitive with no
/// GTE vertex is 2D, and the order of the GP0 stream tells what kind:
///  - background: 2D before the frame's first 3D primitive (the battle panorama wall) stays full width;
///  - screen-anchored overlay: 2D (not a scene sprite) touching the left or right edge of the drawing area
///    or covering nearly all of it (fades, the corridor's darkening vignette, letterbox bars), and the
///    untextured 2D drawn right after it: left as drawn, so it spans the whole wide picture;
///  - scene sprite: textured 2D after a 3D primitive (field and battle characters: flat quads placed at a
///    projected point). The pieces of one character (one run, each near the ones before; another palette over another shadow starts the next) are squeezed by
///    num/den around one anchor: the centre of the character's shadow (a GTE triangle fan drawn before it)
///    when one is near, else the first piece's centre. The character keeps its shape and its place;
///  - HUD: any other 2D (after a non-drawing GP0 command, a line or an untextured primitive) is squeezed
///    toward the screen centre, so the 4:3 layout sits in the middle of the wide picture.
/// Textured 2D keeps its scene/HUD decision per texture (texpage + CLUT) unless the stream disagrees for
/// kStickyFrames frames, so a portrait or a font cannot flip between frames. Safety net: a large
/// semi-transparent 2D layer that is neither centred nor screen-anchored cannot be placed; the scene then
/// falls back to 4:3 (GTE not widened, nothing squeezed, host pillarbox) until none has been seen for
/// kFallbackFrames frames. num/den = the GTE's 4H/3W, so squeezed 2D keeps 4:3 proportions.
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
        ++m_frames;
        if (++m_gen == 0) {  // generation counter wrapped (~18 min): forget everything
            std::fill(m_seen.begin(), m_seen.end(), uint16_t(0));
            m_gen = 1, m_any = false, m_fallback = false, m_fans.clear(), m_sticky.clear();
        }
    }
    /// GP0 vertex word (signed 11-bit X / Y, drawing offset not applied) produced by the GTE in the last
    /// kKeepFrames frames. The game draws its ordering table a frame or two after projecting.
    bool Projected(uint32_t xy) const {
        const uint16_t e = m_seen[Key(xy)];
        return e != 0 && uint16_t(m_gen - e) <= kKeepFrames;
    }
    /// A 3D scene is up (the GTE projected in the last 30 frames). 2D-only screens (title, menus without a
    /// scene) are not classified.
    bool Scene() const { return m_any && uint16_t(m_gen - m_last_projection) < 30; }
    /// Safety net active: the GTE does not widen and the host presents 4:3.
    bool Fallback() const { return m_fallback && uint16_t(m_gen - m_fallback_gen) < kFallbackFrames; }
    /// The host presents W:H and 2D is squeezed.
    bool Wide() const { return Scene() && !Fallback(); }

    /// X around centre cx, squeezed by num/den (symmetric truncation).
    int32_t Squeeze(int32_t x, int32_t cx) const { return cx + (x - cx) * m_num / m_den; }
    int32_t Num() const { return m_num; }
    int32_t Den() const { return m_den; }

    /// Any GP0 command that is not a polygon, line or rectangle: what follows is HUD 2D. A drawing area or
    /// offset (E3h-E5h, PutDrawEnv) starts a new frame: 2D before its first 3D is background.
    void OtherCommand(bool draw_env) {
        m_scene = m_group = m_overlay = false;
        if (draw_env) m_frame_3d = false;
    }

    /// Squeeze a primitive's X coordinates (VRAM space, drawing offset applied; y likewise) if it is
    /// scene-sprite or HUD 2D; [x1, x2] is the drawing area. xy = the GP0 vertex words (3D needs 2 or more
    /// of them projected), null for rectangles, which are always 2D. tex = texpage | CLUT << 16 of a
    /// textured primitive. Returns the centre it squeezed around, or kNone (3D, background, overlay,
    /// fallback).
    int32_t Squeeze2D(const uint32_t* xy, int32_t* x, const int32_t* y, int n, int32_t x1, int32_t x2,
                      bool textured, bool semi, uint32_t tex) {
        int32_t lo = x[0], hi = x[0], ylo = y[0], hits = 0;
        for (int i = 0; i < n; ++i) {
            hits += xy && Projected(xy[i]);
            lo = std::min(lo, x[i]), hi = std::max(hi, x[i]), ylo = std::min(ylo, y[i]);
        }
        if (hits >= 2) {  // one match can be chance (a glyph on a stale projected point); GTE polygons match fully
            m_frame_3d = m_scene = true, m_group = m_overlay = false;
            NoteFan(xy, x[0], y[0], n);
            return kNone;
        }
        m_prev_tri = kNoWord;
        if (!m_frame_3d) return kNone;
        if (!textured) m_scene = false;
        const bool scene = textured && Sticky(tex, m_scene);
        const int32_t w = x2 - x1 + 1;
        int32_t cx = (x1 + x2 + 1) / 2;
        if (scene) {
            m_overlay = false;
            // A piece touching the group with another palette pair (two 16-colour CLUTs side by side, hence
            // >> 17) over another shadow is the next character (a party bunched up while running); without
            // its own shadow it is part of this one (a corpse's blood pool).
            int32_t fan = kNone;
            const bool apart = lo > m_group_hi + kNear || hi < m_group_lo - kNear;
            if (m_group && !apart && tex >> 17 != m_group_clut) fan = FindFan(lo, hi, ylo, kNone);
            if (!m_group || apart || (fan != kNone && fan != m_anchor)) {
                m_group = true, m_group_lo = lo, m_group_hi = hi, m_group_clut = tex >> 17;
                m_anchor = fan != kNone ? fan : FindFan(lo, hi, ylo, (lo + hi + 1) / 2);
            }
            m_group_lo = std::min(m_group_lo, lo), m_group_hi = std::max(m_group_hi, hi);
            cx = m_anchor;
        } else {
            m_group = false;
            // A line starts an overlay only when it spans the whole width: the soft edge of a wipe (1-px
            // gouraud lines, Kismet floor transition) crosses the screen edge but belongs with the wipe.
            const bool line = xy && n == 2;
            const bool anchored = line ? lo <= x1 && hi >= x2
                                       : lo <= x1 + kEdge || hi >= x2 - kEdge || (hi - lo) * 16 >= w * 15;
            // Untextured 2D right after an overlay piece is part of it (the vignette's inner ring, the
            // wipe's edge lines between its black rows).
            if (anchored || (m_overlay && !textured)) {
                m_overlay = true;
                return kNone;
            }
            m_overlay = false;
            if (semi && (hi - lo) * 4 >= w * 3 && std::abs(lo + hi - x1 - x2) * 8 > w) Uncertain(lo, hi, ylo);
        }
        if (Fallback()) return kNone;
        for (int i = 0; i < n; ++i) x[i] = Squeeze(x[i], cx);
        return cx;
    }

    static constexpr int32_t kNone = INT32_MIN;
    static constexpr uint16_t kKeepFrames = 2;
    static constexpr int32_t kEdge = 4;           // px from the drawing area edge that still counts as touching it
    static constexpr int32_t kNear = 16;          // px between two pieces of one character (arm, head, legs)
    static constexpr uint16_t kStickyFrames = 8;  // frames a texture's scene/HUD context must disagree to flip
    static constexpr uint16_t kFallbackFrames = 120;

private:
    struct Fan { int32_t x, y; uint16_t gen; };
    struct StickyEntry { bool scene; uint16_t seen, disagree_gen, disagree; };
    static constexpr uint32_t kNoWord = 0xFFFFFFFFu;

    static uint32_t Key(uint32_t xy) { return (xy & 0x7FF) | ((xy >> 16) & 0x7FF) << 11; }

    /// A 3D triangle sharing its first vertex with the previous one is a fan: a character's round shadow,
    /// whose centre is the projected point the character's sprites are placed at.
    void NoteFan(const uint32_t* xy, int32_t fx, int32_t fy, int n) {
        if (n != 3) {
            m_prev_tri = kNoWord;
            return;
        }
        const bool fan = xy[0] == m_prev_tri;
        if (fan && !m_in_fan) {
            if (m_fans.size() >= 64) m_fans.erase(m_fans.begin());
            m_fans.push_back({fx, fy, m_gen});
        }
        m_in_fan = fan;
        m_prev_tri = xy[0];
    }
    /// The newest recent shadow fan under a piece spanning lo..hi from top ylo, else fallback.
    int32_t FindFan(int32_t lo, int32_t hi, int32_t ylo, int32_t fallback) const {
        for (auto f = m_fans.rbegin(); f != m_fans.rend(); ++f) {
            if (uint16_t(m_gen - f->gen) > kKeepFrames) break;
            if (f->x >= lo - kNear && f->x <= hi + kNear && f->y >= ylo && f->y <= ylo + 160) return f->x;
        }
        return fallback;
    }
    /// The scene/HUD decision for a texture: the stream's context unless it disagreed with the last one
    /// for kStickyFrames frames in a row (a texture unused for a second starts over).
    bool Sticky(uint32_t tex, bool context) {
        auto it = m_sticky.find(tex);
        if (it == m_sticky.end() || uint16_t(m_gen - it->second.seen) > 60) {
            m_sticky[tex] = {context, m_gen, 0, 0};
            return context;
        }
        StickyEntry& e = it->second;
        e.seen = m_gen;
        if (context == e.scene) {
            e.disagree = 0;
        } else if (e.disagree_gen != m_gen) {
            e.disagree_gen = m_gen;
            if (++e.disagree >= kStickyFrames) e.scene = context, e.disagree = 0;
        }
        return e.scene;
    }
    void Uncertain(int32_t lo, int32_t hi, int32_t y) {
        if (!Fallback())
            PRM_LOG_WARN("WIDESCREEN", "frame " + std::to_string(m_frames) + ": 2D layer x " + std::to_string(lo) +
                                           ".." + std::to_string(hi) + " y " + std::to_string(y) +
                                           " is neither centred nor screen-anchored; showing 4:3");
        m_fallback = true, m_fallback_gen = m_gen;
    }

    int32_t m_num, m_den;
    std::vector<uint16_t> m_seen;  // 2048 x 2048 screen positions -> generation last projected (0 = never)
    uint16_t m_gen = 1, m_last_projection = 0, m_fallback_gen = 0;
    uint64_t m_frames = 0;
    bool m_any = false, m_fallback = false;
    bool m_frame_3d = false;  // a 3D primitive was drawn since the last drawing environment
    bool m_scene = false;     // the last drawing command was 3D or a textured 2D sprite following one
    bool m_group = false;     // inside a scene sprite group (one character)
    bool m_overlay = false;   // inside a screen-anchored overlay run
    int32_t m_anchor = 0, m_group_lo = 0, m_group_hi = 0;
    uint32_t m_group_clut = 0;
    uint32_t m_prev_tri = kNoWord;
    bool m_in_fan = false;
    std::vector<Fan> m_fans;
    std::unordered_map<uint32_t, StickyEntry> m_sticky;
};

} // namespace personarm
