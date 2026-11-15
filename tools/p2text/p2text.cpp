#include "p2text.h"

#include "asset_tools.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace personarm::tools::p2text {

static uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static void Wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
static uint16_t Rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
static size_t Align(size_t v, size_t a) { return (v + a - 1) / a * a; }

// Dialogue window limits, measured on frames of the game's own window (docs/TRANSLATION_TOOLS.md): lines start at x=16, the
// inside of the box ends at x=303 (US: 288 px); the box shows the speaker-name line plus 3 text lines (4 per page). Text past
// either limit is not clipped or wrapped: it is drawn over the border / below the box.
static constexpr unsigned kUsMaxPx = 288, kUsMaxLines = 4, kJpMaxPx = 288, kJpMaxLines = 4;

// â”€â”€ LZ â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool LzDecompress(const uint8_t* in, size_t n, size_t out_size, std::vector<uint8_t>& out, size_t* consumed) {
    out.clear();
    out.reserve(out_size);
    size_t i = 0;
    while (out.size() < out_size) {
        if (i >= n) return false;
        const uint8_t c = in[i++];
        if (c < 0x80) {
            const size_t k = size_t(c) + 1;
            if (n - i < k || out_size - out.size() < k) return false;
            out.insert(out.end(), in + i, in + i + k);
            i += k;
        } else {
            if (i >= n) return false;
            const size_t k = size_t(c & 0x7F) + 3, d = size_t(in[i++]) + 1;
            if (d > out.size() || out_size - out.size() < k) return false;
            for (size_t j = 0; j < k; ++j) out.push_back(out[out.size() - d]);
        }
    }
    if (consumed) *consumed = i;
    return true;
}

std::vector<uint8_t> LzCompress(const uint8_t* in, size_t n, bool allow_overlap) {
    std::vector<uint8_t> out;
    size_t lit_start = 0;
    auto flush = [&](size_t end) {
        while (lit_start < end) {
            const size_t k = std::min<size_t>(128, end - lit_start);
            out.push_back(uint8_t(k - 1));
            out.insert(out.end(), in + lit_start, in + lit_start + k);
            lit_start += k;
        }
    };
    size_t i = 0;
    while (i < n) {
        size_t best = 0, best_d = 0;
        for (size_t d = 1; d <= 256 && d <= i; ++d) {
            const size_t cap = std::min<size_t>(130, n - i), lim = allow_overlap ? cap : std::min(cap, d);
            size_t k = 0;
            while (k < lim && in[i + k] == in[i + k - d]) ++k;
            if (k > best) { best = k; best_d = d; }
        }
        if (best >= 3) {
            flush(i);
            out.push_back(uint8_t(0x80 | (best - 3)));
            out.push_back(uint8_t(best_d - 1));
            i += best;
            lit_start = i;
        } else {
            ++i;
        }
    }
    flush(n);
    return out;
}

// â”€â”€ Banks â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

size_t CodeLength(uint16_t c) {
    if ((c >> 12) != 1) return 1;
    const size_t l = (c >> 8) & 0xF;
    return l ? l : 1;
}

bool ParseBank(const std::vector<uint8_t>& dec, Bank& b) {
    b = Bank{};
    if (dec.size() < 24 || Rd32(dec.data()) != 0x18 || Rd32(dec.data() + 4) != 0x18) return false;
    const uint32_t n = Rd32(dec.data() + 8);
    if (n < 1 || n > 255) return false;   // n = number of events; the section table always has 3 entries
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t o = Rd32(dec.data() + 12 + i * 4);
        if (o > dec.size() || (i && o < b.off.back())) return false;
        b.off.push_back(o);
    }
    b.data = dec;
    b.msg_off = b.off.back();
    std::set<size_t> seen;
    for (size_t p = b.off[0]; p + 8 <= b.off[1]; p += 8) {
        const uint32_t ptr = Rd32(dec.data() + p + 4);
        if (Rd32(dec.data() + p) == 19 && ptr >= b.off[1] && size_t(ptr) + 4 <= b.off[2] && seen.insert(ptr).second)
            b.ref_ptrs.push_back(ptr);
    }
    return true;
}

