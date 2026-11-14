#include "engine/file_mods.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "core/json_lite.h"
#include "core/logger.h"

namespace fs = std::filesystem;

namespace personarm {
namespace {

std::string Normalize(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

void Le32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// "F0057.BIN" (any case) -> 57; false for anything else.
bool ParseFileName(const std::string& name, uint32_t& id) {
    if (name.size() != 9 || std::toupper(static_cast<unsigned char>(name[0])) != 'F' || name[5] != '.' ||
        Normalize(name.substr(6)) != "BIN")
        return false;
    id = 0;
    for (int i = 1; i < 5; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
        id = id * 10 + uint32_t(name[i] - '0');
    }
    return true;
}

uint8_t Bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

// In-RAM copy of FILEPOS.DAT per disc (docs/research/TEXT_SYSTEM.md TS-013, DISC_US_FORENSICS.md).
struct TableSite { const char* serial; uint32_t address; };
constexpr TableSite kTables[] = {
    {"SLPS02785", 0x80083948u},   // JP disc 1 (Deluxe Pack), load_file_by_id 0x800272D0
    {"SLUS01158", 0x80085068u},   // US, load_file_by_id 0x80027584
};

} // namespace

std::vector<FileMod> FindFileMods(const std::string& mods_dir, const std::string& serial) {
    struct Pick { int priority; FileMod file; };
    std::map<uint32_t, Pick> best;
    std::error_code ec;
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(mods_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());
    for (const fs::path& f : folders) {
        if (!fs::is_directory(f / "files", ec)) continue;
        int priority = 0;
        if (std::ifstream in{f / "manifest.json"}) {
            std::stringstream ss;
            ss << in.rdbuf();
            auto json = JsonValue::Parse(ss.str());
            if (!json || !json->IsObject()) continue;  // VramTextureHook::LoadMods already warns
            priority = int(json->NumberOr("priority", 0));
            const JsonValue* list = json->Find("serial");
            if (list && list->IsArray() && !serial.empty()) {
                bool match = false;
                for (const auto& s : list->Items()) match = match || Normalize(s.AsString()) == Normalize(serial);
                if (!match) continue;
            }
        }
        std::vector<fs::path> files;
        for (fs::directory_iterator it(f / "files", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& p : files) {
            uint32_t id = 0;
            if (!ParseFileName(p.filename().string(), id)) {
                PRM_LOG_WARN("FILEMODS", "mod " + f.filename().string() + ": ignoring files/" + p.filename().string() + " (expected Fnnnn.BIN)");
                continue;
            }
            auto it = best.find(id);
            if (it != best.end()) {
                // Folders come in name order: an equal priority keeps the earlier folder.
                const bool wins = priority > it->second.priority;
                PRM_LOG_WARN("FILEMODS", p.filename().string() + " is in mods " + it->second.file.mod + " and " +
                                             f.filename().string() + "; using " + (wins ? f.filename().string() : it->second.file.mod));
                if (!wins) continue;
            }
            best[id] = {priority, {id, p.string(), f.filename().string()}};
        }
    }
    std::vector<FileMod> out;
    for (auto& [id, pick] : best) out.push_back(std::move(pick.file));
    return out;
}

std::vector<GamePatch> FileRedirect::Load(const std::vector<FileMod>& mods, const std::string& serial,
                                          const std::vector<uint8_t>& filepos_dat) {
    m_files.clear();
    std::vector<GamePatch> patches;
    if (mods.empty()) return patches;
    uint32_t table = 0;
    for (const auto& t : kTables)
        if (Normalize(serial) == t.serial) table = t.address;
    if (!table) {
        PRM_LOG_WARN("FILEMODS", "disc " + (serial.empty() ? std::string("(unrecognized)") : serial) +
                                     ": FILEPOS table address unknown, " + std::to_string(mods.size()) + " file mod(s) skipped");
        return patches;
    }
    uint32_t next = kVirtualBase;
    for (const FileMod& m : mods) {
        const std::string what = "mod " + m.mod + " F" + std::to_string(m.id) + ": ";
        if (uint64_t(m.id) * 8 + 8 > filepos_dat.size()) {
            PRM_LOG_WARN("FILEMODS", what + "no such file on this disc, skipped");
            continue;
        }
        const uint8_t* entry = filepos_dat.data() + m.id * 8;
        if (!(entry[4] | entry[5] | entry[6] | entry[7])) {
            PRM_LOG_WARN("FILEMODS", what + "empty FILEPOS entry on this disc, skipped");  // the patch triggers on a nonzero size
            continue;
        }
        File f;
        std::ifstream in(m.path, std::ios::binary);
        f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        f.sectors = static_cast<uint32_t>((f.data.size() + 2047) / 2048);
        if (f.data.empty() || f.data.size() > 0xFFFFFFFFu || f.sectors == 0) {
            PRM_LOG_WARN("FILEMODS", what + "cannot read " + m.path + " (or empty), skipped");
            continue;
        }
        if (uint64_t(next) + f.sectors > kVirtualEnd) {
            PRM_LOG_WARN("FILEMODS", what + "no virtual CD space left, skipped");
            continue;
        }
        f.lba = next;
        next += f.sectors;

        const uint32_t addr = table + m.id * 8;
        GamePatch g;
        g.name = "file F" + std::to_string(m.id) + " (" + m.mod + ")";
        g.trigger = addr + 4;          // the size word: the table is written in ascending order
        g.only_if_nonzero = true;      // the zeroed table before the game reads FILEPOS.DAT
        g.expect.push_back({addr, std::vector<uint8_t>(entry, entry + 8)});
        g.write.address = addr;
        Le32(g.write.bytes, f.lba);
        Le32(g.write.bytes, static_cast<uint32_t>(f.data.size()));
        PRM_LOG_INFO("FILEMODS", what + std::to_string(f.data.size()) + " B at virtual LBA " + std::to_string(f.lba));
        patches.push_back(std::move(g));
        m_files.push_back(std::move(f));
    }
    return patches;
}

bool FileRedirect::ReadSector(uint32_t lba, uint8_t* out) {
    if (lba < kVirtualBase || m_files.empty()) return m_base && m_base->ReadSector(lba, out);
    // Mode 2 Form 1 data sector; EDC/ECC left zero (the controller does not check them here).
    std::memset(out, 0, hw::CdRom::kSectorBytes);
    std::memset(out + 1, 0xFF, 10);
    const uint32_t abs = lba + 150;
    out[12] = Bcd(abs / 4500);
    out[13] = Bcd(abs / 75 % 60);
    out[14] = Bcd(abs % 75);
    out[15] = 2;
    out[18] = out[22] = 0x08;   // submode: data
    // Sectors of the virtual area outside every file read as zeros, like padding on a disc.
    auto it = std::upper_bound(m_files.begin(), m_files.end(), lba, [](uint32_t l, const File& f) { return l < f.lba; });
    if (it == m_files.begin()) return true;
    const File& f = *--it;
    if (lba >= f.lba + f.sectors) return true;
    const size_t off = size_t(lba - f.lba) * 2048;
    std::memcpy(out + 24, f.data.data() + off, std::min<size_t>(2048, f.data.size() - off));
    return true;
}

std::vector<GamePatch> InstallFileMods(Engine& engine, FileRedirect& redirect, const std::string& mods_dir) {
    const std::vector<FileMod> mods = FindFileMods(mods_dir, engine.DiscSerial());
    if (mods.empty()) return {};
    std::vector<uint8_t> filepos;
    if (!engine.GetDiscReader().ReadFile("FILEPOS.DAT", filepos)) {
        PRM_LOG_WARN("FILEMODS", "no FILEPOS.DAT on this disc, " + std::to_string(mods.size()) + " file mod(s) skipped");
        return {};
    }
    std::vector<GamePatch> patches = redirect.Load(mods, engine.DiscSerial(), filepos);
    if (redirect.Empty()) return {};
    redirect.SetBase(&engine.GetDiscSectorSource());
    engine.SetCdSectorSource(&redirect);
    return patches;
}

} // namespace personarm
