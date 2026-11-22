#include "rendering/frame_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace personarm::interp {

namespace {

// Word indices of the vertex (xy) words of a GP0 packet (psx-spx "GPU Render Polygon/Line/Rectangle
// Commands"); 0 = not a fixed-slot primitive. Polylines (48h-4Fh, 58h-5Fh) stream their vertices, skip.
// `words` = the packet length the command needs.
int VertexSlots(uint8_t cmd, int slot[4], uint32_t& words) {
    if (cmd >= 0x20 && cmd <= 0x3F) {
        const bool gouraud = cmd & 0x10, textured = cmd & 0x04;
        const int n = (cmd & 0x08) ? 4 : 3;
        int idx = 1;
        for (int i = 0; i < n; ++i) {
            if (gouraud && i > 0) ++idx;
            slot[i] = idx++;
            if (textured) ++idx;
        }
        words = uint32_t(idx);
        return n;
    }
    if ((cmd >= 0x40 && cmd <= 0x47) || (cmd >= 0x50 && cmd <= 0x57)) {
        slot[0] = 1;
        slot[1] = (cmd & 0x10) ? 3 : 2;
        words = uint32_t(slot[1] + 1);
        return 2;
    }
    if (cmd >= 0x60 && cmd <= 0x7F) {
        slot[0] = 1;
        words = 2 + ((cmd & 0x04) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
        return 1;
    }
    return 0;
}

// Vertex slots of the log entry at e ([n, words...]); 0 unless it is a whole fixed-slot primitive
// (a polyline continuation [1, w] can look like any opcode).
int Slots(const uint32_t* e, int slot[4]) {
    uint32_t words = 0;
    const int nv = e[0] ? VertexSlots(e[1] >> 24, slot, words) : 0;
    return nv && words == e[0] ? nv : 0;
}

uint64_t Key(const uint32_t* w, uint32_t n) {
    const uint8_t cmd = w[0] >> 24;
    const bool poly = cmd >= 0x20 && cmd <= 0x3F, rect = cmd >= 0x60 && cmd <= 0x7F;
    const bool textured = (poly || rect) && (cmd & 0x04);
    uint64_t h = 1469598103934665603ull ^ cmd;  // FNV-1a
    auto mix = [&h](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    if (!textured) mix(w[0] & 0xFFFFFF);  // textured: the colour only modulates (lighting changes it)
    if (poly && textured) {
        int slot[4];
        uint32_t words = 0;
        const int nv = VertexSlots(cmd, slot, words);
        for (int k = 0; k < nv; ++k) mix(w[slot[k] + 1]);  // UV (+ CLUT / texpage) after each vertex
    }
    if (rect)
        for (uint32_t i = 2; i < n; ++i) mix(w[i]);  // UV word and/or size word
    return h;
}

// The log entry at e is the 3D tag (Ps1Gpu::SetProjectionTracker) of the packet after it.
bool Tag3D(const uint32_t* e) { return e[0] == 1 && e[1] == Ps1Gpu::kLog3D; }

int32_t Sx(uint32_t v) { return static_cast<int32_t>((v & 0x7FF) << 21) >> 21; }
// Offsets (not positions) are rounded, half away from zero: ties go the same way for every vertex moving
// the same way, wherever it is on screen (positions straddling 0 would round apart).
int32_t Round(double v) { return int32_t(std::lround(v)); }

} // namespace

Match MatchPrimitives(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur, int32_t max_jump) {
    std::unordered_map<uint64_t, std::vector<int32_t>> by_key;
    int slot[4];
    // Only 3D (GTE-projected) polygons move; 2D (sprites, portraits, text, windows, lines) is drawn as in
    // cur: stable at the game's rate beats smooth with wrong partners and sub-frame jitter.
    for (size_t i = 0, tag = 0; i < prev.size(); tag = Tag3D(&prev[i]), i += 1 + prev[i])
        if (tag && Slots(&prev[i], slot)) by_key[Key(&prev[i + 1], prev[i])].push_back(int32_t(i));
    // Every same-key pair within max_jump on all vertices, cheapest (total vertex travel) first: the true
    // partners of a scrolled floor win before an inserted or removed tile can steal a neighbour's.
    struct Cand { int32_t cost, pk, p; };
    std::vector<Cand> cands;
    std::vector<size_t> at;  // word offset of each packet of cur
    Match m;
    for (size_t i = 0, tag = 0; i < cur.size(); tag = Tag3D(&cur[i]), i += 1 + cur[i]) {
        const int32_t pk = int32_t(at.size());
        at.push_back(i);
        const int nv = tag ? Slots(&cur[i], slot) : 0;
        if (!nv) continue;
        ++m.prims;
        auto it = by_key.find(Key(&cur[i + 1], cur[i]));
        if (it == by_key.end()) continue;
        for (int32_t p : it->second) {
            int32_t cost = 0;
            for (int k = 0; k < nv && cost >= 0; ++k) {
                const uint32_t a = prev[p + 1 + slot[k]], b = cur[i + 1 + slot[k]];
                const int32_t dx = std::abs(Sx(b) - Sx(a)), dy = std::abs(Sx(b >> 16) - Sx(a >> 16));
                cost = dx > max_jump || dy > max_jump ? -1 : cost + dx + dy;
            }
            if (cost >= 0) cands.push_back({cost, pk, p});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        return x.cost != y.cost ? x.cost < y.cost : x.pk != y.pk ? x.pk < y.pk : x.p < y.p;
    });
    m.prev_of.assign(at.size(), -1);
    std::vector<bool> used(prev.size());  // by word offset of the prev packet
    for (const Cand& c : cands)
        if (m.prev_of[c.pk] < 0 && !used[c.p]) m.prev_of[c.pk] = c.p, used[c.p] = true, ++m.matched;
    return m;
}

void LerpCommands(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur, const Match& m, double t,
                  int32_t max_jump, std::vector<uint32_t>& out) {
    out = cur;
    int slot[4];
    auto put = [&](size_t w, int32_t ox, int32_t oy) {  // vertex word w at cur - offset
        out[w] = (uint32_t(Sx(cur[w]) - ox) & 0xFFFF) | uint32_t(Sx(cur[w] >> 16) - oy) << 16;
    };
    // Pass 1: the motion (cur - prev) of every vertex of a usable pair, keyed by its position in cur.
    struct Vote { uint32_t xy; int32_t dx, dy, prim; size_t w; };  // w: the vertex word in cur
    std::vector<Vote> votes;
    int32_t prims = 0;
    size_t pk = 0;
    for (size_t i = 0; i < cur.size(); i += 1 + cur[i], ++pk) {
        const int32_t p = pk < m.prev_of.size() ? m.prev_of[pk] : -1;
        if (p < 0) continue;
        const int nv = Slots(&cur[i], slot);
        Vote v[4];
        bool ok = true;
        for (int k = 0; k < nv && ok; ++k) {
            const uint32_t a = prev[p + 1 + slot[k]], b = cur[i + 1 + slot[k]];
            v[k] = {b & 0x07FF07FF, Sx(b) - Sx(a), Sx(b >> 16) - Sx(a >> 16), prims, i + 1 + slot[k]};
            ok = std::abs(v[k].dx) <= max_jump && std::abs(v[k].dy) <= max_jump;
        }
        if (ok && nv >= 3) votes.insert(votes.end(), v, v + nv), ++prims;
        else if (ok)  // lines and rectangles are no mesh: lerped on their own
            for (int k = 0; k < nv; ++k) put(i + 1 + slot[k], Round(v[k].dx * (1 - t)), Round(v[k].dy * (1 - t)));
    }
    // One motion per vertex position: primitives of a mesh share their vertices, and same-key partners
    // (repeated tiles, pillar strips, glass fans) can pair one primitive with a neighbour's previous
    // place; lerped per primitive, the shared vertex would land in two places that change every picture
    // (seams opening and closing). Where the votes of a shared vertex disagree, the odd ones out of an
    // agreeing group, else the pair(s) disagreeing at the most shared vertices, are wrong partners: they
    // cast no vote and are carried by their neighbours (pass 2). The (lower) median of the remaining
    // votes moves each vertex once for all; votes there that disagree by more than a pixel with every other
    // group are another object meeting it by chance (a shadow fan's rim on a floor vertex) and keep their
    // own motion, so neither drags the other.
    std::sort(votes.begin(), votes.end(), [](const Vote& x, const Vote& y) { return x.xy < y.xy; });
    auto group = [&votes](size_t a) { size_t b = a; while (b < votes.size() && votes[b].xy == votes[a].xy) ++b; return b; };
    std::vector<uint8_t> same(votes.size());               // the vote agrees (1 px) with another one at its vertex
    std::vector<int32_t> odd(static_cast<size_t>(prims));  // per pair: shared vertices where it agrees with nobody
    std::vector<uint8_t> wrong(static_cast<size_t>(prims));
    for (size_t a = 0, b; a < votes.size(); a = b) {
        b = group(a);
        for (size_t i = a; i < b && b - a > 1; ++i) {
            for (size_t j = a; j < b && !same[i]; ++j)
                same[i] = j != i && std::abs(votes[j].dx - votes[i].dx) <= 1 && std::abs(votes[j].dy - votes[i].dy) <= 1;
            odd[size_t(votes[i].prim)] += !same[i];
        }
    }
    for (size_t a = 0, b; a < votes.size(); a = b) {
        b = group(a);
        bool cluster = false;
        int32_t worst = 0;
        for (size_t i = a; i < b && b - a > 1; ++i)
            if (same[i]) cluster = true;
            else worst = std::max(worst, odd[size_t(votes[i].prim)]);
        for (size_t i = a; i < b && b - a > 1; ++i)
            if (!same[i] && (cluster || odd[size_t(votes[i].prim)] == worst)) wrong[size_t(votes[i].prim)] = 1;
    }
    struct Field { int32_t dx, dy, root; };
    std::vector<Field> field;
    std::unordered_map<uint32_t, int32_t> at;   // vertex position in cur -> field entry (its largest group)
    std::unordered_map<size_t, int32_t> own;    // vertex word of a voting pair -> the entry of its group
    std::vector<int32_t> xs, ys, grp;
    std::vector<size_t> idx;
    for (size_t a = 0, b; a < votes.size(); a = b) {
        idx.clear();
        for (b = a; b < votes.size() && votes[b].xy == votes[a].xy; ++b)
            if (!wrong[size_t(votes[b].prim)]) idx.push_back(b);
        grp.assign(idx.size(), -1);
        int32_t groups = 0, best = -1;
        size_t best_n = 0;
        for (size_t u = 0; u < idx.size(); ++u) {
            if (grp[u] >= 0) continue;
            grp[u] = groups++;  // single linkage within a pixel
            for (bool grew = true; grew;) {
                grew = false;
                for (size_t v = 0; v < idx.size(); ++v)
                    for (size_t x = 0; x < idx.size() && grp[v] < 0; ++x)
                        if (grp[x] == grp[u] && std::abs(votes[idx[v]].dx - votes[idx[x]].dx) <= 1 &&
                            std::abs(votes[idx[v]].dy - votes[idx[x]].dy) <= 1)
                            grp[v] = grp[u], grew = true;
            }
            xs.clear(), ys.clear();
            for (size_t v = 0; v < idx.size(); ++v)
                if (grp[v] == grp[u]) xs.push_back(votes[idx[v]].dx), ys.push_back(votes[idx[v]].dy), own[votes[idx[v]].w] = int32_t(field.size());
            std::nth_element(xs.begin(), xs.begin() + (xs.size() - 1) / 2, xs.end());
            std::nth_element(ys.begin(), ys.begin() + (ys.size() - 1) / 2, ys.end());
            if (xs.size() > best_n) best_n = xs.size(), best = int32_t(field.size());
            field.push_back({xs[(xs.size() - 1) / 2], ys[(ys.size() - 1) / 2], int32_t(field.size())});
        }
        if (best >= 0) at[votes[a].xy] = best;
    }
    auto entry = [&](size_t w) {  // the field entry moving vertex word w of cur, or -1
        if (auto it = own.find(w); it != own.end()) return it->second;
        const auto it = at.find(cur[w] & 0x07FF07FF);
        return it == at.end() ? -1 : it->second;
    };
    // A polygon without a usable pair (entering the view, a silhouette strip turning visible, a wrong
    // partner) follows the vertices it shares with moved ones, its others by their mean motion, instead
    // of tearing away from them (repeated: it may share them with another such polygon); one sharing none
    // is drawn as in cur.
    std::vector<size_t> polys;
    for (size_t i = 0, tag = 0; i < cur.size(); tag = Tag3D(&cur[i]), i += 1 + cur[i])
        if (tag && Slots(&cur[i], slot) >= 3) polys.push_back(i);
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i : polys) {
            const int nv = Slots(&cur[i], slot);
            int32_t mx = 0, my = 0, found = 0;
            for (int k = 0; k < nv; ++k)
                if (auto it = at.find(cur[i + 1 + slot[k]] & 0x07FF07FF); it != at.end())
                    mx += field[size_t(it->second)].dx, my += field[size_t(it->second)].dy, ++found;
            if (!found || found == nv) continue;
            for (int k = 0; k < nv; ++k)
                if (at.try_emplace(cur[i + 1 + slot[k]] & 0x07FF07FF, int32_t(field.size())).second)
                    field.push_back({Round(double(mx) / found), Round(double(my) / found), int32_t(field.size())});
            grew = true;
        }
    }
    // Meshes: the vertices of each polygon are joined (union-find over the field).
    auto root = [&field](int32_t x) {
        while (field[size_t(x)].root != x) x = field[size_t(x)].root = field[size_t(field[size_t(x)].root)].root;
        return x;
    };
    for (size_t i : polys) {
        const int nv = Slots(&cur[i], slot);
        const int32_t e0 = entry(i + 1 + slot[0]);
        for (int k = 1; k < nv && e0 >= 0; ++k) field[size_t(root(entry(i + 1 + slot[k])))].root = root(e0);
    }
    // Each mesh's median motion (its slide).
    std::vector<int32_t> by_mesh(field.size()), base_x(field.size()), base_y(field.size());
    for (size_t f = 0; f < field.size(); ++f) by_mesh[f] = int32_t(f);
    std::sort(by_mesh.begin(), by_mesh.end(), [&](int32_t a, int32_t b) { return root(a) < root(b); });
    for (size_t a = 0, b; a < by_mesh.size(); a = b) {
        xs.clear(), ys.clear();
        for (b = a; b < by_mesh.size() && root(by_mesh[b]) == root(by_mesh[a]); ++b)
            xs.push_back(field[size_t(by_mesh[b])].dx), ys.push_back(field[size_t(by_mesh[b])].dy);
        std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
        std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        base_x[size_t(root(by_mesh[a]))] = xs[xs.size() / 2], base_y[size_t(root(by_mesh[a]))] = ys[ys.size() / 2];
    }
    // Pass 2. Rounding each vertex on its own makes two vertices whose motions differ by a pixel snap at
    // different pictures: the edge between them grows, shrinks and grows again inside one game frame and
    // the texture on it swims at the presentation rate. So a mesh slides as one by its median motion b
    // (one rounding for all its vertices). A slower vertex (same way, 0 <= d < b) waits at its old place
    // until the slide reaches it; a faster one (up to kStep px more: perspective, GTE rounding) switches
    // its extra once, at t = 0.5, as a real game frame would. Every vertex then moves one way only and
    // every edge between them changes one way only. Other motions (a limb swinging, the other way) are
    // real and lerped per vertex. A mesh that moves at most a pixel (GTE rounding on still scenery: its
    // vertices disagree by a pixel between frames, and a shared one takes a neighbour's motion) is drawn as
    // in cur: a pixel cannot be split at 1x, and lerped it is drawn where it never was (edges jitter).
    constexpr int32_t kStep = 2;
    auto offset = [&](int32_t d, int32_t b) {
        if (std::abs(b) <= 1 && std::abs(d) <= 1) return 0;
        const int32_t sign = b < 0 || (b == 0 && d < 0) ? -1 : 1;  // mirrored so the slide is >= 0
        d *= sign, b *= sign;
        const int32_t slide = Round(b * (1 - t));
        const int32_t o = d < 0 || d > b + kStep ? Round(d * (1 - t)) : d <= b ? std::min(slide, d) : slide + (t < 0.5 ? d - b : 0);
        return o * sign;
    };
    for (size_t i : polys) {
        const int nv = Slots(&cur[i], slot);
        for (int k = 0; k < nv; ++k) {
            const int32_t e = entry(i + 1 + slot[k]);
            if (e < 0) break;  // no vertex moved (all or none are in the field)
            const Field& f = field[size_t(e)];
            const size_t r = size_t(root(e));
            put(i + 1 + slot[k], offset(f.dx, base_x[r]), offset(f.dy, base_y[r]));
        }
    }
}

