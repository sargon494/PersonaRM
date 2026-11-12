#include "trace/trace.h"

#include <algorithm>
#include <cstdio>
#include <istream>
#include <ostream>

namespace personarm::trace {

std::vector<AddrRange> DefaultRamMasks() { return {{0x00000000u, 0x00010000u}}; }

uint64_t Fnv1a64(const uint8_t* data, size_t size, uint64_t seed) {
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 0x100000001B3ull; }
    return h;
}

std::string Hex64(uint64_t v) { char b[20]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }
std::string Hex32(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", v); return b; }

std::string HashBytes(const uint8_t* d, size_t n) { return Hex64(Fnv1a64(d, n)); }

std::string HashMaskedRam(const uint8_t* ram, size_t size, const std::vector<AddrRange>& masks) {
    std::vector<uint8_t> copy(ram, ram + size);
    for (const auto& m : masks) {
        size_t b = m.first, e = std::min<size_t>(m.second, size);
        if (b < e) std::fill(copy.begin() + b, copy.begin() + e, uint8_t(0));
    }
    return HashBytes(copy.data(), copy.size());
}

std::string HashGpr(const CpuRegisters& r) {
    uint8_t buf[34 * 4];
    auto put = [&](size_t i, uint32_t v) { for (int k = 0; k < 4; ++k) buf[i * 4 + k] = uint8_t(v >> (8 * k)); };
    for (size_t i = 0; i < 32; ++i) put(i, i == 0 ? 0u : r.gpr[i]);
    put(32, r.hi);
    put(33, r.lo);
    return HashBytes(buf, sizeof buf);
}

// ── Writer ─────────────────────────────────────────────────────────────────

TraceWriter::TraceWriter(std::ostream& out, const TraceHeader& h) : m_out(out), m_masks(h.ram_masks) {
    JsonValue j = JsonValue::MakeObject();
    j.Set("v", kTraceVersion);
    j.Set("src", h.src);
    j.Set("build", h.build);
    j.Set("disc", h.disc);
    j.Set("exe_sha256", h.exe_sha256);
    j.Set("mode", h.mode);
    JsonValue masks = JsonValue::MakeArray();
    for (const auto& m : h.ram_masks) {
        JsonValue p = JsonValue::MakeArray();
        p.Push(Hex32(m.first));
        p.Push(Hex32(m.second));
        masks.Push(std::move(p));
    }
    j.Set("ram_masks", std::move(masks));
    m_out << j.Dump(-1) << '\n';
}

void TraceWriter::Emit(JsonValue rec) {
    rec.Set("seq", (double)m_seq++);
    m_out << rec.Dump(-1) << '\n';
}

static JsonValue Ev(const char* name) { JsonValue j = JsonValue::MakeObject(); j.Set("ev", name); return j; }

void TraceWriter::OnBiosCall(uint32_t vec, uint32_t fn, const std::array<uint32_t, 4>& a, uint32_t ra) {
    JsonValue j = Ev("call");
    j.Set("kind", "bios");
    j.Set("vec", Hex32(vec));
    j.Set("fn", fn);
    for (int i = 0; i < 4; ++i) j.Set("a" + std::to_string(i), Hex32(a[i]));
    j.Set("ra", Hex32(ra));
    Emit(std::move(j));
}

void TraceWriter::OnLoadFile(uint32_t id, uint32_t dest, uint32_t ra) {
    JsonValue j = Ev("load_file");
    j.Set("id", id);
    j.Set("dest", Hex32(dest));
    j.Set("ra", Hex32(ra));
    Emit(std::move(j));
}

void TraceWriter::OnCdRead(uint32_t lba, uint32_t sectors, uint32_t dest) {
    JsonValue j = Ev("cd_read");
    j.Set("lba", lba);
    j.Set("sectors", sectors);
    j.Set("dest", Hex32(dest));
    Emit(std::move(j));
}

void TraceWriter::OnVsync(uint32_t frame, const std::string& gp0_hash, uint32_t gp0_words) {
    JsonValue j = Ev("vsync");
    j.Set("frame", frame);
    if (!gp0_hash.empty()) { j.Set("gp0_hash", gp0_hash); j.Set("gp0_words", gp0_words); }
    Emit(std::move(j));
}

void TraceWriter::OnCheckpoint(const std::string& tag, uint32_t pc, const std::string& gpr, const std::string& ram,
                               const std::string& scratch, const std::string& vram) {
    JsonValue j = Ev("ckpt");
    j.Set("tag", tag);
    j.Set("pc", Hex32(pc));
    if (!gpr.empty()) j.Set("gpr_hash", gpr);
    if (!ram.empty()) j.Set("ram_hash", ram);
    if (!scratch.empty()) j.Set("scratch_hash", scratch);
    if (!vram.empty()) j.Set("vram_hash", vram);
    Emit(std::move(j));
}

void TraceWriter::OnIrq(const std::string& src, uint32_t pc) {
    JsonValue j = Ev("irq");
    j.Set("src", src);
    j.Set("pc", Hex32(pc));
    Emit(std::move(j));
}

void TraceWriter::OnMmio(uint32_t addr, uint32_t width, bool write, uint32_t value) {
    JsonValue j = Ev("mmio");
    j.Set("addr", Hex32(addr));
    j.Set("w", width);
    j.Set("dir", write ? "w" : "r");
    j.Set("val", Hex32(value));
    Emit(std::move(j));
}

void TraceWriter::OnEnd(const std::string& reason, uint32_t frames) {
    JsonValue j = Ev("end");
    j.Set("reason", reason);
    j.Set("frames", frames);
    Emit(std::move(j));
}

void TraceWriter::Checkpoint(const std::string& tag, const CpuRegisters& regs, const uint8_t* ram, size_t ram_size,
                             const uint8_t* scratch, size_t scratch_size, const uint8_t* vram, size_t vram_size) {
    OnCheckpoint(tag, regs.pc, HashGpr(regs), HashMaskedRam(ram, ram_size, m_masks),
                 scratch ? HashBytes(scratch, scratch_size) : "", vram ? HashBytes(vram, vram_size) : "");
}

// ── Reader ─────────────────────────────────────────────────────────────────

static bool Fail(std::string* e, size_t line, const std::string& msg) {
    if (e) *e = "line " + std::to_string(line) + ": " + msg;
    return false;
}

bool ReadTrace(std::istream& in, TraceFile* out, std::string* error) {
    TraceFile f;
    std::string line;
    size_t n = 0;
    bool have_header = false;
    double last_seq = -1;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string perr;
        auto v = JsonValue::Parse(line, &perr);
        if (!v) return Fail(error, n, "invalid JSON: " + perr);
        if (!v->IsObject()) return Fail(error, n, "record is not an object");
        if (!have_header) {
            const JsonValue* ver = v->Find("v");
            if (!ver || !ver->IsNumber() || (int)ver->AsNumber() != kTraceVersion)
                return Fail(error, n, "missing header or unsupported version");
            f.header = std::move(*v);
            have_header = true;
            continue;
        }
        const JsonValue* ev = v->Find("ev");
        const JsonValue* seq = v->Find("seq");
        if (!ev || !ev->IsString()) return Fail(error, n, "missing string field 'ev'");
        if (!seq || !seq->IsNumber() || seq->AsNumber() < 0) return Fail(error, n, "missing integer field 'seq'");
        if (seq->AsNumber() <= last_seq) return Fail(error, n, "'seq' not strictly increasing");
        last_seq = seq->AsNumber();
        f.events.push_back(std::move(*v));
    }
    if (!have_header) return Fail(error, 0, "empty trace (no header)");
    if (out) *out = std::move(f);
    return true;
}

