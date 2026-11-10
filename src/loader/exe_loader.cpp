#include "loader/exe_loader.h"
#include "core/logger.h"
#include <cstring>
#include <sstream>
#include <algorithm>

namespace personarm {

bool ExeLoader::ParseHeader(const uint8_t* buffer, size_t size, PsxExeHeader& out_header) {
    if (!buffer || size < sizeof(PsxExeHeader)) {
        PRM_LOG_ERROR("EXE", "Buffer too small to contain a PS-X EXE header.");
        return false;
    }

    std::memcpy(&out_header, buffer, sizeof(PsxExeHeader));

    // Validate magic signature "PS-X EXE"
    if (std::memcmp(out_header.magic, "PS-X EXE", 8) != 0) {
        PRM_LOG_ERROR("EXE", "Invalid PS-X EXE magic identifier.");
        return false;
    }

    return true;
}

bool ExeLoader::LoadExe(const uint8_t* buffer, size_t size, CpuContext& cpu, std::vector<uint8_t>& ram, PsxExeInfo& out_info) {
    PsxExeHeader header;
    if (!ParseHeader(buffer, size, header)) {
        return false;
    }

    out_info.valid = true;
    out_info.entry_point = header.initial_pc;
    out_info.load_address = header.destination_ram;
    out_info.file_size = header.file_size;
    out_info.initial_gp = header.initial_gp;
    out_info.initial_sp = header.initial_sp_base + header.initial_sp_offset;

    PRM_LOG_INFO("EXE", "Parsed PS-X EXE: PC=0x" + std::to_string(out_info.entry_point) +
                        " LoadAddr=0x" + std::to_string(out_info.load_address) +
                        " Size=" + std::to_string(out_info.file_size) + " bytes");

    // Copy executable payload (offset 2048) to destination RAM
    const uint8_t* payload = buffer + sizeof(PsxExeHeader);
    size_t payload_size = std::min<size_t>(header.file_size, size - sizeof(PsxExeHeader));

    // PS1 RAM address translation: mask KUSEG / KSEG0 / KSEG1 to physical RAM offset (0..2MB)
    uint32_t ram_offset = header.destination_ram & 0x1FFFFF;

    if (ram_offset + payload_size <= ram.size()) {
        std::memcpy(ram.data() + ram_offset, payload, payload_size);
        PRM_LOG_INFO("EXE", "Loaded " + std::to_string(payload_size) + " bytes into RAM at offset 0x" + std::to_string(ram_offset));
    } else {
        PRM_LOG_WARN("EXE", "Payload exceeds physical RAM buffer size.");
    }

    // Initialize CPU registers according to EXE header
    cpu.SetPC(out_info.entry_point);
    if (out_info.initial_gp != 0) {
        cpu.SetGPR(28, out_info.initial_gp); // $gp is register 28
    }
    if (out_info.initial_sp != 0) {
        cpu.SetGPR(29, out_info.initial_sp); // $sp is register 29
        cpu.SetGPR(30, out_info.initial_sp); // $fp is register 30
    }

    return true;
}

std::string ExeLoader::ExtractBootExecutable(const std::string& system_cnf_content) {
    // Expected format: BOOT = cdrom:\SLUS_011.58;1
    std::istringstream stream(system_cnf_content);
    std::string line;

    while (std::getline(stream, line)) {
        // Strip \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto boot_pos = line.find("BOOT");
        if (boot_pos != std::string::npos) {
            auto eq_pos = line.find('=', boot_pos);
            if (eq_pos != std::string::npos) {
                std::string val = line.substr(eq_pos + 1);
                // Trim leading spaces
                val.erase(val.begin(), std::find_if(val.begin(), val.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                // Trim trailing spaces
                val.erase(std::find_if(val.rbegin(), val.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), val.end());

                // Format: cdrom:\FILENAME;1 or cdrom:FILENAME;1
                auto colon_pos = val.find(':');
                if (colon_pos != std::string::npos) {
                    val = val.substr(colon_pos + 1);
                }
                while (!val.empty() && (val.front() == '\\' || val.front() == '/')) {
                    val.erase(val.begin());
                }
                auto semicolon_pos = val.find(';');
                if (semicolon_pos != std::string::npos) {
                    val = val.substr(0, semicolon_pos);
                }
                return val;
            }
        }
    }

    return "";
}

} // namespace personarm
