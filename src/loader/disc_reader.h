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

    // Directory & File Operations
    std::vector<DiscFileEntry> ListFiles(const std::string& directory_path = "/");
    bool ReadFile(const std::string& file_name, std::vector<uint8_t>& out_data);
    bool FileExists(const std::string& file_name) const;

    // Game Boot Identification (Parses SYSTEM.CNF)
    std::string DetectBootExecutable();

    // Executable Parser
    static ExecutableInfo ParseExecutable(const uint8_t* data, size_t size);

private:
    bool ParseIso9660Volume();
    void ReadDirectorySectors(uint32_t lba, uint32_t length_bytes, const std::string& prefix);

    std::string m_file_path;
    std::ifstream m_stream;
    bool m_is_open = false;
    DiscFormat m_format = DiscFormat::Unknown;
    uint32_t m_sector_size = 2048; // Standard ISO 9660 (or 2352 for raw CD-ROM Mode 2)

    std::unordered_map<std::string, DiscFileEntry> m_file_table;
};

} // namespace personarm