Messages SplitMessages(const Bank& b) {
    Messages m;
    const uint8_t* d = b.data.data();
    const size_t end = b.data.size();
    size_t pos = b.msg_off;
    while (pos + 2 <= end) {
        Message msg;
        msg.id = uint32_t(m.list.size());
        msg.offset = uint32_t(pos - b.msg_off);
        size_t p = pos;
        bool done = false;
        while (p + 2 <= end && !done) {
            const uint16_t c = Rd16(d + p);
            const size_t len = std::min(CodeLength(c), (end - p) / 2);
            for (size_t k = 0; k < len; ++k) msg.codes.push_back(Rd16(d + p + k * 2));
            p += len * 2;
            done = (c == 0x1103);
        }
        if (!done) break;
        m.list.push_back(std::move(msg));
        pos = p;
    }
    m.tail.assign(b.data.begin() + long(pos), b.data.end());
    std::map<uint32_t, size_t> by_off;
    for (size_t i = 0; i < m.list.size(); ++i) by_off[m.list[i].offset] = i;
    for (size_t p : b.ref_ptrs) {
        auto it = by_off.find(Rd32(d + p));
        if (it == by_off.end()) ++m.bad_refs; else ++m.list[it->second].refs;
    }
    for (const auto& x : m.list) if (!x.refs) ++m.orphans;
    return m;
}

bool DecodeBankChunk(const std::vector<uint8_t>& file, const BankRef& r, std::vector<uint8_t>& dec) {
    if (r.size < 12 || r.pos + 12 > file.size() || r.usize > (1u << 24)) return false;
    const size_t avail = std::min<size_t>(r.size, file.size() - r.pos) - 12;
    return LzDecompress(file.data() + r.pos + 12, avail, r.usize, dec);
}

std::vector<BankRef> FindBanks(const std::vector<uint8_t>& file, unsigned* tried) {
    std::vector<BankRef> banks;
    std::vector<Chunk> chunks;
    std::string err;
    WalkChunks(file.data(), file.size(), chunks, err);   // a malformed tail still leaves the leading chunks usable
    for (const auto& c : chunks) {
        const uint32_t t = c.type & 0xFFFF;
        if ((t != 0x201 && t != 0x101) || c.size < 12 || c.offset + 12 > file.size()) continue;
        if (tried) ++*tried;
        BankRef r{c.offset, c.type, c.size, Rd32(file.data() + c.offset + 8)};
        std::vector<uint8_t> dec;
        Bank b;
        if (DecodeBankChunk(file, r, dec) && ParseBank(dec, b) && !SplitMessages(b).list.empty()) banks.push_back(r);
    }
    return banks;
}

// â”€â”€ Text <-> codes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static int Utf8Decode(const std::string& s, size_t i, uint32_t& cp) {
    const uint8_t c = uint8_t(s[i]);
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) return 0;
    cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (int k = 1; k < n; ++k) {
        const uint8_t x = uint8_t(s[i + k]);
        if ((x & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (x & 0x3F);
    }
    return n;
}

GlyphMap::GlyphMap() { for (uint32_t g = 0x20; g <= 0x7E; ++g) Set(g, std::string(1, char(g))); }

void GlyphMap::Set(uint32_t glyph, const std::string& utf8) {
    if (auto it = to_text.find(glyph); it != to_text.end()) {
        auto r = to_glyph.find(it->second);
        if (r != to_glyph.end() && r->second == glyph) to_glyph.erase(r);
    }
    to_text[glyph] = utf8;
    auto r = to_glyph.find(utf8);
    if (r == to_glyph.end() || glyph < r->second) to_glyph[utf8] = glyph;   // duplicates: lowest id wins
}

bool GlyphMap::Load(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "no se puede abrir el mapa " + path; return false; }
    std::string line;
    for (int ln = 1; std::getline(f, line); ++ln) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (ln == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        auto fail = [&](const char* why) { error = path + ":" + std::to_string(ln) + ": " + why; return false; };
        if (tab == std::string::npos) return fail("falta TAB entre id y caracter");
        char* e = nullptr;
        const std::string id = line.substr(0, tab);
        const unsigned long g = std::strtoul(id.c_str(), &e, (id.size() > 1 && id[0] == '0' && (id[1] == 'x' || id[1] == 'X')) ? 16 : 10);
        if (id.empty() || *e || g >= 0x1000) return fail("id de glifo invalido (0..4095)");
        const std::string txt = line.substr(tab + 1);
        uint32_t cp;
        const int n = txt.empty() ? 0 : Utf8Decode(txt, 0, cp);
        if (!n || size_t(n) != txt.size()) return fail("el texto debe ser exactamente un caracter UTF-8");
        Set(uint32_t(g), txt);
    }
    return true;
}

