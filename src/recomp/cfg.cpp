#include "cfg.h"

#include "mips_decode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace personarm::recomp {

namespace {

constexpr size_t kMaxSites = 64;

uint32_t LoadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void Record(std::vector<uint32_t>& list, uint32_t addr) {
    if (list.size() < kMaxSites) list.push_back(addr);
}

constexpr size_t kContextSites = 4;
constexpr uint32_t kContextBefore = 8;


// Register tracking for the straight-line code before a site (WP-14, step 2b).
//
// A Val describes what is known about a register:
//   k    the register holds a known constant part `v` (unknown otherwise)
//   var  the register also holds a variable index part, scaled from register `origin`
// Unknown is (!k && !var). Index parts come from `sll`, so `sll $2,$3,2` yields {var, origin 3}.
struct Val {
    bool k = false;
    bool var = false;
    uint8_t origin = 0;
    uint32_t v = 0;
};

using RegFile = std::array<Val, 32>;

// Apply one instruction to the register file. Anything not modelled makes its destination unknown.
void Apply(RegFile& R, uint32_t w) {
    const uint32_t op = w >> 26;
    const uint32_t rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    const uint32_t funct = w & 63;
    const uint32_t imm = w & 0xFFFF;
    const uint32_t simm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm)));
    auto set = [&](uint32_t reg, Val val) { if (reg != 0) R[reg] = val; };
    auto unknown = Val{};

    if (op == 0x00) {
        switch (funct) {
            case 0x00: {                                       // sll
                const Val s = R[rt];
                if (!s.var && s.k) set(rd, Val{true, false, 0, s.v << sa});
                else set(rd, Val{true, true, s.var ? s.origin : static_cast<uint8_t>(rt), 0});
                return;
            }
            case 0x21: case 0x20: {                            // addu / add
                const Val a = R[rs], b = R[rt];
                if (!(a.k || a.var) || !(b.k || b.var)) { set(rd, unknown); return; }
                if (a.var && b.var) { set(rd, unknown); return; }
                set(rd, Val{a.k && b.k, a.var || b.var, a.var ? a.origin : b.origin, a.v + b.v});
                return;
            }
            case 0x25: {                                       // or
                const Val a = R[rs], b = R[rt];
                if (a.k && !a.var && b.k && !b.var) set(rd, Val{true, false, 0, a.v | b.v});
                else set(rd, unknown);
                return;
            }
            default:
                if (rd != 0) R[rd] = unknown;
                return;
        }
    }
    switch (op) {
        case 0x01:                                             // REGIMM: bltzal / bgezal write $31
            if (rt == 0x10 || rt == 0x11) set(31, unknown);
            return;
        case 0x03: set(31, unknown); return;                   // jal
        case 0x08: case 0x09:                                  // addi / addiu
            if (R[rs].k && !R[rs].var) set(rt, Val{true, false, 0, R[rs].v + simm});
            else if (R[rs].var) set(rt, Val{R[rs].k, true, R[rs].origin, R[rs].v + simm});
            else set(rt, unknown);
            return;
        case 0x0C:                                             // andi
            if (R[rs].k && !R[rs].var) set(rt, Val{true, false, 0, R[rs].v & imm});
            else set(rt, unknown);
            return;
        case 0x0D:                                             // ori
            if (R[rs].k && !R[rs].var) set(rt, Val{true, false, 0, R[rs].v | imm});
            else set(rt, unknown);
            return;
        case 0x0F: set(rt, Val{true, false, 0, imm << 16}); return;   // lui
        case 0x0A: case 0x0B: case 0x0E:                       // slti / sltiu / xori
            set(rt, unknown);
            return;
        case 0x10: case 0x12:                                  // COP0/COP2: mfc/cfc write rt
            if (rs == 0 || rs == 2) set(rt, unknown);
            return;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
            set(rt, unknown);                                  // loads
            return;
        default:
            return;                                            // branches, stores, j, cop loads
    }
}

