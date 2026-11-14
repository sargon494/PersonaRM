// File mods (docs/MOD_FILES.md): mod folder priority/serial, FILEPOS patches with their guards, and the
// virtual CD sectors. Synthetic data only. The store-triggered patch itself is tested in test_recomp_engine.cpp.
#undef NDEBUG
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "engine/file_mods.h"

using namespace personarm;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void WriteFile(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

static uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24; }

struct FakeDisc : hw::SectorSource {
    uint32_t last = ~0u;
    bool ReadSector(uint32_t lba, uint8_t* out) override { last = lba; std::memset(out, 0xAB, 2352); return true; }
};

int main() {
    const fs::path dir = fs::temp_directory_path() / "prm-filemods-test";
    fs::remove_all(dir);
    std::vector<uint8_t> small(100, 0x11), big(5000), other(10, 0x33);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7);
    WriteFile(dir / "a_low" / "files" / "F0002.BIN", other);
    WriteFile(dir / "a_low" / "files" / "f0003.bin", big);
    WriteFile(dir / "a_low" / "files" / "readme.txt", other);   // not Fnnnn.BIN: ignored
    std::ofstream(dir / "a_low" / "manifest.json") << R"({"priority":1})";
    WriteFile(dir / "b_high" / "files" / "F0002.BIN", small);
    std::ofstream(dir / "b_high" / "manifest.json") << R"({"priority":5,"serial":["SLPS_027.85"]})";
    WriteFile(dir / "c_other" / "files" / "F0004.BIN", other);
    std::ofstream(dir / "c_other" / "manifest.json") << R"({"priority":9,"serial":["SLUS-01158"]})";

    // Highest priority wins per id, serial filter with any punctuation, sorted by id.
    auto mods = FindFileMods(dir.string(), "SLPS-02785");
    CHECK(mods.size() == 2);
    CHECK(mods[0].id == 2 && mods[0].mod == "b_high");
    CHECK(mods[1].id == 3 && mods[1].mod == "a_low");
    CHECK(FindFileMods((dir / "missing").string(), "SLPS-02785").empty());
    CHECK(FindFileMods(dir.string(), "SLUS-01158").size() == 3);   // b_high is for the other disc

    // Synthetic FILEPOS.DAT: 5 entries {lba, size}; entry 4 is empty.
    std::vector<uint8_t> filepos;
    for (uint32_t v : {100u, 10u, 110u, 20u, 120u, 3000u, 130u, 4000u, 0u, 0u})
        for (int i = 0; i < 4; ++i) filepos.push_back(uint8_t(v >> (8 * i)));

    FileRedirect none;
    CHECK(none.Load(mods, "SLPS-02786", filepos).empty() && none.Empty());   // table address unknown: skip

    FakeDisc disc;
    FileRedirect r(&disc);
    auto patches = r.Load(mods, "SLPS-02785", filepos);
    CHECK(patches.size() == 2 && !r.Empty());
    const uint32_t table = 0x80083948u;
    CHECK(patches[0].trigger == table + 2 * 8 + 4 && patches[0].only_if_nonzero);
    CHECK(patches[0].expect.size() == 1 && patches[0].expect[0].address == table + 16);
    CHECK(patches[0].expect[0].bytes == std::vector<uint8_t>(filepos.begin() + 16, filepos.begin() + 24));
    CHECK(patches[0].write.address == table + 16 && Rd32(patches[0].write.bytes, 0) == FileRedirect::kVirtualBase &&
          Rd32(patches[0].write.bytes, 4) == 100);
    CHECK(Rd32(patches[1].write.bytes, 0) == FileRedirect::kVirtualBase + 1 && Rd32(patches[1].write.bytes, 4) == 5000);

    // Ids past the table and empty entries are skipped.
    FileRedirect r2;
    CHECK(r2.Load({{4, (dir / "a_low" / "files" / "f0003.bin").string(), "x"}}, "SLUS-01158", filepos).empty());
    CHECK(r2.Load({{9, (dir / "a_low" / "files" / "f0003.bin").string(), "x"}}, "SLUS-01158", filepos).empty());

    // Sectors: real LBAs go to the disc; virtual ones are Mode 2 Form 1 with the mod data.
    std::vector<uint8_t> s(2352);
    CHECK(r.ReadSector(123, s.data()) && disc.last == 123 && s[100] == 0xAB);
    disc.last = ~0u;
    CHECK(r.ReadSector(FileRedirect::kVirtualBase, s.data()) && disc.last == ~0u);
    const uint32_t abs = FileRedirect::kVirtualBase + 150;   // 87:22:66 (+150 lead-in)
    CHECK(s[0] == 0 && s[1] == 0xFF && s[11] == 0 && s[15] == 2 && s[18] == 0x08);
    CHECK(s[12] == ((abs / 4500 / 10) << 4 | abs / 4500 % 10) && s[14] == ((abs % 75 / 10) << 4 | abs % 75 % 10));
    CHECK(std::equal(small.begin(), small.end(), s.begin() + 24) && s[24 + 100] == 0);
    CHECK(r.ReadSector(FileRedirect::kVirtualBase + 2, s.data()));          // second sector of F0003
    CHECK(std::equal(big.begin() + 2048, big.begin() + 4096, s.begin() + 24));
    CHECK(r.ReadSector(FileRedirect::kVirtualBase + 3, s.data()));          // tail, zero padded
    CHECK(std::equal(big.begin() + 4096, big.end(), s.begin() + 24) && s[24 + 904] == 0);
    CHECK(r.ReadSector(FileRedirect::kVirtualBase + 50, s.data()) && s[24] == 0);   // past every file: zeros

    fs::remove_all(dir);
    if (g_fail) return 1;
    std::printf("file_mods: OK\n");
    return 0;
}
