#include "cfg.h"

#include "mips_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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


// Switch-table recovery for `jr rX` (WP-14, step 2). Works backwards inside the straight-line
// block that ends at the jump, looking for the pattern a compiler emits for a switch:
//   sltiu t, idx, N ; beq t, zero, default      (bound check: index < N)
//   sll  s, idx, 2  ; lui b, hi ; addiu b, b, lo (table base)
//   addu  s, s, b   ; lw rX, 0(s)               (load the case address)
// Returns the table entries that lie inside the image, or an empty list when the pattern is not
// complete. It never guesses: a missing piece means the site stays unresolved.
struct JumpTable {
    uint32_t base = 0;
    uint32_t count = 0;
};

bool FindJumpTable(const uint8_t* code, size_t size, uint32_t load, uint32_t jr_addr, JumpTable& out) {
    auto word_at = [&](uint32_t a) { return LoadU32(code + (a - load)); };
    auto in_code = [&](uint32_t a) {
        return a >= load && static_cast<uint64_t>(a) + 4 <= static_cast<uint64_t>(load) + size && ((a - load) & 3u) == 0;
    };

    // Walk back to the start of the straight-line block, never crossing a control transfer.
    uint32_t pc = jr_addr;
    const uint32_t jr_word = word_at(pc);
    const uint32_t jr_reg = (jr_word >> 21) & 31;            // rs of jr

    // Find the lw that loads jr_reg (search backwards, stop at a transfer or a write to it).
    uint32_t lw_addr = 0;
    bool found_lw = false;
    for (uint32_t a = pc; a >= load + 4 && !found_lw; a -= 4) {
        const uint32_t prev = a - 4;
        if (!in_code(prev)) break;
        const uint32_t w = word_at(prev);
        const uint32_t op = w >> 26;
        if (op == 0x00 && (w & 63) == 0x08) break;            // previous jr ends the block
        if (op == 0x02 || op == 0x03 || (op >= 0x04 && op <= 0x07)) break;  // j/jal/branch
        if (op == 0x23 && ((w >> 16) & 31) == jr_reg) {       // lw rX, off(base)
            lw_addr = prev;
            found_lw = true;
        }
    }
    if (!found_lw) return false;

    // The lw address is formed as addu(index_scaled, base): the lw register is the sum, so the
    // table base lives in the other operand of that addu, built by a lui/addiu pair.
    const uint32_t lw_word = word_at(lw_addr);
    const uint32_t lw_base = (lw_word >> 21) & 31;
    const int16_t lw_off = static_cast<int16_t>(lw_word & 0xFFFF);
    if (lw_off != 0) return false;                             // table is read from element 0

    uint32_t addu_addr = 0;
    uint32_t addu_other = 0;                                   // the operand that is not the sum itself
    bool found_addu = false;
    for (uint32_t a = lw_addr; a >= load + 4 && !found_addu; a -= 4) {
        const uint32_t prev = a - 4;
        if (!in_code(prev)) break;
        const uint32_t w = word_at(prev);
        const uint32_t op = w >> 26;
        if (op == 0x00 && (w & 63) == 0x21 && ((w >> 11) & 31) == lw_base) {   // addu lw_base, rs, rt
            addu_addr = prev;
            const uint32_t rs = (w >> 21) & 31;
            const uint32_t rt = (w >> 16) & 31;
            addu_other = (rs == lw_base) ? rt : rs;
            found_addu = true;
        }
    }
    if (!found_addu) return false;

    uint32_t base_hi = 0, base_lo = 0;
    bool have_hi = false, have_lo = false;
    uint32_t sltiu_bound = 0;
    bool have_bound = false;
    for (uint32_t a = addu_addr; a >= load + 4; a -= 4) {
        const uint32_t prev = a - 4;
        if (!in_code(prev)) break;
        const uint32_t w = word_at(prev);
        const uint32_t op = w >> 26;
        const uint32_t rt = (w >> 16) & 31;
        if (op == 0x0F && rt == addu_other && !have_hi) {      // lui base, hi
            base_hi = (w & 0xFFFF) << 16;
            have_hi = true;
        } else if (op == 0x09 && rt == addu_other && !have_lo) { // addiu base, base, lo
            base_lo = static_cast<uint16_t>(w & 0xFFFF);
            have_lo = true;
        } else if (op == 0x0B && !have_bound) {                // sltiu t, idx, N
            sltiu_bound = static_cast<uint16_t>(w & 0xFFFF);
            have_bound = true;
        }
    }
    if (!have_hi || !have_lo || !have_bound || sltiu_bound == 0) return false;

    out.base = base_hi + base_lo;
    out.count = sltiu_bound;
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
                        Record(r.indirect_sites, pc);
                        CaptureContext(code, size, load, pc, r);
                    }
                    block_open = false;
                    break;
                }
                case InsnKind::IndirectCall:
                    ++r.indirect_calls;
                    Record(r.indirect_sites, pc);
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

    r.function_entries.assign(functions.begin(), functions.end());
    r.functions = static_cast<uint32_t>(functions.size());
    if (words > 0) {
        r.coverage_permille = static_cast<uint32_t>(static_cast<uint64_t>(r.reachable_words) * 1000 / words);
    }
    return r;
}

} // namespace personarm::recomp