bool IsUncondTransfer(uint32_t w) {
    const uint32_t op = w >> 26;
    if (op == 0x02 || op == 0x03) return true;                 // j / jal
    return op == 0x00 && ((w & 63) == 0x08 || (w & 63) == 0x09);   // jr / jalr
}

// The register state just before `site`, built forward from the start of its straight-line
// region. The region begins after the last unconditional transfer (skipping its delay slot) and is
// capped at kRegionWords so the walk stays local.
constexpr uint32_t kRegionWords = 96;

struct Region {
    uint32_t start = 0;
};

Region RegionBefore(const uint8_t* code, size_t size, uint32_t load, uint32_t site) {
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    Region reg;
    reg.start = site >= load + 4 * kRegionWords ? site - 4 * kRegionWords : load;
    for (uint32_t a = site; a > reg.start; a -= 4) {
        const uint32_t prev = a - 4;
        if (prev < load || static_cast<uint64_t>(prev) + 4 > static_cast<uint64_t>(load) + size) break;
        if (IsUncondTransfer(word_at(prev))) {
            reg.start = prev + 8;                               // skip the transfer and its delay slot
            break;
        }
    }
    return reg;
}

RegFile StateBefore(const uint8_t* code, size_t size, uint32_t load, uint32_t site) {
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    RegFile R{};
    R[0] = Val{true, false, 0, 0};
    const Region reg = RegionBefore(code, size, load, site);
    for (uint32_t a = reg.start; a < site; a += 4) Apply(R, word_at(a));
    return R;
}

// Switch-table recovery for `jr rX` (WP-14, step 2). Pattern handled:
//   sltiu t, idx, N ; beq t, zero, default       (bound: idx < N)
//   sll   s, idx, 2 ; lui b, hi ; addu b, b, s   (base register holds table base + scaled index)
//   lw    rX, off(b)                             (off may be non-zero; base may come from addiu)
// The table base is (hi << 16) + addiu constant + load offset. Anything not matched stays unresolved.
struct JumpTable {
    uint32_t base = 0;
    uint32_t count = 0;
};

bool FindJumpTable(const uint8_t* code, size_t size, uint32_t load, uint32_t jr_addr, JumpTable& out) {
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    const Region reg = RegionBefore(code, size, load, jr_addr);

    // The last writer of the jump register must be the lw that loads the case address.
    const uint32_t jr_reg = (word_at(jr_addr) >> 21) & 31;
    uint32_t lw_addr = 0;
    bool found = false;
    for (uint32_t a = jr_addr; a > reg.start && !found; a -= 4) {
        const uint32_t w = word_at(a - 4);
        const uint32_t op = w >> 26;
        const uint32_t rt = (w >> 16) & 31;
        if (op == 0x23 && rt == jr_reg) { lw_addr = a - 4; found = true; break; }
        // any other writer of jr_reg means the target is not a plain table load
        if (op == 0x00 && ((w >> 11) & 31) == jr_reg && (w & 63) != 0x08) return false;
        const bool writes_rt = (op >= 0x08 && op <= 0x0F) || (op >= 0x20 && op <= 0x26);
        if (writes_rt && rt == jr_reg) return false;
    }
    if (!found) return false;

    // The load address: base register state just before the lw.
    const uint32_t lw_word = word_at(lw_addr);
    const RegFile before = StateBefore(code, size, load, lw_addr);
    const Val b = before[(lw_word >> 21) & 31];
    if (!b.k || !b.var) return false;                          // need a constant base and an index part
    const uint32_t base = b.v + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(lw_word & 0xFFFF)));

    // The bound: the last sltiu on the index register before the lw.
    const uint8_t index = b.origin;
    uint32_t count = 0;
    bool bound = false;
    for (uint32_t a = lw_addr; a > reg.start && !bound; a -= 4) {
        const uint32_t w = word_at(a - 4);
        if ((w >> 26) == 0x0B && ((w >> 21) & 31) == index) {
            count = w & 0xFFFF;
            bound = true;
        }
    }
    if (!bound || count == 0 || count > 4096) return false;

    out.base = base;
    out.count = count;
    return true;
}

