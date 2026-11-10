#include <cctype>
#include "disc_reader.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace personarm {

static constexpr uint32_t ISO_SECTOR_SIZE = 2048;
static constexpr uint32_t CD_RAW_SECTOR_SIZE = 2352;
static constexpr uint32_t ISO_PVD_LBA = 16; // Primary Volume Descriptor is at sector 16

DiscReader::DiscReader() = default;

DiscReader::~DiscReader() {
    Close();
}

void DiscReader::Close() {
    if (m_stream.is_open()) {
        m_stream.close();
    }
    m_is_open = false;
    m_format = DiscFormat::Unknown;
    m_file_table.clear();
}

bool DiscReader::Open(const std::string& file_path) {
    Close();

    m_stream.open(file_path, std::ios::binary);
    if (!m_stream.is_open()) {
        return false;
    }

    m_file_path = file_path;
    m_is_open = true;

    // Detect format
    std::string ext = file_path;
    size_t dot_pos = ext.find_last_of('.');
    if (dot_pos != std::string::npos) {
        ext = ext.substr(dot_pos);
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    }

    if (ext == ".elf") {
        m_format = DiscFormat::RawExecutable;
        return true;
    }

    // Try 2048-byte sector ISO 9660 first
    m_sector_size = ISO_SECTOR_SIZE;
    if (ParseIso9660Volume()) {
        m_format = DiscFormat::Iso9660;
        return true;
    }

    // Try 2352-byte sector BIN/RAW CD image
    m_sector_size = CD_RAW_SECTOR_SIZE;
    if (ParseIso9660Volume()) {
        m_format = DiscFormat::BinCue;
        return true;
    }

    // Fallback: check if raw executable directly
    m_stream.seekg(0, std::ios::beg);
    char magic[4] = {0};
    m_stream.read(magic, 4);
    if ((magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') ||
        (std::strncmp(magic, "PS-X", 4) == 0)) {
        m_format = DiscFormat::RawExecutable;
        return true;
    }

    return false;
}

bool DiscReader::ParseIso9660Volume() {
    m_file_table.clear();

    // Read Primary Volume Descriptor at LBA 16
    uint64_t offset = static_cast<uint64_t>(ISO_PVD_LBA) * m_sector_size;
    m_stream.seekg(offset, std::ios::beg);

    std::vector<uint8_t> sector(m_sector_size);
    m_stream.read(reinterpret_cast<char*>(sector.data()), m_sector_size);
    if (m_stream.gcount() < static_cast<std::streamsize>(m_sector_size)) {
        return false;
    }

    // Verify "CD001" standard identifier at offset 1
    if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' || sector[4] != '0' || sector[5] != '1') {
        return false;
    }

    // Root directory record starts at offset 156 of the PVD
    // Offset 156 + 2: Location of extent (LBA) in Little-Endian (4 bytes)
    // Offset 156 + 10: Data length in Little-Endian (4 bytes)
    const uint8_t* root_record = sector.data() + 156;
    uint32_t root_lba = *reinterpret_cast<const uint32_t*>(root_record + 2);
    uint32_t root_len = *reinterpret_cast<const uint32_t*>(root_record + 10);

    ReadDirectorySectors(root_lba, root_len, "");
    return !m_file_table.empty();
}

void DiscReader::ReadDirectorySectors(uint32_t lba, uint32_t length_bytes, const std::string& prefix) {
    uint32_t sectors_to_read = (length_bytes + m_sector_size - 1) / m_sector_size;
    std::vector<uint8_t> buffer(m_sector_size);

    for (uint32_t s = 0; s < sectors_to_read; ++s) {
        uint64_t offset = static_cast<uint64_t>(lba + s) * m_sector_size;
        m_stream.seekg(offset, std::ios::beg);
        m_stream.read(reinterpret_cast<char*>(buffer.data()), m_sector_size);

        size_t pos = 0;
        while (pos < m_sector_size) {
            uint8_t record_len = buffer[pos];
            if (record_len == 0) {
                break; // Padding to end of sector
            }
            if (pos + record_len > m_sector_size) {
                break;
            }

            uint32_t file_lba = *reinterpret_cast<const uint32_t*>(&buffer[pos + 2]);
            uint32_t file_size = *reinterpret_cast<const uint32_t*>(&buffer[pos + 10]);
            uint8_t flags = buffer[pos + 25];
            uint8_t name_len = buffer[pos + 32];

            std::string name(reinterpret_cast<const char*>(&buffer[pos + 33]), name_len);

            // Clean ISO 9660 version suffix (e.g. "FILE.TXT;1" -> "FILE.TXT")
            size_t semi = name.find(';');
            if (semi != std::string::npos) {
                name = name.substr(0, semi);
            }

            bool is_dir = (flags & 0x02) != 0;

            if (name != "\x00" && name != "\x01" && !name.empty()) {
                std::string full_path = prefix.empty() ? name : prefix + "/" + name;
                DiscFileEntry entry{full_path, file_lba, file_size, is_dir};
                m_file_table[full_path] = entry;

                // Also store uppercase key for case-insensitive lookup
                std::string upper_name = full_path;
                for (auto& c : upper_name) c = static_cast<char>(std::toupper(c));
                m_file_table[upper_name] = entry;
            }

            pos += record_len;
        }
    }
}