static const char* Named(uint16_t c, size_t n) {
    if (n == 1) switch (c) { case 0x1101: return "<br>"; case 0x1102: return "<page>"; case 0x1103: return "<end>"; case 0x1131: return "<indent>"; }
    return nullptr;
}

// Decodes a canonical packed run at codes[i]==0x1136 into escaped text; returns words used, 0 if not canonical.
static size_t PackedRun(const std::vector<uint16_t>& codes, size_t i, std::string& text) {
    std::string raw;
    size_t j = i + 1;
    for (; j < codes.size(); ++j) {
        const uint8_t lo = uint8_t(codes[j] & 0xFF), hi = uint8_t(codes[j] >> 8);
        if (!lo) { if (hi) return 0; break; }                       // 0x0000 = even-length terminator
        raw += char(lo);
        if (!hi) break;                                             // odd-length terminator
        raw += char(hi);
    }
    if (j >= codes.size() || raw.empty()) return 0;
    for (char ch : raw) if (uint8_t(ch) < 0x20 || uint8_t(ch) > 0x7E) return 0;
    for (char ch : raw) { if (ch == '<') text += "<lt>"; else text += ch; }
    return j + 1 - i;
}

bool UsesPackedText(const Messages& m) {
    for (const auto& x : m.list) for (uint16_t c : x.codes) if (c == 0x1136) return true;
    return false;
}

std::string Render(const std::vector<uint16_t>& codes, const GlyphMap& m) {
    std::string s;
    char buf[32];
    for (size_t i = 0; i < codes.size();) {
        const uint16_t c = codes[i];
        if (c < 0x1000) {
            auto it = m.to_text.find(c);
            const bool safe = !m.packed && it != m.to_text.end() && it->second.find_first_of("<\t\r\n") == std::string::npos;
            if (safe) s += it->second; else { std::snprintf(buf, sizeof buf, "<g%04u>", c); s += buf; }
            ++i;
            continue;
        }
        const size_t n = std::min(CodeLength(c), codes.size() - i);
        if (c == 0x1136 && m.packed) {
            std::string run;
            const size_t used = PackedRun(codes, i, run);
            if (used) { s += run; i += used; continue; }
        }
        if (const char* nm = Named(c, n)) s += nm;
        else if (c == 0x122E && n == 2) { std::snprintf(buf, sizeof buf, "<color=%u>", codes[i + 1]); s += buf; }
        else {
            std::snprintf(buf, sizeof buf, "<c:%04x", c);
            s += buf;
            for (size_t k = 1; k < n; ++k) { std::snprintf(buf, sizeof buf, " %04x", codes[i + k]); s += buf; }
            s += '>';
        }
        i += n;
    }
    return s;
}