void Replay(Ps1Gpu& gpu, const std::vector<uint32_t>& cmds) {
    for (size_t i = 0; i < cmds.size(); i += 1 + cmds[i])
        for (uint32_t k = 1; k <= cmds[i]; ++k) gpu.WriteGP0(cmds[i + k]);
}

namespace {
// The segment's first primitive draws inside a drawing area that contains the display start.
bool DrawsTo(const std::vector<uint32_t>& cmds, uint32_t x, uint32_t y) {
    uint32_t e3 = 0, e4 = 0;
    int slot[4];
    for (size_t i = 0; i < cmds.size(); i += 1 + cmds[i]) {
        if (!cmds[i]) continue;
        const uint32_t w = cmds[i + 1];
        if (w >> 24 == 0xE3) e3 = w;
        else if (w >> 24 == 0xE4) e4 = w;
        else if (Slots(&cmds[i], slot))
            return x >= (e3 & 0x3FF) && x <= (e4 & 0x3FF) && y >= ((e3 >> 10) & 0x1FF) && y <= ((e4 >> 10) & 0x1FF);
    }
    return false;
}
} // namespace

bool FrameInterpolator::Consume(std::vector<uint32_t>& log, const Ps1Gpu& gpu, uint64_t frame) {
    bool flipped = false;
    for (size_t i = 0; i < log.size();) {
        const uint32_t n = log[i];
        if (n == 0) {  // flip marker: the pending segment is the picture now on display
            std::swap(m_prev, m_cur);
            m_cur.cmds.swap(m_pending);
            m_pending.clear();
            m_cur.disp = gpu.GetDisplayArea();
            m_cur.disp.x = log[i + 1] & 0xFFFF;
            m_cur.disp.y = static_cast<uint16_t>(log[i + 1] >> 16);
            m_cur.flip_frame = frame;
            ++m_count;
            flipped = true;
            i += 2;
        } else {
            m_pending.insert(m_pending.end(), log.begin() + i, log.begin() + i + 1 + n);
            i += 1 + n;
        }
    }
    log.clear();
    if (!flipped) return false;
    // At the flip, not after the emulated frame: the next segment may already have drawn over it, and the
    // replay of a segment must start from the VRAM it was drawn over (the previous flip's).
    m_cur.vram = gpu.GetFlipVram();
    if (m_prev.vram.empty()) m_prev.vram = m_cur.vram;
    m_match = MatchPrimitives(m_prev.cmds, m_cur.cmds, kMaxJump);
    const GpuDisplayArea &a = m_prev.disp, &b = m_cur.disp;
    m_pair_ok = m_count >= 2 && !b.is_24bit && a.width == b.width && a.height == b.height && m_match.prims &&
                m_match.matched >= kMinMatched * m_match.prims && DrawsTo(m_cur.cmds, b.x, b.y);
    return true;
}

double FrameInterpolator::Fraction(double time) const {
    const double interval = double(m_cur.flip_frame - m_prev.flip_frame);
    return interval > 0 ? (time - double(m_cur.flip_frame)) / interval : 1.0;
}

void FrameInterpolator::Render(double t, const std::vector<uint16_t>*& out, GpuDisplayArea& disp) {
    // Not interpolable: the latest frame at once (holding the previous one only adds latency).
    if (t >= 1 || !m_pair_ok) { out = &m_cur.vram; disp = m_cur.disp; return; }
    if (t <= 0) { out = &m_prev.vram; disp = m_prev.disp; return; }
    LerpCommands(m_prev.cmds, m_cur.cmds, m_match, t, kMaxJump, m_lerped);
    // Replayed over the VRAM cur's segment was drawn over: unmoved, it redraws cur exactly (replayed over cur
    // itself, blended and partly covered primitives drew twice: brighter, seams, stale pixels).
    m_scratch.SetVram(m_prev.vram);
    m_scratch.WriteGP1(0x01000000);  // reset the command buffer: replay starts on a packet boundary
    Replay(m_scratch, m_lerped);
    out = &m_scratch.GetVram();
    disp = m_cur.disp;
}

} // namespace personarm::interp