std::vector<DiscFileEntry> DiscReader::ListFiles(const std::string& directory_path) {
    (void)directory_path;
    std::vector<DiscFileEntry> result;
    for (const auto& [k, v] : m_file_table) {
        // Return entries without duplication (original case only)
        if (k == v.name) {
            result.push_back(v);
        }
    }
    return result;
}

bool DiscReader::FileExists(const std::string& file_name) const {
    std::string upper = file_name;
    for (auto& c : upper) c = static_cast<char>(std::toupper(c));
    return m_file_table.find(file_name) != m_file_table.end() ||
           m_file_table.find(upper) != m_file_table.end();
}

bool DiscReader::ReadFile(const std::string& file_name, std::vector<uint8_t>& out_data) {
    if (!m_is_open) return false;

    std::string upper = file_name;
    for (auto& c : upper) c = static_cast<char>(std::toupper(c));

    auto it = m_file_table.find(file_name);
    if (it == m_file_table.end()) {
        it = m_file_table.find(upper);
    }
    if (it == m_file_table.end()) {
        return false;
    }

    const DiscFileEntry& entry = it->second;
    out_data.resize(entry.size_bytes);

    uint64_t offset = static_cast<uint64_t>(entry.sector_lba) * m_sector_size;
    m_stream.seekg(offset, std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(out_data.data()), entry.size_bytes);

    return m_stream.gcount() == static_cast<std::streamsize>(entry.size_bytes);
}

std::string DiscReader::DetectBootExecutable() {
    std::vector<uint8_t> cnf_data;
    if (ReadFile("SYSTEM.CNF", cnf_data)) {
        std::string text(cnf_data.begin(), cnf_data.end());
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            // Looking for line like: BOOT = cdrom:\SLUS_011.58;1
            size_t boot_pos = line.find("BOOT");
            if (boot_pos != std::string::npos) {
                size_t slash_pos = line.find('\\');
                if (slash_pos == std::string::npos) {
                    slash_pos = line.find('/');
                }
                if (slash_pos != std::string::npos) {
                    std::string exe = line.substr(slash_pos + 1);
                    // Trim semicolon and trailing spaces
                    size_t semi = exe.find(';');
                    if (semi != std::string::npos) exe = exe.substr(0, semi);
                    while (!exe.empty() && (exe.back() == '\r' || exe.back() == ' ' || exe.back() == '\n')) {
                        exe.pop_back();
                    }
                    return exe;
                }
            }
        }
    }

    // Fallback: probe for the boot executable of every known Persona 2 EP disc
    for (const std::string& exe : KnownPersona2BootExecutables()) {
        if (FileExists(exe)) {
            return exe;
        }
    }

    return "";
}