// Records the instructions before an unresolved site, as text, for local inspection.
void CaptureContext(const uint8_t* code, size_t size, uint32_t load, uint32_t site, CfgReport& r) {
    if (r.unresolved_contexts.size() >= kContextSites) return;
    SiteContext ctx;
    ctx.site = site;
    const uint32_t first = site >= load + 4 * kContextBefore ? site - 4 * kContextBefore : load;
    for (uint32_t a = first; a <= site + 4 && a + 4 <= load + size; a += 4) {
        const uint32_t w = LoadU32(code + (a - load));
        char line[160];
        std::snprintf(line, sizeof line, "0x%08X  %s%s", a, DescribeInsn(w).c_str(), a == site ? "   <- sin resolver" : "");
        ctx.window.emplace_back(line);
    }
    r.unresolved_contexts.push_back(std::move(ctx));
}


// True when `w` writes general register `reg` (conservative for ops not modelled elsewhere).
bool WritesReg(uint32_t w, uint32_t reg) {
    if (reg == 0) return false;
    const uint32_t op = w >> 26;
    const uint32_t rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    if (op == 0x00) {
        const uint32_t funct = w & 63;
        if (funct == 0x08 || funct == 0x0C || funct == 0x0D) return false;   // jr, syscall, break
        if (funct >= 0x18 && funct <= 0x1B) return false;                    // mult/div: HI/LO only
        return rd == reg;
    }
    if (op == 0x03) return reg == 31;                                        // jal
    if (op == 0x01) return (rt == 0x10 || rt == 0x11) && reg == 31;          // bltzal / bgezal
    if ((op >= 0x08 && op <= 0x0F) || (op >= 0x20 && op <= 0x26)) return rt == reg;
    return false;
}

constexpr size_t kPointerSamples = 8;
constexpr size_t kSamplesPerStatus = 4;
constexpr size_t kMaxCandidates = 8;

// Constant values written to each address by `sw` with a constant base (WP-14, step 3b).
struct SlotStores {
    std::vector<uint32_t> values;   // constant values written
    bool unknown = false;           // at least one store wrote a value that is not a constant
};
using StoreTable = std::map<uint32_t, SlotStores>;

// One pass over the image: every `sw` with a constant base contributes its stored constant.
StoreTable CollectStores(const uint8_t* code, size_t size, uint32_t load) {
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    StoreTable table;
    for (uint32_t a = load; static_cast<uint64_t>(a) + 4 <= static_cast<uint64_t>(load) + size; a += 4) {
        const uint32_t w = word_at(a);
        if ((w >> 26) != 0x2B) continue;                              // sw
        const RegFile R = StateBefore(code, size, load, a);
        const Val base = R[(w >> 21) & 31];
        if (!(base.k && !base.var)) continue;
        const uint32_t addr = base.v + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w & 0xFFFF)));
        const Val val = R[(w >> 16) & 31];
        SlotStores& st = table[addr];
        if (val.k && !val.var) {
            if (st.values.size() < kMaxCandidates) st.values.push_back(val.v);
        } else {
            st.unknown = true;
        }
    }
    return table;
}