bool Encode(const std::string& text, const GlyphMap& m, std::vector<uint16_t>& out, std::vector<std::string>& errors) {
    const size_t first = out.size(), nerr = errors.size();
    char buf[64];
    std::string pk;                                   // pending packed ASCII run (map.packed)
    auto flush = [&]() {
        if (pk.empty()) return;
        out.push_back(0x1136);
        for (size_t k = 0; k < pk.size(); k += 2)
            out.push_back(uint16_t(uint8_t(pk[k]) | (k + 1 < pk.size() ? uint16_t(uint8_t(pk[k + 1])) << 8 : 0)));
        if (pk.size() % 2 == 0) out.push_back(0);
        pk.clear();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            const size_t e = text.find('>', i);
            if (e == std::string::npos) { errors.push_back("etiqueta '<' sin cerrar"); break; }
            const std::string t = text.substr(i + 1, e - i - 1);
            i = e + 1;
            unsigned v = 0;
            char tail = 0;
            if (!(t == "lt" && m.packed)) flush();
            if (t == "lt" && m.packed) pk += '<';
            else if (t == "br") out.push_back(0x1101);
            else if (t == "page") out.push_back(0x1102);
            else if (t == "end") out.push_back(0x1103);
            else if (t == "indent") out.push_back(0x1131);
            else if (std::sscanf(t.c_str(), "color=%u%c", &v, &tail) == 1 && v <= 0xFFFF) { out.push_back(0x122E); out.push_back(uint16_t(v)); }
            else if (std::sscanf(t.c_str(), "g%u%c", &v, &tail) == 1 && v < 0x1000) out.push_back(uint16_t(v));
            else if (t.compare(0, 2, "c:") == 0) {
                std::vector<uint16_t> w;
                const char* p = t.c_str() + 2;
                bool ok = true;
                while (*p) {
                    char* ep;
                    const unsigned long x = std::strtoul(p, &ep, 16);
                    if (ep == p || x > 0xFFFF) { ok = false; break; }
                    w.push_back(uint16_t(x));
                    p = ep;
                    while (*p == ' ') ++p;
                }
                if (!ok || w.empty() || (w[0] >> 12) == 0 || CodeLength(w[0]) != w.size()) errors.push_back("etiqueta <" + t + "> invalida (codigo >=0x1000 con L palabras)");
                else out.insert(out.end(), w.begin(), w.end());
            } else errors.push_back("etiqueta desconocida <" + t + ">");
            continue;
        }
        uint32_t cp = 0;
        const int n = Utf8Decode(text, i, cp);
        if (!n) { errors.push_back("UTF-8 invalido"); break; }
        if (m.packed && n == 1 && cp >= 0x20 && cp <= 0x7E) { pk += char(cp); ++i; continue; }
        auto it = m.to_glyph.find(text.substr(i, size_t(n)));
        if (it == m.to_glyph.end()) {
            std::snprintf(buf, sizeof buf, "U+%04X '", cp);
            errors.push_back(std::string(buf) + text.substr(i, size_t(n)) + "' sin glifo");
        } else out.push_back(uint16_t(it->second));
        i += size_t(n);
    }
    flush();
    if (errors.size() == nerr) {
        const auto ends = std::count(out.begin() + long(first), out.end(), uint16_t(0x1103));
        if (out.size() == first || out.back() != 0x1103 || ends != 1) errors.push_back("el mensaje debe terminar con <end> (una sola vez)");
    }
    return errors.size() == nerr;
}

// â”€â”€ Rebuild â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool RebuildBank(const Bank& b, const Messages& orig, const std::vector<std::vector<uint16_t>>& msgs,
                 std::vector<uint8_t>& out_dec, RebuildReport& rep, std::string& error) {
    if (msgs.size() != orig.list.size()) { error = "numero de mensajes distinto del original"; return false; }
    rep = RebuildReport{};
    rep.old_dec = b.data.size();
    rep.old_msg_bytes = b.data.size() - b.msg_off;
    std::vector<uint8_t> sec;
    std::vector<uint32_t> new_off;
    for (size_t i = 0; i < msgs.size(); ++i) {
        new_off.push_back(uint32_t(sec.size()));
        if (msgs[i] != orig.list[i].codes) ++rep.changed;
        const size_t o = sec.size();
        sec.resize(o + msgs[i].size() * 2);
        if (!msgs[i].empty()) std::memcpy(sec.data() + o, msgs[i].data(), msgs[i].size() * 2);
    }
    sec.insert(sec.end(), orig.tail.begin(), orig.tail.end());
    if (!rep.changed) { out_dec = b.data; rep.new_dec = out_dec.size(); rep.new_msg_bytes = rep.old_msg_bytes; return true; }
    while ((b.msg_off + sec.size()) % 4) sec.push_back(0);
    out_dec.assign(b.data.begin(), b.data.begin() + long(b.msg_off));
    out_dec.insert(out_dec.end(), sec.begin(), sec.end());
    std::map<uint32_t, uint32_t> remap;
    for (size_t i = 0; i < msgs.size(); ++i) remap[orig.list[i].offset] = new_off[i];
    for (size_t p : b.ref_ptrs) {
        auto it = remap.find(Rd32(b.data.data() + p));
        if (it != remap.end()) Wr32(out_dec.data() + p, it->second);
    }
    for (size_t i = 0; i < msgs.size(); ++i)
        if (!orig.list[i].refs && new_off[i] != orig.list[i].offset) ++rep.moved_orphans;
    if (rep.moved_orphans)
        rep.warnings.push_back(std::to_string(rep.moved_orphans) + " mensajes sin referencia op 19 han cambiado de offset (si otro opcode los apunta se rompen)");
    if (orig.bad_refs) rep.warnings.push_back(std::to_string(orig.bad_refs) + " operandos op 19 no apuntan a inicio de mensaje (sin tocar)");
    rep.new_dec = out_dec.size();
    rep.new_msg_bytes = sec.size();
    return true;
}

