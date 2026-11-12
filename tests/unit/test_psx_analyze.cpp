// Tests for tools/psx_analyze: code scanning heuristics and end-to-end analysis of synthetic discs.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "analyzer.h"
#include "core/hash.h"

using namespace personarm;
using namespace personarm::tools;
namespace fs = std::filesystem;

static int g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static void PutU32(std::vector<uint8_t>& v, size_t off, uint32_t x) { std::memcpy(v.data() + off, &x, 4); }
static void AppendWords(std::vector<uint8_t>& v, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) { size_t o = v.size(); v.resize(o + 4); PutU32(v, o, w); }
}

// One "function": prologue, direct GPU + CD-ROM register accesses, a BIOS stub, return.
static std::vector<uint8_t> MakeCode(int functions) {
    std::vector<uint8_t> code;
    for (int i = 0; i < functions; ++i) {
        AppendWords(code, {
            0x27BDFFE8u,            // addiu sp,sp,-24
            0xAFBF0014u,            // sw ra,20(sp)
            0x3C041F80u,            // lui a0,0x1F80
            0xAC821810u,            // sw v0,0x1810(a0)   -> GPU
            0x3C051F80u,            // lui a1,0x1F80
            0x90A21800u,            // lbu v0,0x1800(a1)  -> CD-ROM
            0x240A00A0u, 0x01400008u, 0x2409003Fu,   // A(0x3F) stub
            0x03E00008u, 0x00000000u});             // jr ra ; nop
    }
    return code;
}

static std::vector<uint8_t> MakeExe(int functions) {
    auto code = MakeCode(functions);
    std::vector<uint8_t> exe(0x800, 0);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    PutU32(exe, 0x10, 0x80010000u);
    PutU32(exe, 0x14, 0);
    PutU32(exe, 0x18, 0x80010000u);
    PutU32(exe, 0x1C, static_cast<uint32_t>(code.size()));
    PutU32(exe, 0x30, 0x801FFF00u);
    const char* marker = "Sony Computer Entertainment Inc. for Japan area";
    std::memcpy(exe.data() + 0x4C, marker, std::strlen(marker));
    exe.insert(exe.end(), code.begin(), code.end());
    return exe;
}

static void TestScanners() {
    CHECK(HardwareBlockName(0x1F801810) == "GPU");
    CHECK(HardwareBlockName(0x9F801800) == "CD-ROM");
    CHECK(HardwareBlockName(0xBF801080) == "DMA");
    CHECK(HardwareBlockName(0x1F801C00) == "SPU");
    CHECK(HardwareBlockName(0x1F801070) == "Interrupciones (I_STAT/I_MASK)");
    CHECK(HardwareBlockName(0x1F800010).empty());   // scratchpad is not I/O
    CHECK(HardwareBlockName(0x80010000).empty());

    auto code = MakeCode(300);
    CodeScan s = ScanMips(code.data(), code.size());
    CHECK(s.words == 300u * 11);
    CHECK(s.prologues == 300 && s.ra_saves == 300 && s.jr_ra == 300);
    CHECK(s.hw_blocks.at("GPU") == 300 && s.hw_blocks.at("CD-ROM") == 300);
    CHECK(s.hw_registers.at(0x1F801810) == 300);
    CHECK(s.bios_calls.size() == 1 && s.bios_calls.count({'A', 0x3F}) == 1);
    CHECK(LooksLikeMipsCode(s));

    // A jump forgets the base: lui ; jr ; sw must NOT count.
    std::vector<uint8_t> brk;
    AppendWords(brk, {0x3C041F80u, 0x03E00008u, 0x00000000u, 0xAC821810u});
    CHECK(ScanMips(brk.data(), brk.size()).hw_blocks.empty());
    // addiu/ori on a known base propagate: lui a0,0x1F80 ; ori a0,a0,0x1814 ; sw v0,0(a0) -> GPU status
    std::vector<uint8_t> prop;
    AppendWords(prop, {0x3C041F80u, 0x34841814u, 0xAC820000u});
    auto ps = ScanMips(prop.data(), prop.size());
    CHECK(ps.hw_registers.count(0x1F801814) == 1);
    // Overwriting the register invalidates it.
    std::vector<uint8_t> clob;
    AppendWords(clob, {0x3C041F80u, 0x24040001u /*addiu a0,zero,1*/, 0xAC821810u});
    CHECK(ScanMips(clob.data(), clob.size()).hw_blocks.empty());

    // Pseudo-random data must not look like code.
    std::vector<uint8_t> noise(1 << 20);
    uint32_t x = 12345;
    for (auto& b : noise) { x = x * 1664525u + 1013904223u; b = static_cast<uint8_t>(x >> 24); }
    CHECK(!LooksLikeMipsCode(ScanMips(noise.data(), noise.size())));

    const char text[] = "xx libgpu yy LIBCD zz libcd";
    auto hints = FindSdkHints(reinterpret_cast<const uint8_t*>(text), sizeof(text) - 1);
    CHECK(hints.at("LIBGPU") == 1 && hints.at("LIBCD") == 2 && !hints.count("LIBSPU"));

    auto exe = MakeExe(4);
    auto h = ParsePsxExeHeader(exe.data(), exe.size());
    CHECK(h.valid && h.entry == 0x80010000u && h.load_address == 0x80010000u && h.sp_base == 0x801FFF00u);
    CHECK(h.region_marker == "Sony Computer Entertainment Inc. for Japan area");
    CHECK(!ParsePsxExeHeader(exe.data(), 100).valid);
    exe[0] = 'X';
    CHECK(!ParsePsxExeHeader(exe.data(), exe.size()).valid);
}