// ── Comparator ─────────────────────────────────────────────────────────────

static std::vector<const JsonValue*> Filter(const TraceFile& t, const CompareOptions& o) {
    std::vector<const JsonValue*> r;
    for (const auto& e : t.events)
        if (!o.ignore_events.count(e.StringOr("ev", ""))) r.push_back(&e);
    return r;
}

// First differing field of two objects, or "" if equal. A field present on one side only is an
// accepted omission (e.g. Redux has no gp0_hash); `skip` fields are never compared.
static std::string DiffFields(const JsonValue& a, const JsonValue& b, const std::set<std::string>& skip) {
    for (const auto& [k, va] : a.Members()) {
        if (skip.count(k)) continue;
        const JsonValue* vb = b.Find(k);
        if (vb && va.Dump(-1) != vb->Dump(-1))
            return "field '" + k + "': " + va.Dump(-1) + " vs " + vb->Dump(-1);
    }
    return "";
}

static std::string Key(const JsonValue& e) { return e.StringOr("ev", "") + "/" + e.StringOr("kind", ""); }

Divergence CompareTraces(const TraceFile& a, const TraceFile& b, const CompareOptions& opt) {
    Divergence d;
    auto ea = Filter(a, opt), eb = Filter(b, opt);
    auto done = [&](size_t i, std::string why) {
        auto ctx = [&](const std::vector<const JsonValue*>& s) {
            std::vector<std::string> r;
            size_t lo = i > opt.context ? i - opt.context : 0, hi = std::min(s.size(), i + opt.context + 1);
            for (size_t k = lo; k < hi; ++k) r.push_back(s[k]->Dump(-1));
            return r;
        };
        d.found = true; d.index = i; d.reason = std::move(why);
        d.context_a = ctx(ea); d.context_b = ctx(eb);
        return d;
    };

    std::string h = DiffFields(a.header, b.header, {"src", "build"});
    if (!h.empty()) return done(0, "header " + h);

    size_t n = std::min(ea.size(), eb.size());
    for (size_t i = 0; i < n; ++i) {
        if (Key(*ea[i]) != Key(*eb[i]))
            return done(i, "event type: " + Key(*ea[i]) + " vs " + Key(*eb[i]));
        std::string f = DiffFields(*ea[i], *eb[i], {"seq"});
        if (!f.empty()) return done(i, "event '" + Key(*ea[i]) + "' " + f);
    }
    if (ea.size() != eb.size())
        return done(n, std::string("trace ") + (ea.size() < eb.size() ? "A" : "B") + " ends early");
    return d;
}

} // namespace personarm::trace
