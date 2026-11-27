#include "native_prep.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

#include "asset_tools.h"
#include "recomp/cfg.h"
#include "recomp/codegen.h"
#include "recomp/mips_decode.h"
#include "recomp/runtime/native_cache.h"

#ifdef PRM_HAVE_TCC
#include "libtcc.h"
#endif

namespace personarm::native {

extern const char kPrmAbiHeader[];   // src/recomp/runtime/prm_abi.h, embedded by CMake

namespace fs = std::filesystem;
using recomp::OpOf;

namespace {

uint32_t Word(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint32_t JalTarget(uint32_t w) { return 0x80000000u | ((w & 0x03FFFFFFu) << 2); }
constexpr uint32_t kRamEnd = 0x80200000u;

std::string Hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }

// Entries of a unit: every function the CFG reaches from its candidates. A candidate already found
// as a call target was explored with it, so it is not walked again.
std::vector<uint32_t> UnitEntries(const UnitSource& u) {
    std::set<uint32_t> found;
    for (uint32_t c : u.candidates) {
        if (found.count(c)) continue;
        const recomp::CfgReport r = recomp::AnalyzeControlFlow(u.image.data(), u.image.size(), u.base, c);
        found.insert(r.function_entries.begin(), r.function_entries.end());
        found.insert(c);
    }
    return {found.begin(), found.end()};
}

struct Collected {
    recomp::NativeDisc disc;
    std::vector<UnitSource> units;   // resident first
};

void Report(const PrepOptions& o, const std::string& stage, double p, const std::string& msg) {
    if (o.progress) o.progress(stage, p, msg);
}

// Disc -> resident + overlays with their CFG start points.
bool Collect(const PrepOptions& opt, Collected& c, std::string& err) {
    Report(opt, "disc", 0.0, "Reading the disc");
    if (!recomp::ReadNativeDisc(opt.disc, c.disc, &err)) return false;
    const std::vector<uint8_t>& exe = c.disc.exe;
    const uint32_t pc0 = Word(&exe[0x10]), t_addr = Word(&exe[0x18]);
    const uint32_t t_size = std::min<uint32_t>(Word(&exe[0x1C]), static_cast<uint32_t>(exe.size() - 0x800)) & ~3u;
    if (t_addr < 0x80000000u || t_addr >= kRamEnd || t_size == 0 || pc0 < t_addr || pc0 >= t_addr + t_size) {
        err = "unexpected PS-X EXE header";
        return false;
    }
    // Overlay slots sit over the tail of the EXE image (its data and BSS once running), never over
    // its first 64 KB (crt0 and libraries).
    const uint32_t lo = t_addr + 0x10000u;

    tools::DiscFiles files;
    if (!files.Open(opt.disc)) { err = "no FILEPOS.DAT on the disc (not a supported game)"; return false; }
    std::vector<uint8_t> data;
    std::vector<UnitSource> ovls;
    std::vector<uint32_t> hits;
    for (uint32_t i = 0; i < files.Count(); ++i) {
        if (i % 32 == 0) Report(opt, "scan", 0.02 + 0.08 * i / files.Count(), "Looking for code on the disc");
        if (!files.Read(i, data) || data.size() < 256 || data.size() > 0x180000) continue;
        data.resize(data.size() & ~size_t{3});
        const BaseGuess g = InferOverlayBase(data.data(), data.size(), lo, kRamEnd);
        if (std::getenv("PRM_NATIVE_VERBOSE") && g.hits)
            std::fprintf(stderr, "F%04u base=%08X hits=%u in_range=%u%s (lo %08X)\n", i, g.base, g.hits, g.in_range, g.accepted ? " code" : "", lo);
        if (!g.accepted) continue;
        UnitSource u;
        char name[16];
        std::snprintf(name, sizeof name, "ovl_F%04u", i);
        u.module = name;
        u.base = g.base;
        u.image = data;
        u.candidates = FunctionStarts(data.data(), data.size(), g.base);
        ovls.push_back(std::move(u));
        hits.push_back(g.hits);
    }
    // The resident image stops at the lowest slot: a page-aligned base with strong evidence (an
    // unaligned or weak low guess is a data file that happened to vote). Anything below that end
    // would overlap the resident code and is dropped.
    uint32_t res_end = t_addr + t_size;
    for (size_t i = 0; i < ovls.size(); ++i)
        if (!(ovls[i].base & 0xFFFu) && hits[i] >= 20) res_end = std::min(res_end, ovls[i].base);
    ovls.erase(std::remove_if(ovls.begin(), ovls.end(), [&](const UnitSource& u) { return u.base < res_end; }), ovls.end());
    UnitSource res;
    res.module = "resident";
    res.base = t_addr;
    res.image.assign(exe.begin() + 0x800, exe.begin() + 0x800 + (res_end - t_addr));
    res.candidates = FunctionStarts(res.image.data(), res.image.size(), t_addr);
    res.candidates.push_back(pc0);
    c.units.push_back(std::move(res));
    for (UnitSource& u : ovls) c.units.push_back(std::move(u));
    for (UnitSource& u : c.units) {
        std::sort(u.candidates.begin(), u.candidates.end());
        u.candidates.erase(std::unique(u.candidates.begin(), u.candidates.end()), u.candidates.end());
    }
    return true;
}

#ifdef PRM_HAVE_TCC
struct TccErrors { std::string text; int count = 0; };
void OnTccError(void* opaque, const char* msg) {
    auto* e = static_cast<TccErrors*>(opaque);
    if (e->count++ < 5) e->text += std::string(msg) + "\n";
}
#endif

// The generated source with prm_abi.h inlined (the compiler gets no include path).
std::string Standalone(const std::string& src, const std::string& file) {
    const std::string inc = "#include \"prm_abi.h\"";
    const size_t at = src.find(inc);
    if (at == std::string::npos) return src;
    return src.substr(0, at) + kPrmAbiHeader + "\n#line 1 \"" + file + "\"\n" + src.substr(at + inc.size());
}

} // namespace

std::vector<uint32_t> FunctionStarts(const uint8_t* data, size_t size, uint32_t base) {
    std::vector<uint32_t> out{base};
    for (size_t o = 0; o + 4 <= size; o += 4) {
        const uint32_t w = Word(data + o);
        if ((w & 0xFFFF8000u) == 0x27BD8000u) out.push_back(base + static_cast<uint32_t>(o));   // addiu sp,sp,-N
        if (w == 0x03E00008u && o + 12 <= size && Word(data + o + 8) != 0) out.push_back(base + static_cast<uint32_t>(o) + 8);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

BaseGuess InferOverlayBase(const uint8_t* data, size_t size, uint32_t lo, uint32_t hi) {
    BaseGuess best;
    if (size < 8 || hi - lo < size) return best;
    std::vector<uint32_t> jals;
    for (size_t o = 0; o + 4 <= size; o += 4)
        if (OpOf(Word(data + o)) == 3) jals.push_back(JalTarget(Word(data + o)));
    const std::vector<uint32_t> starts = FunctionStarts(data, size, 0);
    if (jals.empty()) return best;
    const uint32_t top = hi - static_cast<uint32_t>(size);
    std::unordered_map<uint32_t, uint32_t> votes;
    // Every 4-byte base when that stays cheap; otherwise page-aligned bases only (bucketed by offset).
    if (jals.size() * starts.size() <= 4'000'000) {
        for (uint32_t t : jals)
            for (uint32_t o : starts)
                if (t - o >= lo && t - o <= top && t >= o) ++votes[t - o];
    } else {
        std::unordered_map<uint32_t, std::vector<uint32_t>> by_page;
        for (uint32_t o : starts) by_page[o & 0xFFFu].push_back(o);
        for (uint32_t t : jals) {
            const auto it = by_page.find(t & 0xFFFu);
            if (it == by_page.end()) continue;
            for (uint32_t o : it->second)
                if (t - o >= lo && t - o <= top && t >= o) ++votes[t - o];
        }
    }
    for (const auto& [b, n] : votes) {
        const bool better = n > best.hits || (n == best.hits && (b & 0xFFFu) == 0 && (best.base & 0xFFFu) != 0) ||
                            (n == best.hits && ((b & 0xFFFu) == 0) == ((best.base & 0xFFFu) == 0) && b < best.base);
        if (better) { best.base = b; best.hits = n; }
    }
    for (uint32_t t : jals) best.in_range += (t >= best.base && t - best.base < size) ? 1 : 0;
    // OVERLAY_MAP method step 5: >= 8 internal hits need 80 % consistency; fewer must all hit (+-1).
    best.accepted = best.hits >= 3 && (best.hits >= 8 ? best.hits * 10 >= best.in_range * 8 : best.hits + 1 >= best.in_range);
    return best;
}

bool AddGnuStackHeader(std::vector<uint8_t>& elf) {
    auto get = [&](size_t off, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; --i) v = v << 8 | elf[off + i]; return v; };
    auto put = [&](size_t off, int n, uint64_t v) { for (int i = 0; i < n; ++i) elf[off + i] = static_cast<uint8_t>(v >> (8 * i)); };
    constexpr uint32_t kPtGnuStack = 0x6474E551u, kPfR = 4, kPfW = 2;
    if (elf.size() < 64 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 2 || elf[5] != 1) return false;
    const uint64_t phoff = get(32, 8), shoff = get(40, 8), phent = get(54, 2), phnum = get(56, 2);
    const uint64_t shent = get(58, 2), shnum = get(60, 2);
    if (phent != 56 || phoff + phnum * phent > elf.size() || (shnum && (shent != 64 || shoff + shnum * shent > elf.size()))) return false;
    for (uint64_t i = 0; i < phnum; ++i)
        if (get(phoff + i * phent, 4) == kPtGnuStack) return true;
    const uint64_t new_end = phoff + (phnum + 1) * phent;
    // Nothing but the section header table may sit in the bytes the new entry takes.
    uint64_t first_data = elf.size();
    for (uint64_t i = 1; i < shnum; ++i) {
        const size_t s = shoff + i * shent;
        if (get(s + 4, 4) != 8 /*NOBITS*/ && get(s + 32, 8)) first_data = std::min<uint64_t>(first_data, get(s + 24, 8));
    }
    if (new_end > first_data) return false;
    if (shnum && shoff < new_end && shoff + shnum * shent > phoff + phnum * phent) {   // move the section headers to the end of the file (8-aligned)
        const std::vector<uint8_t> sh(elf.begin() + shoff, elf.begin() + shoff + shnum * shent);
        elf.resize((elf.size() + 7) & ~size_t{7});
        put(40, 8, elf.size());
        elf.insert(elf.end(), sh.begin(), sh.end());
    }
    const size_t p = phoff + phnum * phent;
    std::fill(elf.begin() + p, elf.begin() + p + phent, 0);
    put(p, 4, kPtGnuStack);
    put(p + 4, 4, kPfR | kPfW);
    put(p + 48, 8, 16);
    put(56, 2, phnum + 1);
    return true;
}

std::string CompilerName() {
#ifdef PRM_HAVE_TCC
    return std::string("tcc ") + PRM_TCC_VERSION;
#else
    return "";
#endif
}

bool CacheReady(const std::string& disc, const std::string& cache_root, std::string& dir, std::string& why) {
    recomp::NativeDisc d;
    if (!recomp::ReadNativeDisc(disc, d, &why)) return false;
    dir = recomp::NativeCacheDir(cache_root.empty() ? recomp::NativeCacheRoot() : cache_root, d);
    return recomp::NativeCacheReady(dir, d, &why);
}

PrepResult Prepare(const PrepOptions& opt) {
    PrepResult res;
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    try {
        const bool emit = !opt.emit_dir.empty();
        std::string why;
        if (!emit && !opt.force && CacheReady(opt.disc, opt.cache_root, res.dir, why)) {
            res.ok = res.cached = true;
            return res;
        }
        if (!emit && CompilerName().empty()) { res.error = "this build has no compiler for native code"; return res; }
        Collected c;
        if (!Collect(opt, c, res.error)) return res;
        const std::string root = opt.cache_root.empty() ? recomp::NativeCacheRoot() : opt.cache_root;
        res.dir = emit ? opt.emit_dir : recomp::NativeCacheDir(root, c.disc);
        const fs::path tmp = emit ? fs::path(res.dir) : fs::path(res.dir + ".tmp");
        std::error_code ec;
        if (!emit) fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        if (ec) { res.error = "cannot create " + tmp.string() + ": " + ec.message(); return res; }

#ifdef PRM_HAVE_TCC
        TccErrors errors;
        TCCState* tcc = nullptr;
        if (!emit) {
            tcc = tcc_new();
            tcc_set_error_func(tcc, &errors, OnTccError);
            tcc_set_options(tcc, "-nostdlib");
            tcc_set_output_type(tcc, TCC_OUTPUT_DLL);
        }
        struct Free { TCCState*& s; ~Free() { if (s) tcc_delete(s); } } free_tcc{tcc};
#endif
        double compile = 0;
        std::vector<std::string> names;
        // Total work ~ image bytes (the resident dominates): progress 0.1..0.95 over it.
        size_t total = 0, done = 0;
        for (const UnitSource& u : c.units) total += u.image.size();
        for (const UnitSource& u : c.units) {
            Report(opt, "generate", 0.1 + 0.85 * done / total, "Translating " + u.module);
            const recomp::CodegenResult g = recomp::GenerateC(u.image.data(), u.image.size(), u.base, UnitEntries(u), u.module, {}, emit);
            size_t ok = 0;
            for (const auto& f : g.functions) ok += f.generated ? 1 : 0;
            if (std::getenv("PRM_NATIVE_VERBOSE"))
                std::fprintf(stderr, "%s base=%08X size=%zu candidates=%zu functions=%zu/%zu\n", u.module.c_str(), u.base,
                             u.image.size(), u.candidates.size(), ok, g.functions.size());
            if (ok == 0) { done += u.image.size(); continue; }   // nothing compiled: leave it to the interpreter
            res.functions += ok;
            names.push_back(u.module);
            if (emit) {
                std::ofstream f(tmp / (u.module + ".c"), std::ios::binary);
                if (!(f << g.source)) { res.error = "cannot write " + (tmp / (u.module + ".c")).string(); return res; }
            }
#ifdef PRM_HAVE_TCC
            else {
                Report(opt, "compile", 0.1 + 0.85 * (done + u.image.size() / 2) / total, "Compiling " + u.module);
                const auto c0 = std::chrono::steady_clock::now();
                const int rc = tcc_compile_string(tcc, Standalone(g.source, u.module + ".c").c_str());
                compile += secs(c0, std::chrono::steady_clock::now());
                if (rc != 0) { res.error = "compiler error in " + u.module + ": " + errors.text; return res; }
            }
#endif
            done += u.image.size();
        }
        res.modules = names.size();
        if (emit) {
            res.ok = true;
            res.seconds = secs(t0, std::chrono::steady_clock::now());
            return res;
        }
#ifdef PRM_HAVE_TCC
        Report(opt, "write", 0.96, "Writing native code");
        const auto c0 = std::chrono::steady_clock::now();
        const std::string lib = (tmp / recomp::NativeLibraryName()).string();
        if (tcc_compile_string(tcc, Standalone(recomp::GenerateRegistryC(names), "registry.c").c_str()) != 0 ||
            tcc_output_file(tcc, lib.c_str()) != 0) {
            res.error = "cannot link " + lib + ": " + errors.text;
            return res;
        }
#ifndef _WIN32
        {
            std::ifstream in(lib, std::ios::binary);
            std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), {});
            in.close();
            bool ok = AddGnuStackHeader(image);
            if (ok) {
                std::ofstream out(lib, std::ios::binary | std::ios::trunc);
                ok = static_cast<bool>(out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size())));
            }
            if (!ok) { res.error = "cannot mark " + lib + " as needing no executable stack"; return res; }
        }
#endif
        compile += secs(c0, std::chrono::steady_clock::now());
        res.compile_seconds = compile;
        res.library_bytes = static_cast<size_t>(fs::file_size(lib, ec));
        {
            std::ofstream m(tmp / "manifest.json", std::ios::binary);
            if (!(m << recomp::NativeManifest(c.disc, CompilerName()))) { res.error = "cannot write the manifest"; return res; }
        }
        // Swap the finished folder in: a crash before this leaves the old cache (or none), never half of one.
        fs::remove_all(res.dir, ec);
        fs::rename(tmp, res.dir, ec);
        if (ec) { res.error = "cannot move " + tmp.string() + " to " + res.dir + ": " + ec.message(); return res; }
        res.ok = true;
#endif
    } catch (const std::exception& e) {
        res.error = std::string("unexpected error: ") + e.what();
    }
    res.seconds = secs(t0, std::chrono::steady_clock::now());
    return res;
}

} // namespace personarm::native
