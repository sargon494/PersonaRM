#include "cfg.h"

#include "mips_decode.h"

#include <algorithm>
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
                case InsnKind::IndirectJump:
                    ++r.indirect_jumps;
                    Record(r.indirect_sites, pc);
                    block_open = false;
                    break;
                case InsnKind::IndirectCall:
                    ++r.indirect_calls;
                    Record(r.indirect_sites, pc);
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
