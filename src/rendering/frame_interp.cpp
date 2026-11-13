#include "rendering/frame_interp.h"

#include <climits>
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

int32_t Sx(uint32_t v) { return static_cast<int32_t>((v & 0x7FF) << 21) >> 21; }

constexpr size_t kMaxCandidates = 256;  // bounds the per-key scan (a floor of identical tiles)

} // namespace

Match MatchPrimitives(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur) {
    std::unordered_map<uint64_t, std::vector<int32_t>> by_key;
    int slot[4];
    for (size_t i = 0; i < prev.size(); i += 1 + prev[i])
        if (Slots(&prev[i], slot)) by_key[Key(&prev[i + 1], prev[i])].push_back(int32_t(i));
    Match m;
    for (size_t i = 0; i < cur.size(); i += 1 + cur[i]) {
        int32_t p = -1;
        if (Slots(&cur[i], slot)) {
            ++m.prims;
            auto it = by_key.find(Key(&cur[i + 1], cur[i]));
            if (it != by_key.end() && !it->second.empty()) {
                // Nearest first vertex among the unused candidates (repeated tiles, glyphs, scrolling).
                std::vector<int32_t>& c = it->second;
                const uint32_t v = cur[i + 1 + slot[0]];
                size_t best = 0;
                int32_t best_d = INT32_MAX;
                for (size_t k = 0; k < c.size() && k < kMaxCandidates; ++k) {
                    const uint32_t q = prev[c[k] + 1 + slot[0]];
                    const int32_t d = std::abs(Sx(q) - Sx(v)) + std::abs(Sx(q >> 16) - Sx(v >> 16));
                    if (d < best_d) best_d = d, best = k;
                }
                p = c[best];
                c[best] = c.back();
                c.pop_back();
                ++m.matched;
            }
        }
        m.prev_of.push_back(p);
    }
    return m;
}

void LerpCommands(const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur, const Match& m, double t,
                  int32_t max_jump, std::vector<uint32_t>& out) {
    out = cur;
    size_t pk = 0;
    int slot[4];
    for (size_t i = 0; i < cur.size(); i += 1 + cur[i], ++pk) {
        const int32_t p = pk < m.prev_of.size() ? m.prev_of[pk] : -1;
        if (p < 0) continue;
        const int nv = Slots(&cur[i], slot);
        bool ok = true;
        for (int k = 0; k < nv && ok; ++k) {
            const uint32_t a = prev[p + 1 + slot[k]], b = cur[i + 1 + slot[k]];
            ok = std::abs(Sx(b) - Sx(a)) <= max_jump && std::abs(Sx(b >> 16) - Sx(a >> 16)) <= max_jump;
        }
        if (!ok) continue;
        for (int k = 0; k < nv; ++k) {
            const uint32_t a = prev[p + 1 + slot[k]], b = cur[i + 1 + slot[k]];
            auto lerp = [t](int32_t x0, int32_t x1) { return int32_t(std::lround(x0 + (x1 - x0) * t)); };
            const int32_t x = lerp(Sx(a), Sx(b)), y = lerp(Sx(a >> 16), Sx(b >> 16));
            out[i + 1 + slot[k]] = (uint32_t(x) & 0xFFFF) | uint32_t(y) << 16;
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
    m_cur.vram = gpu.GetVram();
    if (m_prev.vram.empty()) m_prev.vram = m_cur.vram;
    m_match = MatchPrimitives(m_prev.cmds, m_cur.cmds);
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
    if (t >= 1) { out = &m_cur.vram; disp = m_cur.disp; return; }
    if (t <= 0 || !m_pair_ok) { out = &m_prev.vram; disp = m_prev.disp; return; }
    LerpCommands(m_prev.cmds, m_cur.cmds, m_match, t, kMaxJump, m_lerped);
    m_scratch.SetVram(m_cur.vram);
    m_scratch.WriteGP1(0x01000000);  // reset the command buffer: replay starts on a packet boundary
    Replay(m_scratch, m_lerped);
    out = &m_scratch.GetVram();
    disp = m_cur.disp;
}

} // namespace personarm::interp
