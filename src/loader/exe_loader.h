#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "cpu/cpu_context.h"
#include "core/memory_manager.h"

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 PS-X Executable (EXE) Header Format
// ───────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct PsxExeHeader {
    char     magic[8];          // 0x00 "PS-X EXE"
    uint32_t text_offset;       // 0x08 unused (zero)
    uint32_t data_offset;       // 0x0C unused (zero)
    uint32_t initial_pc;        // 0x10 Program entry point (usually 0x80010000)
    uint32_t initial_gp;        // 0x14 Initial $gp value
    uint32_t destination_ram;   // 0x18 RAM destination address (usually 0x80010000)
    uint32_t file_size;         // 0x1C Data size in bytes (excluding 2048-byte header, multiple of 2048)
    uint32_t unused_20[2];      // 0x20-0x27 unused in PSX-SPX (NOT .data addr/size)
    uint32_t memfill_start;     // 0x28 Memfill start address (0 = no fill)
    uint32_t memfill_size;      // 0x2C Memfill size in bytes (0 = no fill)
    uint32_t initial_sp_base;   // 0x30 Initial $sp base address (e.g. 0x801FFF00)
    uint32_t initial_sp_offset; // 0x34 Added to the $sp base
    uint8_t  reserved[20];      // 0x38 Reserved for A(43h) Exec() function
    char     marker[1972];      // 0x4C Region marker / copyright text, padded to 2048 bytes
};
#pragma pack(pop)

static_assert(offsetof(PsxExeHeader, initial_pc) == 0x10 && offsetof(PsxExeHeader, memfill_start) == 0x28 &&
              offsetof(PsxExeHeader, initial_sp_base) == 0x30 && offsetof(PsxExeHeader, marker) == 0x4C,
              "PsxExeHeader field offsets must match PSX-SPX");
static_assert(sizeof(PsxExeHeader) == 2048, "PsxExeHeader size must be exactly 2048 bytes (1 CD-ROM sector)");

struct PsxExeInfo {
    bool valid = false;
    uint32_t entry_point = 0;
    uint32_t load_address = 0;
    uint32_t file_size = 0;
    uint32_t initial_gp = 0;
    uint32_t initial_sp = 0;
};

class ExeLoader {
public:
    ExeLoader() = default;
    ~ExeLoader() = default;

    /// Parses and validates a PS-X EXE header from binary buffer
    static bool ParseHeader(const uint8_t* buffer, size_t size, PsxExeHeader& out_header);

    /// Loads the executable into the CPU context and RAM buffer
    static bool LoadExe(const uint8_t* buffer, size_t size, CpuContext& cpu, std::vector<uint8_t>& ram, PsxExeInfo& out_info);

    /// Helper to parse SYSTEM.CNF to extract boot executable path (e.g., "BOOT = cdrom:\\SLUS_011.58;1")
    static std::string ExtractBootExecutable(const std::string& system_cnf_content);
};

} // namespace personarm