bool PatchFile(const std::vector<uint8_t>& file, const BankRef& r, const std::vector<uint8_t>& new_dec,
               bool keep_original_if_same, std::vector<uint8_t>& out, std::string& error) {
    if (keep_original_if_same) {
        std::vector<uint8_t> cur;
        if (DecodeBankChunk(file, r, cur) && cur == new_dec) { out = file; return true; }
    }
    const std::vector<uint8_t> comp = LzCompress(new_dec.data(), new_dec.size());
    std::vector<uint8_t> chunk(12 + comp.size());
    Wr32(chunk.data(), r.type);
    Wr32(chunk.data() + 4, uint32_t(chunk.size()));
    Wr32(chunk.data() + 8, uint32_t(new_dec.size()));
    std::memcpy(chunk.data() + 12, comp.data(), comp.size());
    chunk.resize(Align(chunk.size(), 4));

    const size_t orig_next = std::min(file.size(), r.pos + Align(r.size, 4));
    size_t z = orig_next;
    while (z < file.size() && !file[z]) ++z;           // first non-zero byte after the chunk
    out.assign(file.begin(), file.begin() + long(r.pos));
    out.insert(out.end(), chunk.begin(), chunk.end());
    if (z >= file.size()) {                            // only padding follows: keep the sector alignment of the file
        const size_t want = file.size() % 0x800 == 0 ? Align(out.size(), 0x800) : out.size() + (file.size() - orig_next);
        out.resize(std::max(want, out.size()), 0);
    } else {
        if (z != orig_next) out.resize(z % 0x800 == 0 ? Align(out.size(), 0x800) : out.size() + (z - orig_next), 0);
        out.insert(out.end(), file.begin() + long(z), file.end());
    }
    (void)error;
    return true;
}

// â”€â”€ Font preview â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// US resident code at 0x8001BF2C: per char `idx[c - 0x20]` (96 B at 0x80010464) selects a {left, width} pair (16 at 0x80010444);
// advance = width + 1. The tables are read from the user's own EXE, never embedded.
bool UsMetricsFromExe(const std::vector<uint8_t>& exe, Metrics& m, std::string& error) {
    constexpr uint32_t kPairs = 0x80010444, kIdx = 0x80010464, kCode = 0x8001BF48;
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8)) { error = "no es un PS-X EXE"; return false; }
    const uint32_t base = Rd32(exe.data() + 0x18), size = Rd32(exe.data() + 0x1C);
    auto at = [&](uint32_t a, size_t n) -> const uint8_t* {
        return a >= base && a - base + n <= size && 0x800 + (a - base) + n <= exe.size() ? exe.data() + 0x800 + (a - base) : nullptr;
    };
    // the routine loads both table addresses here (`addiu t0,v0,0x464`, `addiu a3,v0,0x444`): ties the offsets to the US build
    const uint8_t* code = at(kCode, 12);
    if (!code || Rd32(code) != 0x24480464 || Rd32(code + 8) != 0x24470444) { error = "el ejecutable no es el de SLUS-01158 (rutina de anchos no reconocida)"; return false; }
    const uint8_t* pairs = at(kPairs, 32);
    const uint8_t* idx = at(kIdx, 96);
    if (!pairs || !idx) { error = "tabla de anchos fuera del ejecutable"; return false; }
    for (int i = 0; i < 96; ++i) {
        if (idx[i] > 15) { error = "tabla de anchos invalida"; return false; }
        m.adv[i] = uint8_t(pairs[2 * idx[i] + 1] + 1);
    }
    m.us = true;
    return true;
}

// Measured on the game's own text window (US: f7000+ frames of F0057; see docs/TRANSLATION_TOOLS.md).
Metrics DefaultMetrics(bool us) {
    Metrics m;
    m.max_px = us ? kUsMaxPx : kJpMaxPx;
    m.max_lines = us ? kUsMaxLines : kJpMaxLines;
    return m;
}

