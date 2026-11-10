#pragma once

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
    char     magic[8];          // "PS-X EXE"
    uint32_t text_offset;       // Unused / zero
    uint32_t data_offset;       // Unused / zero
    uint32_t initial_pc;        // Program entry point (usually 0x80010000)
    uint32_t initial_gp;        // Initial $gp value
    uint32_t destination_ram;   // RAM destination address (usually 0x80010000)
    uint32_t file_size;         // Data size in bytes (excluding 2048-byte header)
    uint32_t memfill_start;     // Data section start for bss fill
    uint32_t memfill_size;      // BSS fill size
    uint32_t initial_sp_base;   // Initial $sp base address (e.g. 0x801FFFF0)
    uint32_t initial_sp_offset; // Stack offset
    uint8_t  reserved[16];      // Reserved by Sony
    char     marker[1972];      // Sony copyright notice / pad to 2048 bytes
};
#pragma pack(pop)

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
