#include "codegen.h"

#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "cfg.h"
#include "mips_decode.h"
#include "runtime/recomp_runtime.h"

namespace personarm::recomp {

namespace {

std::string Hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08Xu", v); return b; }
std::string Addr(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
std::string R(uint32_t r) { return "s.gpr[" + std::to_string(r) + "]"; }
// A write to $zero still evaluates the value (a load from MMIO has side effects).
std::string Set(uint32_t r, const std::string& e) { return r ? R(r) + " = " + e + ";" : "static_cast<void>(" + e + ");"; }

struct Image {
    const uint8_t* p;
    size_t size;
    uint32_t base;
    bool In(uint32_t a) const {
        return a >= base && static_cast<uint64_t>(a) + 4 <= static_cast<uint64_t>(base) + size && ((a - base) & 3u) == 0;
    }
    uint32_t Word(uint32_t a) const {
        const uint8_t* q = p + (a - base);
        return static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
               (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
    }
};

// Words the interpreter executes (anything else is refused rather than guessed).
bool Supported(uint32_t w) {
    const uint32_t op = OpOf(w);
    switch (op) {
        case 0x00: {
            const uint32_t f = FunctOf(w);
            return f == 0x08 || f == 0x09 || f == 0x0C || f == 0x0D || Decode(w, 0).kind == InsnKind::Alu;
        }
        case 0x01: { const uint32_t rt = RtOf(w); return rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11; }
        case 0x10: case 0x12:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: case 0x32: case 0x3A:
            return true;
        default:
            return op >= 0x02 && op <= 0x0F;
    }
}

struct Use {
    uint32_t reads = 0;    // registers read (bit mask)
    uint32_t writes = 0;   // registers written immediately
    uint32_t delayed = 0;  // register written through the load delay
};

Use RegUse(uint32_t w) {
    Use u;
    const uint32_t op = OpOf(w), rs = RsOf(w), rt = RtOf(w), rd = RdOf(w), f = FunctOf(w);
    const uint32_t S = 1u << rs, T = 1u << rt, D = 1u << rd;
    switch (op) {
        case 0x00:
            if (f <= 0x03) { u.reads = T; u.writes = D; }
            else if (f <= 0x07) { u.reads = S | T; u.writes = D; }
            else if (f == 0x08) u.reads = S;
            else if (f == 0x09) { u.reads = S; u.writes = 1u << (rd ? rd : 31); }
            else if (f == 0x10 || f == 0x12) u.writes = D;
            else if (f == 0x11 || f == 0x13) u.reads = S;
            else if (f >= 0x18 && f <= 0x1B) u.reads = S | T;
            else if (f >= 0x20) { u.reads = S | T; u.writes = D; }
            break;
        case 0x01: u.reads = S; if (rt & 0x10) u.writes = 1u << 31; break;
        case 0x03: u.writes = 1u << 31; break;
        case 0x04: case 0x05: u.reads = S | T; break;
        case 0x06: case 0x07: u.reads = S; break;
        case 0x0F: u.writes = T; break;
        case 0x10:
            if (rs == 0x00) { u.delayed = T; if (rd != 8 && rd != 12 && rd != 13 && rd != 14) u.reads = T; }
            else if (rs == 0x04) u.reads = T;
            break;
        case 0x12:
            if (w & 0x02000000u) break;
            if (rs == 0x00 || rs == 0x02) u.delayed = T;
            else if (rs == 0x04 || rs == 0x06) u.reads = T;
            break;
        // LWL/LWR merge with rt, but the interpreter forwards an in-flight load to them, so that
        // read never sees a stale value and is not a hazard.
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
            u.reads = S; u.delayed = T; break;
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: u.reads = S | T; break;
        case 0x32: case 0x3A: u.reads = S; break;
        default:
            if (op >= 0x08 && op <= 0x0E) { u.reads = S; u.writes = T; }
            break;
    }
    u.writes &= ~1u;
    u.delayed &= ~1u;
    return u;
}

// True when executing `next` right after the delayed load `x` differs from the literal translation.
bool LoadDelayHazard(uint32_t x, uint32_t next) {
    const Use ux = RegUse(x);
    if (!ux.delayed) return false;
    const Use un = RegUse(next);
    return ((un.reads | un.writes) & ux.delayed) != 0;
}

struct Func {
    uint32_t entry = 0;
    bool ok = true;
    std::string reason;
    std::set<uint32_t> starts;                       // instruction addresses translated as statements
    std::set<uint32_t> labels;                       // jump targets inside the function
    std::map<uint32_t, std::set<uint32_t>> tables;   // jr address -> switch targets
};

std::string Fail(const char* what, uint32_t a) { return std::string(what) + " at " + Addr(a); }

Func Analyze(const Image& img, uint32_t entry, const std::set<uint32_t>& entries) {
    Func f;
    f.entry = entry;
    if (!img.In(entry)) { f.ok = false; f.reason = Fail("entry outside the image", entry); return f; }
    auto local = [&](uint32_t t) { return t == entry || (!entries.count(t) && img.In(t)); };

    std::vector<uint32_t> work{entry};
    auto jump = [&](uint32_t t) { if (local(t)) { f.labels.insert(t); work.push_back(t); } };
    auto fall = [&](uint32_t t) { if (local(t)) work.push_back(t); };

    while (!work.empty() && f.ok) {
        const uint32_t a = work.back();
        work.pop_back();
        if (!f.starts.insert(a).second) continue;
        const uint32_t w = img.Word(a);
        if (!Supported(w)) { f.ok = false; f.reason = Fail("unsupported word", a); break; }
        if (!OwnsDelaySlot(w)) {
            const uint32_t fn = FunctOf(w);
            if (!(OpOf(w) == 0 && (fn == 0x0C || fn == 0x0D))) fall(a + 4);  // SYSCALL/BREAK leave
            continue;
        }
        if (!img.In(a + 4)) { f.ok = false; f.reason = Fail("delay slot outside the image", a); break; }
        const uint32_t ds = img.Word(a + 4);
        if (OwnsDelaySlot(ds)) { f.ok = false; f.reason = Fail("branch in delay slot", a); break; }
        if (!Supported(ds)) { f.ok = false; f.reason = Fail("unsupported word", a + 4); break; }

        const Insn in = Decode(w, a);
        const uint32_t op = OpOf(w);
        if (op == 0x00 && FunctOf(w) == 0x08) {          // jr
            uint32_t base = 0, count = 0;
            if (RsOf(w) != 31 && FindSwitchTable(img.p, img.size, img.base, a, base, count)) {
                for (uint32_t i = 0; i < count && img.In(base + 4 * i); ++i) {
                    const uint32_t t = img.Word(base + 4 * i);
                    f.tables[a].insert(t);
                    jump(t);
                }
            }
        } else if (in.kind == InsnKind::Branch) {
            jump(in.target);
            fall(a + 8);
        } else if (in.kind == InsnKind::Jump) {
            jump(in.target);
        } else {                                          // jal, bltzal/bgezal, jalr
            fall(a + 8);
        }
    }
    if (!f.ok) return f;

    // Load-delay verifier: the instruction executed after each delayed load must not touch rt.
    for (uint32_t a : f.starts) {
        const uint32_t w = img.Word(a);
        if (!OwnsDelaySlot(w)) {
            if (img.In(a + 4) && LoadDelayHazard(w, img.Word(a + 4))) { f.ok = false; f.reason = Fail("load-delay hazard", a); break; }
            continue;
        }
        const uint32_t ds = img.Word(a + 4);
        std::vector<uint32_t> next;
        const Insn in = Decode(w, a);
        if (in.has_target) next.push_back(in.target);
        if (in.kind == InsnKind::Branch || OpOf(w) == 0x01) next.push_back(a + 8);
        if (f.tables.count(a)) next.insert(next.end(), f.tables[a].begin(), f.tables[a].end());
        for (uint32_t n : next) {
            if (img.In(n) && LoadDelayHazard(ds, img.Word(n))) { f.ok = false; f.reason = Fail("load-delay hazard", a + 4); break; }
        }
        if (!f.ok) break;
    }
    return f;
}

class Emitter {
public:
    Emitter(const Image& img, const std::set<uint32_t>& generated, Func& f)
        : m_img(img), m_gen(generated), m_f(f), m_labels(f.labels) {}

    std::string Body() {
        std::ostringstream os;
        if (*m_f.starts.begin() != m_f.entry) {   // blocks below the entry are emitted first
            m_f.labels.insert(m_f.entry);
            os << "    goto L_" << Addr(m_f.entry) << ";\n";
        }
        for (auto it = m_f.starts.begin(); it != m_f.starts.end(); ++it) {
            const uint32_t a = *it;
            const auto next = std::next(it);
            if (m_f.labels.count(a)) os << "L_" << Addr(a) << ":\n";
            const uint32_t w = m_img.Word(a);
            bool falls = true;
            uint32_t ft = a + 4;
            if (OwnsDelaySlot(w)) {
                ft = a + 8;
                falls = Transfer(os, a);
            } else {
                os << "    // " << Addr(a) << ": " << DescribeInsn(w) << "\n";
                const uint32_t fn = FunctOf(w);
                if (OpOf(w) == 0 && (fn == 0x0C || fn == 0x0D)) {
                    os << "    " << Stmt(a, false) << "\n";   // the exception counts its cycle
                    falls = false;
                } else {
                    os << "    " << Timed(a, false, Hex(a + 4)) << "\n";
                }
            }
            if (!falls) continue;
            if (m_f.starts.count(ft)) {
                if (next == m_f.starts.end() || *next != ft) {
                    m_f.labels.insert(ft);
                    os << "    goto L_" << Addr(ft) << ";\n";
                }
            } else {
                os << "    " << Tail(ft) << "\n";
            }
        }
        return os.str();
    }

private:
    std::string Exc(uint32_t code, uint32_t a, bool bd) const {
        return "{ RaiseException(s, " + Hex(code) + ", " + Hex(a) + ", " + (bd ? "true" : "false") + "); s.cycles += 1; return; }";
    }
    // After an instruction: its cycle, then the host's service point. `next` resumes there;
    // `slot_next` nonzero when `next` is a delay slot (the branch then continues at slot_next).
    static std::string Tick(const std::string& next, const std::string& slot_next = "") {
        return "s.cycles += 1; if (s.cycles >= s.deadline && Stop(s, " + next + (slot_next.empty() ? "" : ", " + slot_next) + ")) return;";
    }
    // A non-transfer instruction and its Tick. After a delayed load (or MFC0/MFC2/CFC2) a stop hands
    // the interpreter the state it would hold: old register value, the new one still in flight.
    // A chain of loads into one register (LWL+LWR): the interpreter supersedes the in-flight load
    // without writing it back, so the register keeps its value from before the first one.
    std::string Timed(uint32_t a, bool bd, const std::string& next) const {
        const uint32_t w = m_img.Word(a);
        if (!RegUse(w).delayed) return Stmt(a, bd) + " " + Tick(next);
        const std::string rt = std::to_string(RtOf(w));
        const std::string stop = " s.cycles += 1; if (s.cycles >= s.deadline && StopLoad(s, " + next + ", " + rt + ", ";
        if (Chained(a)) return "{ " + Stmt(a, bd) + stop + "s.pair_prev)) return; }";
        const std::string save = Chained(a + 4) ? "s.pair_prev = s.gpr[" + rt + "]; " : "";
        return "{ const uint32_t prev = s.gpr[" + rt + "]; " + save + Stmt(a, bd) + stop + "prev)) return; }";
    }
    // True when `x` is only ever reached right after `x - 4` and both are delayed loads into the
    // same register (x - 4 a plain instruction, or the delay slot of a conditional branch).
    bool Chained(uint32_t x) const {
        if (!m_f.starts.count(x) || !m_f.starts.count(x - 4) || m_labels.count(x)) return false;
        const uint32_t p = m_img.Word(x - 4);
        if (OwnsDelaySlot(p)) return false;
        if (m_f.starts.count(x - 8) && OwnsDelaySlot(m_img.Word(x - 8)) &&
            Decode(m_img.Word(x - 8), x - 8).kind != InsnKind::Branch) return false;
        const uint32_t d = RegUse(p).delayed;
        return d != 0 && d == RegUse(m_img.Word(x)).delayed;
    }
    std::string Fn(uint32_t t) const { return "f_" + Addr(t); }
    std::string Call(uint32_t t) const { return m_gen.count(t) ? Fn(t) + "(s);" : "Dispatch(s, " + Hex(t) + ");"; }
    std::string Tail(uint32_t t) const { return m_gen.count(t) ? Fn(t) + "(s); return;" : "Dispatch(s, " + Hex(t) + "); return;"; }
    std::string JumpTo(uint32_t t) const {
        if (m_f.labels.count(t) && m_f.starts.count(t)) return "goto L_" + Addr(t) + ";";
        return "{ " + Tail(t) + " }";
    }

    // Emits a branch/jump with its delay slot. Returns whether execution can fall through to a+8.
    bool Transfer(std::ostringstream& os, uint32_t a) {
        const uint32_t w = m_img.Word(a), ds = m_img.Word(a + 4);
        const uint32_t op = OpOf(w), rs = RsOf(w), rt = RtOf(w), rd = RdOf(w), ret = a + 8;
        const Insn in = Decode(w, a);
        const std::string after = "(taken ? " + Hex(in.target) + " : " + Hex(ret) + ")";
        const std::string done = "if (s.pc != " + Hex(ret) + " || s.stopped) return;";
        os << "    // " << Addr(a) << ": " << DescribeInsn(w) << "\n";
        os << "    // " << Addr(a + 4) << ": " << DescribeInsn(ds) << "  (delay slot)\n";
        os << "    {\n";
        bool falls = true;
        auto slot = [&](const std::string& next) {   // branch cycle, delay slot, its cycle
            os << "        " << Tick(Hex(a + 4), next) << "\n";
            os << "        " << Timed(a + 4, true, next) << "\n";
        };
        auto cond = [&](const std::string& c) {
            os << "        const bool taken = " << c << ";\n";
            slot(after);
            os << "        if (taken) " << JumpTo(in.target) << "\n";
        };
        const std::string S = "static_cast<int32_t>(" + R(rs) + ")";
        switch (op) {
            case 0x00:
                os << "        const uint32_t target = " << R(rs) << ";\n";
                if (FunctOf(w) == 0x09) {                      // jalr
                    os << "        " << Set(rd ? rd : 31, Hex(ret)) << "\n";
                    slot("target");
                    os << "        Dispatch(s, target);\n";
                    os << "        " << done << "\n";
                    break;
                }
                slot("target");                               // jr
                falls = false;
                if (m_f.tables.count(a)) {
                    os << "        switch (target) {\n";
                    for (uint32_t t : m_f.tables[a]) os << "            case " << Hex(t) << ": " << JumpTo(t) << "\n";
                    os << "            default: break;\n        }\n";
                }
                if (rs == 31 && !m_f.tables.count(a)) os << "        s.pc = target;\n        return;\n";
                else os << "        Dispatch(s, target);\n        return;\n";
                break;
            case 0x01:
                if (rt == 0x00 || rt == 0x01) { cond(S + (rt ? " >= 0" : " < 0")); break; }
                os << "        const bool taken = " << S << (rt == 0x11 ? " >= 0" : " < 0") << ";\n";
                os << "        s.gpr[31] = " << Hex(ret) << ";\n";
                slot(after);
                os << "        if (taken) {\n            " << Call(in.target) << "\n";
                os << "            " << done << "\n        }\n";
                break;
            case 0x02:
                slot(Hex(in.target));
                os << "        " << JumpTo(in.target) << "\n";
                falls = false;
                break;
            case 0x03:
                os << "        s.gpr[31] = " << Hex(ret) << ";\n";
                slot(Hex(in.target));
                os << "        " << Call(in.target) << "\n";
                os << "        " << done << "\n";
                break;
            case 0x04: cond(R(rs) + " == " + R(rt)); break;
            case 0x05: cond(R(rs) + " != " + R(rt)); break;
            case 0x06: cond(S + " <= 0"); break;
            default:   cond(S + " > 0"); break;            // 0x07 bgtz
        }
        os << "    }\n";
        return falls;
    }

    // One non-transfer instruction as a single line of C++. `bd`: it sits in a delay slot.
    std::string Stmt(uint32_t a, bool bd) const {
        const uint32_t w = m_img.Word(a);
        const uint32_t op = OpOf(w), rs = RsOf(w), rt = RtOf(w), rd = RdOf(w), sh = ShamtOf(w), fn = FunctOf(w);
        const uint32_t imm = ImmOf(w), simm = static_cast<uint32_t>(SImmOf(w));
        const std::string RS = R(rs), RT = R(rt), SH = std::to_string(sh);
        const std::string ea = RS + " + " + Hex(simm);
        auto i32 = [](const std::string& e) { return "static_cast<int32_t>(" + e + ")"; };
        auto addr_check = [&](uint32_t mask, uint32_t code) {
            return "if (a & " + std::to_string(mask) + "u) { s.badvaddr = a; " + Exc(code, a, bd).substr(2);
        };
        switch (op) {
            case 0x00:
                switch (fn) {
                    case 0x00: return w == 0 ? "/* nop */" : Set(rd, RT + " << " + SH);
                    case 0x02: return Set(rd, RT + " >> " + SH);
                    case 0x03: return Set(rd, "static_cast<uint32_t>(" + i32(RT) + " >> " + SH + ")");
                    case 0x04: return Set(rd, RT + " << (" + RS + " & 31u)");
                    case 0x06: return Set(rd, RT + " >> (" + RS + " & 31u)");
                    case 0x07: return Set(rd, "static_cast<uint32_t>(" + i32(RT) + " >> (" + RS + " & 31u))");
                    case 0x0C: return Exc(0x08, a, bd);
                    case 0x0D: return Exc(0x09, a, bd);
                    case 0x10: return Set(rd, "s.hi");
                    case 0x11: return "s.hi = " + RS + ";";
                    case 0x12: return Set(rd, "s.lo");
                    case 0x13: return "s.lo = " + RS + ";";
                    case 0x18: return "{ const int64_t p = static_cast<int64_t>(" + i32(RS) + ") * " + i32(RT) +
                                      "; s.lo = static_cast<uint32_t>(p); s.hi = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32); }";
                    case 0x19: return "{ const uint64_t p = static_cast<uint64_t>(" + RS + ") * " + RT +
                                      "; s.lo = static_cast<uint32_t>(p); s.hi = static_cast<uint32_t>(p >> 32); }";
                    case 0x1A: return "Div(s, " + RS + ", " + RT + ");";
                    case 0x1B: return "Divu(s, " + RS + ", " + RT + ");";
                    case 0x20: return "{ const uint32_t x = " + RS + ", y = " + RT + ", r = x + y; if (~(x ^ y) & (x ^ r) & 0x80000000u) " +
                                      Exc(0x0C, a, bd) + " " + Set(rd, "r") + " }";
                    case 0x21: return Set(rd, RS + " + " + RT);
                    case 0x22: return "{ const uint32_t x = " + RS + ", y = " + RT + ", r = x - y; if ((x ^ y) & (x ^ r) & 0x80000000u) " +
                                      Exc(0x0C, a, bd) + " " + Set(rd, "r") + " }";
                    case 0x23: return Set(rd, RS + " - " + RT);
                    case 0x24: return Set(rd, RS + " & " + RT);
                    case 0x25: return Set(rd, RS + " | " + RT);
                    case 0x26: return Set(rd, RS + " ^ " + RT);
                    case 0x27: return Set(rd, "~(" + RS + " | " + RT + ")");
                    case 0x2A: return Set(rd, "(" + i32(RS) + " < " + i32(RT) + ") ? 1u : 0u");
                    default:   return Set(rd, "(" + RS + " < " + RT + ") ? 1u : 0u");   // 0x2B sltu
                }
            case 0x08: return "{ const uint32_t x = " + RS + ", r = x + " + Hex(simm) + "; if (~(x ^ " + Hex(simm) +
                              ") & (x ^ r) & 0x80000000u) " + Exc(0x0C, a, bd) + " " + Set(rt, "r") + " }";
            case 0x09: return Set(rt, RS + " + " + Hex(simm));
            case 0x0A: return Set(rt, "(" + i32(RS) + " < " + std::to_string(SImmOf(w)) + ") ? 1u : 0u");
            case 0x0B: return Set(rt, "(" + RS + " < " + Hex(simm) + ") ? 1u : 0u");
            case 0x0C: return Set(rt, RS + " & " + Hex(imm));
            case 0x0D: return Set(rt, RS + " | " + Hex(imm));
            case 0x0E: return Set(rt, RS + " ^ " + Hex(imm));
            case 0x0F: return Set(rt, Hex(imm << 16));
            case 0x10:                                                     // COP0
                if (rs == 0x00) {
                    if (rd == 8) return Set(rt, "s.badvaddr");
                    if (rd == 12) return Set(rt, "s.sr");
                    if (rd == 13) return Set(rt, "s.cause");
                    if (rd == 14) return Set(rt, "s.epc");
                    return "/* mfc0 of an unmodelled register: rt keeps its value */";
                }
                if (rs == 0x04) {
                    // SR/CAUSE writes can unmask a pending IRQ: deadline 0 forces the service point.
                    if (rd == 12) return "s.sr = " + RT + "; s.deadline = 0;";
                    if (rd == 13) return "s.cause = (s.cause & ~0x300u) | (" + RT + " & 0x300u); s.deadline = 0;";
                    if (rd == 14) return "s.epc = " + RT + ";";
                    return "/* mtc0 to an unmodelled register */";
                }
                if (fn == 0x10 && (w & 0x02000000u)) return "s.sr = (s.sr & ~0x0Fu) | ((s.sr >> 2) & 0x0Fu); s.deadline = 0; /* rfe */";
                return "/* cop0: no-op in the interpreter */";
            case 0x12:                                                     // COP2 (GTE)
                if (w & 0x02000000u) return "s.gte->ExecuteCommand(" + Hex(w) + ");";
                if (rs == 0x00) return Set(rt, "s.gte->ReadDataRegister(" + std::to_string(rd) + ")");
                if (rs == 0x02) return Set(rt, "s.gte->ReadControlRegister(" + std::to_string(rd) + ")");
                if (rs == 0x04) return "s.gte->WriteDataRegister(" + std::to_string(rd) + ", " + RT + ");";
                if (rs == 0x06) return "s.gte->WriteControlRegister(" + std::to_string(rd) + ", " + RT + ");";
                return "/* cop2: no-op in the interpreter */";
            case 0x20: return Set(rt, "static_cast<uint32_t>(static_cast<int8_t>(Read(s, " + ea + ", 1)))");
            case 0x24: return Set(rt, "static_cast<uint32_t>(static_cast<uint8_t>(Read(s, " + ea + ", 1)))");
            case 0x21: return "{ const uint32_t a = " + ea + "; " + addr_check(1, 0x04) + " " +
                              Set(rt, "static_cast<uint32_t>(static_cast<int16_t>(Read(s, a, 2)))") + " }";
            case 0x25: return "{ const uint32_t a = " + ea + "; " + addr_check(1, 0x04) + " " +
                              Set(rt, "static_cast<uint32_t>(static_cast<uint16_t>(Read(s, a, 2)))") + " }";
            case 0x23: return "{ const uint32_t a = " + ea + "; " + addr_check(3, 0x04) + " " + Set(rt, "Read(s, a, 4)") + " }";
            case 0x22: return "{ const uint32_t a = " + ea + ", sh = (a & 3u) * 8, m = Read(s, a & ~3u, 4); " +
                              Set(rt, "(" + RT + " & (0x00FFFFFFu >> sh)) | (m << (24 - sh))") + " }";
            case 0x26: return "{ const uint32_t a = " + ea + ", sh = (a & 3u) * 8, m = Read(s, a & ~3u, 4); " +
                              Set(rt, "(" + RT + " & (sh == 0 ? 0u : (0xFFFFFF00u << (24 - sh)))) | (m >> sh)") + " }";
            case 0x28: return "Write(s, " + ea + ", " + RT + " & 0xFFu, 1);";
            case 0x29: return "{ const uint32_t a = " + ea + "; " + addr_check(1, 0x05) + " Write(s, a, " + RT + " & 0xFFFFu, 2); }";
            case 0x2B: return "{ const uint32_t a = " + ea + "; " + addr_check(3, 0x05) + " Write(s, a, " + RT + ", 4); }";
            case 0x2A: return "{ const uint32_t a = " + ea + ", sh = (a & 3u) * 8, m = Read(s, a & ~3u, 4); " +
                              "Write(s, a & ~3u, (m & (0xFFFFFF00u << sh)) | (" + RT + " >> (24 - sh)), 4); }";
            case 0x2E: return "{ const uint32_t a = " + ea + ", sh = (a & 3u) * 8, m = Read(s, a & ~3u, 4); " +
                              "Write(s, a & ~3u, (m & (sh == 0 ? 0u : (0x00FFFFFFu >> (24 - sh)))) | (" + RT + " << sh), 4); }";
            case 0x32: return "s.gte->WriteDataRegister(" + std::to_string(rt) + ", Read(s, " + ea + ", 4));";
            default:   return "Write(s, " + ea + ", s.gte->ReadDataRegister(" + std::to_string(rt) + "), 4);";  // 0x3A swc2
        }
    }

    const Image& m_img;
    const std::set<uint32_t>& m_gen;
    Func& m_f;
    const std::set<uint32_t> m_labels;   // branch targets and resume points, before Body() adds gotos
};

} // namespace

CodegenResult GenerateCpp(const uint8_t* image, size_t size, uint32_t base,
                          const std::vector<uint32_t>& entries, const std::string& module,
                          const std::vector<uint32_t>& exclude) {
    const Image img{image, size, base};
    const std::set<uint32_t> excluded(exclude.begin(), exclude.end());
    const std::set<uint32_t> entry_set(entries.begin(), entries.end());

    std::vector<Func> funcs;
    std::set<uint32_t> generated;
    for (uint32_t e : entry_set) {
        funcs.push_back(Analyze(img, e, entry_set));
        if (excluded.count(e)) { funcs.back().ok = false; funcs.back().reason = "excluded"; }
        if (funcs.back().ok) generated.insert(e);
    }

    CodegenResult res;
    std::ostringstream os;
    os << "// Generated by tools/recomp_gen (PersonaRM static recompiler v0). Do not edit.\n"
       << "// Image base " << Hex(base) << ", " << size << " bytes, " << entry_set.size() << " entries, "
       << generated.size() << " generated.\n";
    for (const Func& f : funcs) {
        if (!f.ok) os << "// not generated: " << Addr(f.entry) << " (" << f.reason << ")\n";
    }
    os << "\n#include \"recomp/runtime/recomp_runtime.h\"\n\nnamespace {\n\nusing namespace personarm::recomp;\n\n";
    for (uint32_t e : generated) os << "void f_" << Addr(e) << "(CpuState& s);\n";
    std::vector<std::pair<uint32_t, uint32_t>> resume_table;   // (address, function entry)
    for (Func& f : funcs) {
        GeneratedFunction g;
        g.entry = f.entry;
        g.generated = f.ok;
        g.reason = f.reason;
        g.instructions = static_cast<uint32_t>(f.starts.size());
        if (!f.ok) { res.functions.push_back(g); continue; }
        // Resume points: where the interpreter lands after this function was unwound (a call's
        // return address) or inside its loops (branch targets), so it can re-enter native code
        // mid-function instead of interpreting the rest of it.
        std::set<uint32_t> resume = f.labels;
        for (uint32_t a : f.starts) {
            const uint32_t w = img.Word(a);
            const bool call = OpOf(w) == 0x03 || (OpOf(w) == 0 && FunctOf(w) == 0x09) || (OpOf(w) == 0x01 && (RtOf(w) & 0x10));
            if (call) resume.insert(a + 8);
        }
        for (auto it = resume.begin(); it != resume.end();) {
            if (*it == f.entry || !f.starts.count(*it)) it = resume.erase(it);
            else ++it;
        }
        f.labels.insert(resume.begin(), resume.end());
        g.resume_points = static_cast<uint32_t>(resume.size());
        res.functions.push_back(g);
        const std::string F = Addr(f.entry);
        const std::string body = Emitter(img, generated, f).Body();
        if (resume.empty()) {
            os << "\nvoid f_" << F << "(CpuState& s) {\n" << body << "}\n";
            continue;
        }
        os << "\nstatic void b_" << F << "(CpuState& s, uint32_t at) {\n    switch (at) {\n";
        for (uint32_t r : resume) {
            os << "        case " << Hex(r) << ": goto L_" << Addr(r) << ";\n";
            resume_table.push_back({r, f.entry});
        }
        os << "        default: break;\n    }\n" << body << "}\n";
        os << "void f_" << F << "(CpuState& s) { b_" << F << "(s, 0); }\n";
        os << "void r_" << F << "(CpuState& s) { b_" << F << "(s, s.pc); }\n";
    }
    os << "\n} // namespace\n\nextern const personarm::recomp::FunctionEntry kRecomp_" << module << "[] = {\n";
    for (uint32_t e : generated) os << "    {" << Hex(e) << ", f_" << Addr(e) << "},\n";
    if (generated.empty()) os << "    {0u, nullptr},\n";
    os << "};\nextern const size_t kRecomp_" << module << "_count = " << generated.size() << ";\n";
    os << "extern const personarm::recomp::FunctionEntry kRecomp_" << module << "_resume[] = {\n";
    for (const auto& r : resume_table) os << "    {" << Hex(r.first) << ", r_" << Addr(r.second) << "},\n";
    if (resume_table.empty()) os << "    {0u, nullptr},\n";
    os << "};\n";
    // Translated words (instructions and their delay slots) as ranges, with their values: the host
    // keys the module by these alone (the image's data may change while the code stays valid).
    std::set<uint32_t> code;
    for (const Func& f : funcs) {
        if (!f.ok) continue;
        for (uint32_t a : f.starts) {
            code.insert(a);
            if (OwnsDelaySlot(img.Word(a))) code.insert(a + 4);
        }
    }
    std::vector<uint32_t> ranges;
    for (uint32_t a : code) {
        if (!ranges.empty() && ranges.back() == a) ranges.back() = a + 4;
        else { ranges.push_back(a); ranges.push_back(a + 4); }
    }
    os << "static const uint32_t kCode[] = {";
    for (size_t i = 0; i < ranges.size(); ++i) os << (i % 8 ? " " : "\n    ") << Hex(ranges[i]) << ",";
    if (ranges.empty()) os << "0u";
    os << "\n};\nstatic const uint32_t kWords[] = {";
    size_t n = 0;
    for (uint32_t a : code) os << (n++ % 8 ? " " : "\n    ") << Hex(img.Word(a)) << ",";
    if (code.empty()) os << "0u";
    os << "\n};\n";
    Module m{};
    m.base = base;
    m.code = ranges.data();
    m.code_count = ranges.size() / 2;
    char hash[24], code_hash[24];
    std::snprintf(hash, sizeof hash, "0x%016llXull", static_cast<unsigned long long>(Fnv1a64(image, size)));
    std::snprintf(code_hash, sizeof code_hash, "0x%016llXull", static_cast<unsigned long long>(CodeHash(m, image)));
    os << "extern const personarm::recomp::Module kRecompModule_" << module << " = {\"" << module << "\", " << Hex(base)
       << ", " << size << "u, " << hash << ", kRecomp_" << module << ", " << generated.size() << ", kRecomp_" << module
       << "_resume, " << resume_table.size() << ", kCode, " << m.code_count << ", kWords, " << code_hash << "};\n";
    res.source = os.str();
    return res;
}

} // namespace personarm::recomp