std::vector<LineWidth> MeasureLines(const std::vector<uint16_t>& codes, const Metrics& m) {
    std::vector<LineWidth> out;
    LineWidth cur{1, 1, 0};
    bool visible = false;                                // line has at least one glyph/char (control-only lines do not count)
    auto close = [&](bool new_page) {
        if (visible) out.push_back(cur);
        if (new_page) { cur.page++; cur.line = 1; } else cur.line++;
        cur.px = 0;
        visible = false;
    };
    for (size_t i = 0; i < codes.size();) {
        const uint16_t c = codes[i];
        if (c == 0x1136) {                               // packed ASCII run: two chars per u16, ends at the first NUL byte
            size_t j = i + 1;
            for (bool stop = false; j < codes.size() && !stop; ++j)
                for (uint8_t ch : {uint8_t(codes[j] & 0xFF), uint8_t(codes[j] >> 8)}) {
                    if (!ch) { stop = true; break; }
                    visible = true;
                    cur.px += (m.us && ch >= 0x20 && ch < 0x80) ? m.adv[ch - 0x20] : 0;
                }
            i = j;
            continue;
        }
        if (c < 0x1000) { visible = true; cur.px += 12; ++i; continue; }
        if (c == 0x1101) close(false);
        else if (c == 0x1102 || c == 0x1103) close(true);
        i += std::min(CodeLength(c), codes.size() - i);
    }
    close(false);
    return out;
}

std::vector<std::string> CheckLayout(const std::vector<uint16_t>& codes, const Metrics& m, const std::string& where) {
    std::vector<std::string> w;
    std::map<unsigned, unsigned> lines;
    for (const auto& l : MeasureLines(codes, m)) {
        ++lines[l.page];
        if (m.max_px && l.px > m.max_px)
            w.push_back(where + " pag " + std::to_string(l.page) + " linea " + std::to_string(l.line) + ": " + std::to_string(l.px) +
                        " px > " + std::to_string(m.max_px) + " px de ventana");
    }
    for (const auto& [pg, n] : lines)
        if (m.max_lines && n > m.max_lines)
            w.push_back(where + " pag " + std::to_string(pg) + ": " + std::to_string(n) + " lineas > " + std::to_string(m.max_lines) + " por pagina");
    return w;
}

std::vector<uint8_t> FontSheetBmp(const std::vector<uint8_t>& font, unsigned first, unsigned count) {
    const unsigned cols = 32, cell = 13, sc = 2, rows = (count + cols - 1) / cols;
    const unsigned W = cols * cell * sc, H = std::max(1u, rows) * cell * sc, stride = (W + 3) & ~3u;
    std::vector<uint8_t> img(size_t(stride) * H, 255);
    for (unsigned g = 0; g < count; ++g) {
        const size_t o = 0x480 + size_t(first + g) * 18;
        if (o + 18 > font.size()) break;
        for (unsigned p = 0; p < 144; ++p) {
            if (!((font[o + p / 8] >> (p % 8)) & 1)) continue;
            const unsigned x = ((g % cols) * cell + p % 12) * sc, y = ((g / cols) * cell + p / 12) * sc;
            for (unsigned dy = 0; dy < sc; ++dy)
                for (unsigned dx = 0; dx < sc; ++dx) img[size_t(H - 1 - (y + dy)) * stride + x + dx] = 0;   // BMP is bottom-up
        }
    }
    std::vector<uint8_t> bmp(14 + 40 + 1024);
    auto w32 = [&](size_t o, uint32_t v) { Wr32(bmp.data() + o, v); };
    bmp[0] = 'B'; bmp[1] = 'M';
    w32(2, uint32_t(bmp.size() + img.size())); w32(10, 14 + 40 + 1024);
    w32(14, 40); w32(18, W); w32(22, H); bmp[26] = 1; bmp[28] = 8; w32(34, uint32_t(img.size())); w32(46, 256);
    for (unsigned i = 0; i < 256; ++i) { bmp[54 + i * 4] = bmp[55 + i * 4] = bmp[56 + i * 4] = uint8_t(i); }
    bmp.insert(bmp.end(), img.begin(), img.end());
    return bmp;
}

} // namespace personarm::tools::p2text