// Classifies the function pointer that a `jalr` on `reg` calls through, when it was loaded by
// `lw off(base)` from a constant base (WP-14, step 3). Only counts and a few samples are kept.
void ClassifyPointer(const uint8_t* code, size_t size, uint32_t load, uint32_t site, uint32_t reg,
                     const StoreTable& stores, CfgReport& r) {
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    auto in_image = [&](uint32_t a) {
        return a >= load && static_cast<uint64_t>(a) + 4 <= static_cast<uint64_t>(load) + size && ((a - load) & 3u) == 0;
    };

    PointerSite ps;
    ps.site = site;
    const Region region = RegionBefore(code, size, load, site);
    bool found = false;
    uint32_t writer = 0;
    for (uint32_t a = site; a > region.start; a -= 4) {
        if (WritesReg(word_at(a - 4), reg)) { writer = a - 4; found = true; break; }
    }

    if (!found || (word_at(writer) >> 26) != 0x23) {
        ps.status = PointerStatus::Unrecognised;
        ++r.pointers.unrecognised;
    } else {
        const uint32_t lw = word_at(writer);
        const Val base = StateBefore(code, size, load, writer)[(lw >> 21) & 31];
        if (!(base.k && !base.var)) {
            ps.status = PointerStatus::Unrecognised;
            ++r.pointers.unrecognised;
        } else {
            const uint32_t addr = base.v + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(lw & 0xFFFF)));
            ps.address = addr;
            if (!in_image(addr)) {
                ps.status = PointerStatus::AddressOutside;
                ++r.pointers.address_outside;
            } else {
                ps.value = word_at(addr);
                if (ps.value == 0) {
                    const auto it = stores.find(addr);
                    if (it != stores.end()) {
                        for (uint32_t v : it->second.values) {
                            if (in_image(v) && ps.candidates.size() < kMaxCandidates) ps.candidates.push_back(v);
                        }
                    }
                    if (!ps.candidates.empty()) {
                        ps.status = PointerStatus::AssignedInImage;
                        ++r.pointers.assigned_in_image;
                    } else {
                        ps.status = PointerStatus::Null;
                        ++r.pointers.null_value;
                    }
                } else if (in_image(ps.value)) {
                    ps.status = PointerStatus::InImage;
                    ++r.pointers.in_image_value;
                } else {
                    ps.status = PointerStatus::ValueOutside;
                    ++r.pointers.value_outside;
                }
            }
        }
    }
    size_t same = 0;
    for (const auto& q : r.pointer_samples) same += q.status == ps.status;
    if (r.pointer_samples.size() < kPointerSamples && same < kSamplesPerStatus) r.pointer_samples.push_back(ps);
}

} // namespace

