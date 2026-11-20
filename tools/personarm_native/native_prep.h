#pragma once

// First-run native code pipeline (shipped with the release, runs on the player's PC):
// disc -> boot executable + code overlays -> generated C -> TinyCC -> modules.dll in the cache
// (recomp/runtime/native_cache.h). Nothing derived from the game leaves the player's PC.
//
// Overlay discovery (C++ port of the former tools/recomp_gen/gen_overlays.py, without its research
// map): every FILEPOS file is tried as code. Its load address is the base that makes the most of its
// own `jal` targets land on function starts (offset 0, the word after `jr ra` + delay slot, an
// `addiu sp,sp,-N` prologue), as in docs/research/OVERLAY_MAP.md (method, step 4). A wrong guess
// costs nothing at run time: the engine runs a module only while RAM holds exactly the code it was
// generated from (code hash), so data files and wrong bases simply never go live.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace personarm::native {

/// Function starts in `data` loaded at `base`: base, after `jr ra` + delay slot, `addiu sp,sp,-N`.
std::vector<uint32_t> FunctionStarts(const uint8_t* data, size_t size, uint32_t base);

struct BaseGuess {
    uint32_t base = 0;
    uint32_t hits = 0;       // jal targets on a function start of the file at that base
    uint32_t in_range = 0;   // jal targets inside the file at that base
    bool accepted = false;   // enough hits, and consistent (see InferOverlayBase)
};

/// The load address of a code file in [lo, hi): the base with the most `jal` hits on its own
/// function starts (page-aligned bases win ties).
BaseGuess InferOverlayBase(const uint8_t* data, size_t size, uint32_t lo, uint32_t hi);

struct UnitSource {
    std::string module;      // C identifier
    uint32_t base = 0;
    std::vector<uint8_t> image;
    std::vector<uint32_t> candidates;   // CFG start points
};

struct PrepOptions {
    std::string disc;
    std::string cache_root;  // empty = NativeCacheRoot()
    std::string emit_dir;    // non-empty: write the generated .c files there (developer build) and stop
    bool force = false;      // rebuild even if the cache is ready
    // stage ("disc", "scan", "generate", "compile", "write"), overall progress 0..1, a short message
    std::function<void(const std::string& stage, double progress, const std::string& message)> progress;
};

struct PrepResult {
    bool ok = false;
    bool cached = false;     // the cache was already ready: nothing done
    std::string dir;         // the cache folder (or emit_dir)
    std::string error;
    double seconds = 0, compile_seconds = 0;
    size_t modules = 0, functions = 0, library_bytes = 0;
};

/// Runs the whole pipeline. Never throws.
PrepResult Prepare(const PrepOptions& opt);

/// Whether the cache for `disc` is ready (`dir` and `why` filled either way). False on an unreadable disc too.
bool CacheReady(const std::string& disc, const std::string& cache_root, std::string& dir, std::string& why);

/// "tcc <version>" when this build carries TinyCC, else empty (no on-device compile possible).
std::string CompilerName();

} // namespace personarm::native