// ── Synthetic disc ─────────────────────────────────────────────────────

struct Layout { std::map<uint32_t, std::vector<uint8_t>> sectors; };

static std::vector<uint8_t> DirRecord(uint32_t lba, uint32_t size, bool dir, const std::string& name) {
    size_t len = 33 + name.size();
    if (len % 2) ++len;
    std::vector<uint8_t> r(len, 0);
    r[0] = static_cast<uint8_t>(len);
    PutU32(r, 2, lba);
    PutU32(r, 10, size);
    r[25] = dir ? 2 : 0;
    r[32] = static_cast<uint8_t>(name.size());
    std::memcpy(r.data() + 33, name.data(), name.size());
    return r;
}

static std::vector<uint8_t> DirSector(const std::vector<std::vector<uint8_t>>& records) {
    std::vector<uint8_t> s(2048, 0);
    size_t pos = 0;
    for (const auto& r : records) { std::memcpy(s.data() + pos, r.data(), r.size()); pos += r.size(); }
    return s;
}

static void PutFile(Layout& l, uint32_t lba, const std::vector<uint8_t>& data) {
    for (size_t off = 0, n = 0; off < data.size(); off += 2048, ++n) {
        std::vector<uint8_t> sec(2048, 0);
        std::memcpy(sec.data(), data.data() + off, std::min<size_t>(2048, data.size() - off));
        l.sectors[lba + static_cast<uint32_t>(n)] = sec;
    }
}

static Layout BuildDisc(uint32_t& exe_sectors) {
    Layout l;
    const std::string cnf = "BOOT = cdrom:\\SLPS_027.85;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n";
    auto exe = MakeExe(300);
    exe_sectors = static_cast<uint32_t>((exe.size() + 2047) / 2048);
    auto ovl = MakeCode(300);

    std::vector<uint8_t> pvd(2048, 0);
    pvd[0] = 1; std::memcpy(pvd.data() + 1, "CD001", 5); pvd[6] = 1;
    std::memset(pvd.data() + 40, ' ', 32);
    std::memcpy(pvd.data() + 40, "TESTDISC", 8);
    auto root = DirRecord(20, 2048, true, std::string(1, '\0'));
    std::memcpy(pvd.data() + 156, root.data(), 34);
    l.sectors[16] = pvd;

    l.sectors[20] = DirSector({
        DirRecord(20, 2048, true, std::string(1, '\0')), DirRecord(20, 2048, true, std::string(1, '\1')),
        DirRecord(21, static_cast<uint32_t>(cnf.size()), false, "SYSTEM.CNF;1"),
        DirRecord(22, static_cast<uint32_t>(exe.size()), false, "SLPS_027.85;1"),
        DirRecord(100, 2048 * 4, false, "MOVIE.STR;1"),
        DirRecord(30, 2048, true, "DATA")});
    l.sectors[30] = DirSector({
        DirRecord(30, 2048, true, std::string(1, '\0')), DirRecord(20, 2048, true, std::string(1, '\1')),
        DirRecord(31, static_cast<uint32_t>(ovl.size()), false, "OVL.BIN;1")});
    PutFile(l, 21, std::vector<uint8_t>(cnf.begin(), cnf.end()));
    PutFile(l, 22, exe);
    PutFile(l, 31, ovl);
    PutFile(l, 100, std::vector<uint8_t>(2048 * 4, 0xAB));
    return l;
}