CfgReport AnalyzeControlFlow(const uint8_t* code, size_t size, uint32_t load, uint32_t entry) {
    CfgReport r;
    const size_t words = size / 4;
    r.text_words = static_cast<uint32_t>(words);
    const uint64_t image_end = static_cast<uint64_t>(load) + static_cast<uint64_t>(words) * 4;

    auto in_image = [&](uint32_t a) {
        return a >= load && static_cast<uint64_t>(a) < image_end && ((a - load) & 3u) == 0;
    };
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    auto index_of = [&](uint32_t a) { return static_cast<size_t>((a - load) / 4); };

    const StoreTable stores = CollectStores(code, size, load);

    std::vector<bool> visited(words, false);
    std::set<uint32_t> functions;
    std::vector<uint32_t> work;

    auto push_target = [&](uint32_t from, uint32_t target) {
        if (!in_image(target)) {
            ++r.out_of_range_targets;
            Record(r.out_of_range_sites, from);
            return;
        }
        work.push_back(target);
    };

    if (in_image(entry)) {
        functions.insert(entry);
        work.push_back(entry);
    } else {
        ++r.out_of_range_targets;
        Record(r.out_of_range_sites, entry);
    }

    while (!work.empty()) {
        uint32_t pc = work.back();
        work.pop_back();

        bool block_open = true;
        while (block_open) {
            if (!in_image(pc)) {                // ran off the end of the image
                ++r.out_of_range_targets;
                Record(r.out_of_range_sites, pc);
                break;
            }
            if (visited[index_of(pc)]) break;   // already explored from another path
            visited[index_of(pc)] = true;
            ++r.reachable_words;

            const Insn in = Decode(word_at(pc), pc);
            if (in.kind == InsnKind::Invalid) {
                ++r.invalid_reached;
                Record(r.invalid_sites, pc);
                break;
            }
            if (!in.has_delay_slot) {
                if (in.kind == InsnKind::Syscall) ++r.syscalls;
                pc += 4;
                continue;
            }

            // Control transfer: its delay slot always executes.
            const uint32_t ds = pc + 4;
            if (!in_image(ds)) {
                ++r.out_of_range_targets;
                Record(r.out_of_range_sites, pc);
                break;
            }
            if (!visited[index_of(ds)]) {
                visited[index_of(ds)] = true;
                ++r.reachable_words;
                if (Decode(word_at(ds), ds).kind == InsnKind::Invalid) {
                    ++r.invalid_reached;
                    Record(r.invalid_sites, ds);
                }
            }

            const uint32_t after = pc + 8;
            switch (in.kind) {
                case InsnKind::Branch:
                    ++r.conditional_branches;
                    push_target(pc, in.target);
                    pc = after;
                    break;
                case InsnKind::Jump:
                    ++r.direct_jumps;
                    push_target(pc, in.target);
                    block_open = false;
                    break;
                case InsnKind::Call:
                    ++r.direct_calls;
                    if (in_image(in.target)) {
                        functions.insert(in.target);
                        work.push_back(in.target);
                    } else {
                        ++r.out_of_range_targets;
                        Record(r.out_of_range_sites, pc);
                    }
                    pc = after;
                    break;
                case InsnKind::IndirectJump: {
                    {
                        const Val tv = StateBefore(code, size, load, pc)[in.rs];
                        if (tv.k && !tv.var && (tv.v == 0xA0 || tv.v == 0xB0 || tv.v == 0xC0)) {
                            ++r.bios_calls;                    // BIOS vector: returns after the delay slot
                            pc = after;
                            break;
                        }
                    }
                    ++r.indirect_jumps;
                    JumpTable jt;
                    uint32_t read = 0;
                    bool table_ok = FindJumpTable(code, size, load, pc, jt);
                    if (table_ok) {
                        for (uint32_t i = 0; i < jt.count; ++i) {
                            const uint32_t entry_addr = jt.base + 4 * i;
                            if (!in_image(entry_addr)) break;
                            const uint32_t target = word_at(entry_addr);
                            if (!in_image(target)) {
                                ++r.out_of_range_targets;
                                Record(r.out_of_range_sites, pc);
                                break;
                            }
                            ++read;
                            functions.insert(target);  // table entries are case blocks in this image
                            work.push_back(target);
                        }
                    }
                    if (read > 0) {
                        ++r.resolved_jump_tables;
                        r.resolved_table_entries += read;
                    } else {
                        ++r.unresolved_jumps;
                        Record(r.indirect_sites, pc);
                        CaptureContext(code, size, load, pc, r);
                    }
                    block_open = false;
                    break;
                }
                case InsnKind::IndirectCall:
                    ++r.indirect_calls;
                    Record(r.indirect_sites, pc);
                    ClassifyPointer(code, size, load, pc, in.rs, stores, r);
                    CaptureContext(code, size, load, pc, r);
                    pc = after;
                    break;
                case InsnKind::Return:
                    ++r.returns;
                    block_open = false;
                    break;
                default:
                    block_open = false;
                    break;
            }
        }
    }

    r.indirect_calls_assigned = r.pointers.assigned_in_image;
    r.indirect_calls_unresolved = r.indirect_calls - r.indirect_calls_assigned;

    r.function_entries.assign(functions.begin(), functions.end());
    r.functions = static_cast<uint32_t>(functions.size());
    if (words > 0) {
        r.coverage_permille = static_cast<uint32_t>(static_cast<uint64_t>(r.reachable_words) * 1000 / words);
    }
    return r;
}

} // namespace personarm::recomp
