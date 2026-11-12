#pragma once

// psx_analyze — offline analysis of PlayStation disc images (WP-13 of the roadmap).
//
// Produces a reproducible Markdown report with file names, sizes, addresses and counters.
// It NEVER copies game data into the report: only metadata and statistics.
//
// Everything in CodeScan is a HEURISTIC over raw MIPS-I words (no disassembler, no control-flow
// analysis). It is meant to guide decisions (does the game talk to hardware directly? which BIOS
// calls does it use? which files look like code?) and must be confirmed by the real recompiler
// analysis (WP-14).

#include <cstddef>
#include <cstdint>
#include "recomp/cfg.h"
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace personarm::tools {

/// Statistics gathered by a linear scan of little-endian MIPS-I words.
struct CodeScan {
    size_t words = 0;
    uint32_t jr_ra = 0;       // `jr $ra`
    uint32_t prologues = 0;   // `addiu $sp,$sp,-N`
    uint32_t ra_saves = 0;    // `sw $ra,N($sp)`
    /// I/O block name -> number of load/store accesses with a statically known address.
    std::map<std::string, uint32_t> hw_blocks;
    /// Absolute I/O address -> number of accesses.
    std::map<uint32_t, uint32_t> hw_registers;
    /// BIOS call stubs `li $t2,0xA0/B0/C0 ; jr $t2 ; li $t1,N` as (table 'A'/'B'/'C', function).
    std::set<std::pair<char, uint32_t>> bios_calls;
};

/// Scans `size` bytes (trailing bytes that do not fill a word are ignored).
CodeScan ScanMips(const uint8_t* data, size_t size);

/// True when the scan has the density of function prologues expected from real MIPS code.
bool LooksLikeMipsCode(const CodeScan& scan);

/// Name of the hardware block that owns an I/O address (0x1F801000..0x1F802FFF), or "".
std::string HardwareBlockName(uint32_t address);

/// Keywords of well-known SDK libraries found as ASCII text (case-insensitive) -> occurrences.
/// Absence proves nothing: it is a hint, not a signature match.
std::map<std::string, uint32_t> FindSdkHints(const uint8_t* data, size_t size);

struct ExeHeader {
    bool valid = false;
    uint32_t entry = 0;
    uint32_t gp = 0;
    uint32_t load_address = 0;
    uint32_t payload_size = 0;
    uint32_t sp_base = 0;
    uint32_t sp_offset = 0;
    std::string region_marker;   // text stored at header offset 0x4C
};
ExeHeader ParsePsxExeHeader(const uint8_t* data, size_t size);

struct FileReport {
    std::string path;
    uint32_t lba = 0;
    uint32_t size = 0;
    std::string sector_kind;     // "Mode 2 Form 1 datos", "Mode 2 Form 2 audio", "2048 (ISO)", ...
    bool scanned = false;
    bool is_psx_exe = false;
    ExeHeader exe;
    CodeScan code;
    bool code_candidate = false; // not a PS-X EXE but looks like raw MIPS code (overlay candidate)
};

struct ExtensionStats {
    uint32_t files = 0;
    uint64_t bytes = 0;
};

struct DiscReport {
    std::string image_name;      // base name only unless full paths were requested
    std::string format;          // "ISO 9660 (2048)", "BIN/CUE 2352", ...
    uint32_t sector_size = 0;
    uint64_t image_bytes = 0;
    uint64_t total_sectors = 0;
    std::string volume_label;
    std::string crc32_hex;       // only with AnalyzeOptions::hash
    std::string boot_exe;        // as written in SYSTEM.CNF
    std::string system_cnf;      // SYSTEM.CNF text (small, needed to identify the boot file)
    std::string product_id;      // recognised serial, if any
    std::string edition;
    std::string region;
    int disc_number = 0;
    size_t directories = 0;
    size_t skipped_large = 0;    // files over max_scan_bytes that were not scanned
    std::vector<FileReport> files;
    std::map<std::string, ExtensionStats> extensions;
    FileReport boot;             // full analysis of the boot executable
    bool has_boot = false;
    bool has_cfg = false;
    recomp::CfgReport boot_cfg;
    recomp::CfgReport boot_cfg_follow;   // same analysis with the experimental pointer-following option
    std::map<std::string, uint32_t> sdk_hints;
};

struct AnalyzeOptions {
    bool hash = false;                 // CRC32 of the whole image (slow on 700 MB)
    bool full_paths = false;           // print the image path as given instead of its base name
    bool cfg = false;                  // control-flow reachability of the boot executable (WP-14)
    size_t max_scan_bytes = 8u << 20;  // per file; the boot executable is always scanned
};

bool AnalyzeDisc(const std::string& image_path, const AnalyzeOptions& options,
                 DiscReport& out, std::string& error);

std::string RenderMarkdown(const std::vector<DiscReport>& reports);

} // namespace personarm::tools
