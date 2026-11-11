#pragma once

// Control-flow reachability over a PS-X EXE payload (WP-14, first step).
//
// Starting from the entry point, follows direct branches, jumps and calls (including their
// delay slots) and records everything it cannot follow: indirect jumps, reserved words that
// are reached as code, and targets outside the loaded image. Coverage is the share of the
// image's words that are reachable this way.
//
// Limits: no data-flow, so jump tables and function pointers are not resolved; code that is
// loaded later (overlays) is not visible in one image.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace personarm::recomp {

struct CfgReport {
    uint32_t text_words = 0;
    uint32_t reachable_words = 0;       // distinct words reached, delay slots included
    uint32_t coverage_permille = 0;     // reachable_words * 1000 / text_words
    uint32_t functions = 0;             // entry point + distinct in-image call targets
    uint32_t direct_calls = 0;
    uint32_t direct_jumps = 0;
    uint32_t conditional_branches = 0;
    uint32_t returns = 0;
    uint32_t indirect_jumps = 0;
    uint32_t indirect_calls = 0;
    uint32_t syscalls = 0;
    uint32_t invalid_reached = 0;       // reserved encodings reached as code
    uint32_t out_of_range_targets = 0;  // branch/call/fall-through targets outside the image
    uint32_t resolved_jump_tables = 0;  // indirect jumps whose switch table was decoded
    uint32_t resolved_table_entries = 0; // in-image entries read from those tables
    std::vector<uint32_t> function_entries;     // sorted, in-image
    std::vector<uint32_t> indirect_sites;       // addresses of unresolved indirect jumps/calls
    std::vector<uint32_t> invalid_sites;        // addresses of reserved words reached
    std::vector<uint32_t> out_of_range_sites;   // addresses of the instructions that point outside
};

/// `code` is the payload as loaded at `load_address`; `entry` is the initial PC.
CfgReport AnalyzeControlFlow(const uint8_t* code, size_t size, uint32_t load_address, uint32_t entry);

} // namespace personarm::recomp