static void WriteIso(const Layout& l, const std::string& path) {
    uint32_t last = 0;
    for (const auto& kv : l.sectors) last = std::max(last, kv.first);
    std::ofstream f(path, std::ios::binary);
    for (uint32_t s = 0; s <= last; ++s) {
        auto it = l.sectors.find(s);
        std::vector<uint8_t> blank(2048, 0);
        const auto& d = it == l.sectors.end() ? blank : it->second;
        f.write(reinterpret_cast<const char*>(d.data()), 2048);
    }
}

static void WriteRaw(const Layout& l, const std::string& path) {
    uint32_t last = 0;
    for (const auto& kv : l.sectors) last = std::max(last, kv.first);
    std::ofstream f(path, std::ios::binary);
    for (uint32_t s = 0; s <= last; ++s) {
        std::vector<uint8_t> raw(2352, 0);
        raw[0] = 0; std::memset(raw.data() + 1, 0xFF, 10); raw[11] = 0;
        raw[15] = 2;
        // Streaming file MOVIE.STR (sectors 100..103): Form 2 + video; everything else Form 1 data.
        const uint8_t sub = (s >= 100 && s < 104) ? 0x62 : 0x08;
        raw[18] = raw[22] = sub;
        auto it = l.sectors.find(s);
        if (it != l.sectors.end()) std::memcpy(raw.data() + 24, it->second.data(), 2048);
        f.write(reinterpret_cast<const char*>(raw.data()), 2352);
    }
}

static const FileReport* Find(const DiscReport& r, const std::string& path) {
    for (const auto& f : r.files) if (f.path == path) return &f;
    return nullptr;
}

static void CheckDisc(const DiscReport& r, uint32_t sector_size) {
    CHECK(r.sector_size == sector_size);
    CHECK(r.volume_label == "TESTDISC");
    CHECK(r.boot_exe == "SLPS_027.85");
    CHECK(r.product_id == "SLPS-02785" && r.region == "JP" && r.edition == "Deluxe Pack" && r.disc_number == 1);
    CHECK(r.directories == 1);
    CHECK(Find(r, "DATA/OVL.BIN") != nullptr);           // subdirectory traversal
    CHECK(r.has_boot && r.boot.is_psx_exe && r.boot.exe.entry == 0x80010000u);
    CHECK(r.boot.code.hw_blocks.at("GPU") == 300 && r.boot.code.hw_blocks.at("CD-ROM") == 300);
    CHECK(r.boot.code.bios_calls.count({'A', 0x3F}) == 1);
    const FileReport* ovl = Find(r, "DATA/OVL.BIN");
    CHECK(ovl->scanned && ovl->code_candidate && !ovl->is_psx_exe);
    const FileReport* str = Find(r, "MOVIE.STR");
    CHECK(str && !str->scanned);                          // media is never read as code
    CHECK(r.extensions.at(".STR").files == 1 && r.extensions.at(".BIN").files == 1);
    CHECK(r.extensions.count("(ejecutable por serial)") == 1);
}

