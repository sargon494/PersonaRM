#pragma once
// File mods: a mod's <mod>/files/Fnnnn.BIN replaces disc file Fnnnn (any size) without touching
// guest code. The file is served from virtual CD sectors past the end of any real disc, and the
// game's own FILEPOS table in RAM is pointed at them by a GamePatch fired when the game loads that
// table. Design and limits: docs/MOD_FILES.md.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "hw/cdrom.h"

namespace personarm {

struct FileMod {
    uint32_t id = 0;     // Fnnnn
    std::string path;    // the replacement file
    std::string mod;     // folder name, for logs
};

/// Files of the mods under `mods_dir` (<dir>/<mod>/files/Fnnnn.BIN, manifest.json "priority"/"serial"
/// as for textures and gameplay): mods whose "serial" list excludes `serial` are ignored; for each id
/// the highest priority wins, ties by folder name. Sorted by id.
std::vector<FileMod> FindFileMods(const std::string& mods_dir, const std::string& serial);

/// Sector source in front of the disc: LBAs >= kVirtualBase come from the mod files, the rest from `base`.
class FileRedirect : public hw::SectorSource {
public:
    // 87:22:66 MSF, beyond an 80-minute disc (360 000 sectors); the last MSF (99:59:74) bounds the space.
    static constexpr uint32_t kVirtualBase = 0x60000;
    static constexpr uint32_t kVirtualEnd = 100u * 60u * 75u - 150u;

    explicit FileRedirect(hw::SectorSource* base = nullptr) : m_base(base) {}
    void SetBase(hw::SectorSource* base) { m_base = base; }
    bool Empty() const { return m_files.empty(); }

    /// Reads the mod files and returns one patch per file that rewrites its entry of the in-RAM FILEPOS
    /// table to {virtual lba, new size} each time the game loads the table. `filepos_dat` is the disc's
    /// FILEPOS.DAT: the patch only fires while the RAM entry still equals it. Empty (and nothing served)
    /// for a disc whose table address is unknown; files that cannot be used are skipped with a log line.
    std::vector<GamePatch> Load(const std::vector<FileMod>& mods, const std::string& serial,
                                const std::vector<uint8_t>& filepos_dat);

    bool ReadSector(uint32_t lba, uint8_t* out2352) override;

private:
    struct File { uint32_t lba = 0, sectors = 0; std::vector<uint8_t> data; };
    hw::SectorSource* m_base;
    std::vector<File> m_files;  // ascending, contiguous from kVirtualBase
};

/// Host glue: the file mods under `mods_dir` for the booted disc. When any applies, `redirect` becomes
/// the engine's CD sector source and the FILEPOS patches are returned (add them to SetGamePatches);
/// otherwise nothing is installed and the result is empty.
std::vector<GamePatch> InstallFileMods(Engine& engine, FileRedirect& redirect, const std::string& mods_dir);

} // namespace personarm
