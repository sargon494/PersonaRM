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
#include <string>
#include <vector>

namespace personarm::recomp {

/// What a function pointer loaded from memory (`lui`+`lw` then `jalr`) points to.
enum class PointerStatus : uint8_t {
    Null,           // the slot holds zero and no constant store to it was found
    AssignedInImage, // the slot holds zero, but constant stores put addresses inside the image in it
    InImage,        // the slot holds an address inside the image
    ValueOutside,   // the slot holds an address outside the image
    AddressOutside, // the slot itself is outside the image (RAM or BSS)
    Unrecognised,   // the base is not a constant, so the slot cannot be located
};

struct PointerSummary {
    uint32_t null_value = 0;
    uint32_t assigned_in_image = 0;
    uint32_t in_image_value = 0;
    uint32_t value_outside = 0;
    uint32_t address_outside = 0;
    uint32_t unrecognised = 0;
};

struct PointerSite {
    uint32_t site = 0;      // the jalr
    uint32_t address = 0;   // the slot it loads from (0 when unrecognised)
    uint32_t value = 0;     // the word at that slot in the image (0 when not readable)
    PointerStatus status = PointerStatus::Unrecognised;
    std::vector<uint32_t> candidates;   // in-image addresses stored into the slot by constant stores
    bool followable = false;            // exactly one distinct constant stored, in image, no unknown store
};

/// Local diagnostic: the instructions leading up to an unresolved indirect transfer.
struct SiteContext {
    uint32_t site = 0;                      // address of the unresolved jr/jalr
    std::vector<std::string> window;        // the preceding words, then the site, as text
};

struct CfgOptions {
    /// Treat the single constant stored into a function-pointer slot as a function entry.
    /// Off by default: a constant store does not prove the value is a valid function.
    bool follow_assigned_pointers = false;
    /// Resolve a `lw` from a constant address to the constant kept there (the image word, or the one
    /// constant stored to it), so a function pointer reached through a structure pointer in a
    /// global can be located. Off by default: the value in memory may change at run time.
    bool second_level_bases = false;
};

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
    uint32_t indirect_calls = 0;        // all jalr reached
    uint32_t indirect_calls_assigned = 0;   // jalr whose slot has in-image constant stores (candidates known, not followed)
    uint32_t indirect_calls_unresolved = 0; // the rest: indirect_calls - indirect_calls_assigned
    uint32_t indirect_calls_followed = 0;   // assigned jalr whose candidate was followed (option enabled)
    uint32_t followed_candidates = 0;       // distinct candidate functions added by following pointers
    uint32_t syscalls = 0;
    uint32_t invalid_reached = 0;       // reserved encodings reached as code
    uint32_t out_of_range_targets = 0;  // branch/call/fall-through targets outside the image
    uint32_t bios_calls = 0;            // jr to a BIOS vector (0xA0/0xB0/0xC0) with a constant target
    uint32_t resolved_jump_tables = 0;  // indirect jumps whose switch table yielded at least one entry
    uint32_t resolved_table_entries = 0; // in-image entries read from those tables
    std::vector<uint32_t> function_entries;     // sorted, in-image
    std::vector<uint32_t> indirect_sites;       // addresses of unresolved indirect jumps/calls
    uint32_t unresolved_jumps = 0;      // `jr` (not BIOS) with no switch table recovered
    PointerSummary pointers;            // `jalr` targets loaded from memory, by status
    std::vector<PointerSite> pointer_samples;   // first few, for local inspection
    std::vector<SiteContext> unresolved_contexts; // first few unresolved sites, for local inspection
    std::vector<uint32_t> invalid_sites;        // addresses of reserved words reached
    std::vector<uint32_t> out_of_range_sites;   // addresses of the instructions that point outside
};

/// `code` is the payload as loaded at `load_address`; `entry` is the initial PC.
CfgReport AnalyzeControlFlow(const uint8_t* code, size_t size, uint32_t load_address, uint32_t entry,
                             const CfgOptions& options = CfgOptions{});

} // namespace personarm::recomp