namespace {

struct Persona2Entry {
    const char* product_id;   // canonical "XXXX-NNNNN"
    const char* region;
    const char* edition;
    int disc_number;          // 0 = unknown
};

// Source: psxdatacenter / libretro database (community, unverified against media).
const Persona2Entry kPersona2Discs[] = {
    {"SLUS-01158", "US", "Standard",              1},
    {"SLUS-01339", "US", "Standard",              2},
    {"SLPS-02785", "JP", "Deluxe Pack",           1},
    {"SLPS-02786", "JP", "Deluxe Pack",           2},
    {"SLPS-02825", "JP", "Standard",              1},
    {"SLPS-02826", "JP", "Standard",              2},
    {"SCPS-45495", "JP", "SCPS release",          1},
    {"SCPS-45496", "JP", "SCPS release",          2},
    {"SLPM-86967", "JP", "Atlus Best Collection", 0},
};

// "SLUS_011.58" / "slus-01158" / "SLUS01158;1" -> "SLUS01158"
std::string NormalizeSerial(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == ';') break;
        if (c == '_' || c == '.' || c == '-' || c == ' ') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// "SLUS-01158" -> "SLUS_011.58"
std::string ToBootExeName(const char* product_id) {
    std::string digits = NormalizeSerial(product_id).substr(4); // 5 digits
    return std::string(product_id, 4) + "_" + digits.substr(0, 3) + "." + digits.substr(3);
}

} // namespace

Persona2DiscInfo DiscReader::IdentifyPersona2(const std::string& boot_exe_or_product_id) {
    Persona2DiscInfo info;
    const std::string key = NormalizeSerial(boot_exe_or_product_id);
    for (const auto& entry : kPersona2Discs) {
        if (NormalizeSerial(entry.product_id) == key) {
            info.recognized = true;
            info.product_id = entry.product_id;
            info.boot_exe = ToBootExeName(entry.product_id);
            info.region = entry.region;
            info.edition = entry.edition;
            info.disc_number = entry.disc_number;
            return info;
        }
    }
    return info;
}

std::vector<std::string> DiscReader::KnownPersona2BootExecutables() {
    std::vector<std::string> names;
    for (const auto& entry : kPersona2Discs) {
        names.push_back(ToBootExeName(entry.product_id));
    }
    return names;
}

ExecutableInfo DiscReader::ParseExecutable(const uint8_t* data, size_t size) {
    ExecutableInfo info;
    if (!data || size < 64) {
        return info;
    }

    // 1. Check for PS-X EXE (PlayStation executable)
    if (std::memcmp(data, "PS-X EXE", 8) == 0 && size >= 2048) {
        info.valid = true;
        info.format = "PS-X EXE";
        info.entry_point = *reinterpret_cast<const uint32_t*>(data + 0x10);
        info.gp_value = *reinterpret_cast<const uint32_t*>(data + 0x14);

        uint32_t load_address = *reinterpret_cast<const uint32_t*>(data + 0x18);
        uint32_t data_size = *reinterpret_cast<const uint32_t*>(data + 0x1C);

        ExecutableSegment seg;
        seg.virtual_address = load_address;
        seg.file_offset = 2048; // Header is 2048 bytes
        seg.file_size = data_size;
        seg.mem_size = data_size;
        seg.flags = 0x7; // R/W/X
        info.segments.push_back(seg);
        return info;
    }

    // 2. Check for ELF Header (MIPS 32-bit Little Endian)
    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F' && size >= 52) {
        info.valid = true;
        info.format = "ELF";
        info.entry_point = *reinterpret_cast<const uint32_t*>(data + 0x18);

        uint32_t ph_offset = *reinterpret_cast<const uint32_t*>(data + 0x1C);
        uint16_t ph_entry_size = *reinterpret_cast<const uint16_t*>(data + 0x2A);
        uint16_t ph_count = *reinterpret_cast<const uint16_t*>(data + 0x2C);

        if (ph_offset > 0 && ph_entry_size >= 32) {
            for (uint16_t i = 0; i < ph_count; ++i) {
                const uint8_t* ph = data + ph_offset + (i * ph_entry_size);
                if (ph + 32 > data + size) break;

                uint32_t type = *reinterpret_cast<const uint32_t*>(ph + 0);
                if (type == 1) { // PT_LOAD segment
                    ExecutableSegment seg;
                    seg.file_offset = *reinterpret_cast<const uint32_t*>(ph + 4);
                    seg.virtual_address = *reinterpret_cast<const uint32_t*>(ph + 8);
                    seg.file_size = *reinterpret_cast<const uint32_t*>(ph + 16);
                    seg.mem_size = *reinterpret_cast<const uint32_t*>(ph + 20);
                    seg.flags = *reinterpret_cast<const uint32_t*>(ph + 24);
                    info.segments.push_back(seg);
                }
            }
        }
        return info;
    }

    return info;
}

} // namespace personarm

