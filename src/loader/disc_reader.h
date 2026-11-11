#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <unordered_map>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────
// ISO 9660 & Disc Structures
// ───────────────────────────────────────────────────────────────────────

struct DiscFileEntry {
    std::string name;
    uint32_t sector_lba = 0;
    uint32_t size_bytes = 0;
    bool is_directory = false;
};

enum class DiscFormat {
    Unknown,
    Iso9660,
    BinCue,
    RawExecutable
};

// ───────────────────────────────────────────────────────────────────────
// Executable Format (ELF / PS-X EXE)
// ───────────────────────────────────────────────────────────────────────

struct ExecutableSegment {
    uint32_t virtual_address = 0;
    uint32_t file_offset = 0;
    uint32_t mem_size = 0;
    uint32_t file_size = 0;
    uint32_t flags = 0; // R/W/X
};

struct ExecutableInfo {
    bool valid = false;
    std::string format;         // "ELF" or "PS-X EXE"
    uint32_t entry_point = 0;
    uint32_t gp_value = 0;      // Global pointer register (MIPS)
    std::vector<ExecutableSegment> segments;
    std::string game_id;        // e.g. "SLUS_01158"
};

// ───────────────────────────────────────────────────────────────────────
// Persona 2: Eternal Punishment disc identification
// Serials come from community databases (psxdatacenter, libretro); NOT checked
// against a physical disc or Redump. See docs/research/PERSONARM_ERRATA_Y_CORRECCIONES.md.
// ───────────────────────────────────────────────────────────────────────

struct Persona2DiscInfo {
    bool recognized = false;
    std::string product_id;     // Canonical form, e.g. "SLUS-01158"
    std::string boot_exe;       // SYSTEM.CNF form, e.g. "SLUS_011.58"
    std::string region;         // "US" or "JP"
    std::string edition;        // e.g. "Standard", "Deluxe Pack", "Atlus Best Collection"
    int disc_number = 0;        // 1 or 2; 0 when unknown
    int disc_count = 2;         // The game spans two discs
};

// ───────────────────────────────────────────────────────────────────────
// DiscReader Class
// ───────────────────────────────────────────────────────────────────────

class DiscReader {
public:
    DiscReader();
    ~DiscReader();

    bool Open(const std::string& file_path);
    void Close();

    bool IsOpen() const { return m_is_open; }
    DiscFormat GetFormat() const { return m_format; }
    std::string GetFilePath() const { return m_file_path; }
    uint32_t GetSectorSize() const { return m_sector_size; }
    uint32_t GetHeaderOffset() const { return m_sector_header_offset; }
    const std::string& GetVolumeLabel() const { return m_volume_label; }
    size_t GetFileCount() const { return m_file_table.size(); }

    // Directory & File Operations
    std::vector<DiscFileEntry> ListFiles(const std::string& directory_path = "/");
    bool ReadFile(const std::string& file_name, std::vector<uint8_t>& out_data);
    bool FileExists(const std::string& file_name) const;

    // Game Boot Identification (Parses SYSTEM.CNF)
    std::string DetectBootExecutable();

    /// Identifies a Persona 2 EP disc from its boot executable name or product id
    /// ("SLUS_011.58", "SLUS-01158" and "slus01158" are all accepted).
    static Persona2DiscInfo IdentifyPersona2(const std::string& boot_exe_or_product_id);

    /// Boot executable names (SYSTEM.CNF form) of every known Persona 2 EP disc.
    static std::vector<std::string> KnownPersona2BootExecutables();

    /// Resolves the binary image path referenced inside a .cue sheet file.
    static std::string ResolveCueBinary(const std::string& cue_path);

    // Executable Parser
    static ExecutableInfo ParseExecutable(const uint8_t* data, size_t size);

private:
    bool ParseIso9660Volume();
    void ReadDirectorySectors(uint32_t lba, uint32_t length_bytes, const std::string& prefix, int depth = 0);

    std::string m_file_path;
    std::ifstream m_stream;
    bool m_is_open = false;
    DiscFormat m_format = DiscFormat::Unknown;
    uint32_t m_sector_size = 2048; // Standard ISO 9660 (2048) or raw CD-ROM Mode 1/2 (2352)
    uint32_t m_sector_header_offset = 0; // 0 for ISO 2048, 16 for Mode 1 2352, 24 for Mode 2 Form 1 2352
    uint32_t m_sector_data_size = 2048;
    std::string m_volume_label;

    std::unordered_map<std::string, DiscFileEntry> m_file_table;
};

} // namespace personarm