static void TestDiscs() {
    fs::path dir = fs::temp_directory_path() / "prm_analyze_test";
    fs::remove_all(dir); fs::create_directories(dir);
    uint32_t exe_sectors = 0;
    Layout l = BuildDisc(exe_sectors);
    std::string iso = (dir / "test.iso").string();
    std::string bin = (dir / "test.bin").string();
    WriteIso(l, iso);
    WriteRaw(l, bin);

    AnalyzeOptions opt;
    opt.hash = true;
    DiscReport r;
    std::string err;
    CHECK(AnalyzeDisc(iso, opt, r, err));
    CheckDisc(r, 2048);
    CHECK(r.image_name == "test.iso");
    CHECK(Find(r, "MOVIE.STR")->sector_kind.find("2048") == 0);
    {
        std::ifstream f(iso, std::ios::binary);
        std::vector<char> all((std::istreambuf_iterator<char>(f)), {});
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", Crc32(reinterpret_cast<const uint8_t*>(all.data()), all.size()));
        CHECK(r.crc32_hex == buf);
    }

    DiscReport raw;
    CHECK(AnalyzeDisc(bin, AnalyzeOptions{}, raw, err));
    CheckDisc(raw, 2352);
    CHECK(Find(raw, "MOVIE.STR")->sector_kind == "Mode 2 Form 2 video");
    CHECK(Find(raw, "SLPS_027.85")->sector_kind == "Mode 2 Form 1 datos");
    CHECK(raw.crc32_hex.empty());

    // Size limit: a 1-byte limit skips non-boot files but the boot executable is always scanned.
    AnalyzeOptions tiny; tiny.max_scan_bytes = 1;
    DiscReport small;
    CHECK(AnalyzeDisc(iso, tiny, small, err));
    CHECK(small.has_boot && small.boot.scanned && !Find(small, "DATA/OVL.BIN")->scanned && small.skipped_large >= 1);

    std::string md = RenderMarkdown({r, raw});
    CHECK(md.find("SLPS-02785") != std::string::npos && md.find("Deluxe Pack") != std::string::npos);
    CHECK(md.find("| GPU | 300 |") != std::string::npos && md.find("A(0x3F)") != std::string::npos);
    CHECK(md.find("DATA/OVL.BIN") != std::string::npos && md.find("test.iso") != std::string::npos);
    CHECK(md.find(dir.string()) == std::string::npos);    // base name only: no local paths leak

    AnalyzeOptions withcfg; withcfg.cfg = true;
    DiscReport cfgr;
    CHECK(AnalyzeDisc(iso, withcfg, cfgr, err) && cfgr.has_cfg);
    // MakeCode ends each function in a BIOS stub (li $t2,0xA0 ; jr $t2): these are BIOS calls, not indirect jumps.
    CHECK(cfgr.boot_cfg.bios_calls >= 1 && cfgr.boot_cfg.indirect_jumps == 0 && cfgr.boot_cfg.indirect_sites.empty());
    CHECK(cfgr.boot_cfg.reachable_words > 0);
    CHECK(cfgr.boot_cfg.invalid_reached == 0);
    CHECK(cfgr.boot_cfg_follow.reachable_words >= cfgr.boot_cfg.reachable_words);
    CHECK(cfgr.boot_cfg_follow.functions >= cfgr.boot_cfg.functions);
    CHECK(cfgr.boot_cfg_follow.indirect_calls == cfgr.boot_cfg.indirect_calls);
    CHECK(RenderMarkdown({cfgr}).find("Control de flujo") != std::string::npos);
    CHECK(!r.has_cfg);

    // --extract-boot writes the boot executable byte for byte (the user's own copy, locally).
    {
        const std::string out_exe = (dir / "boot.exe").string();
        AnalyzeOptions ex; ex.extract_boot = out_exe;
        DiscReport er;
        CHECK(AnalyzeDisc(iso, ex, er, err) && er.has_boot && er.extract_error.empty());
        std::ifstream f(out_exe, std::ios::binary);
        std::vector<char> got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(got.size() == er.boot.size && got.size() > 0x800);
        CHECK(std::string(got.begin(), got.begin() + 8) == "PS-X EXE");
        AnalyzeOptions bad; bad.extract_boot = (dir / "no_such_dir" / "boot.exe").string();
        DiscReport br;
        CHECK(AnalyzeDisc(iso, bad, br, err) && !br.extract_error.empty());
    }

    DiscReport full;
    AnalyzeOptions fp; fp.full_paths = true;
    CHECK(AnalyzeDisc(iso, fp, full, err) && full.image_name == iso);

    CHECK(!AnalyzeDisc((dir / "missing.iso").string(), opt, r, err) && !err.empty());
    { std::ofstream(dir / "junk.bin") << "not a disc"; }
    CHECK(!AnalyzeDisc((dir / "junk.bin").string(), opt, r, err));
    if (!std::getenv("PSX_ANALYZE_KEEP")) fs::remove_all(dir);   // keep the synthetic images to try the CLI
}

int main() {
    TestScanners();
    TestDiscs();
    std::cout << "psx_analyze tests passed (" << g_checks << " checks)\n";
    return 0;
}
