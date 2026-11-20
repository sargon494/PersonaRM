// personarm_native: prepares the native (recompiled) code for the player's own disc, on the
// player's PC, and caches it (recomp/runtime/native_cache.h). Shipped with the release; the
// launcher runs it before PLAY when the cache is missing. docs/NATIVE_CODE.md.
//
//   personarm_native <disc.cue> [--json] [--force] [--cache-root DIR]   prepare (no-op if ready)
//   personarm_native <disc.cue> --check [--cache-root DIR]              exit 0 ready, 3 not ready
//   personarm_native <disc.cue> --emit-c DIR                            developers: the generated C
//                                                                       for -DPRM_RECOMP_GENERATED_DIR
//   personarm_native --scan <disc.cue>                                  list the code files found
//
// --json: one JSON object per line on stdout:
//   {"stage":"compile","progress":0.42,"message":"Compiling resident"}
//   {"stage":"done","ok":true,"cached":false,"dir":"...","seconds":31.2,...}
//   {"stage":"error","ok":false,"message":"..."}
// Exit codes: 0 ok / ready, 1 failed (play interpreted), 2 usage, 3 not ready (--check).

#include <cstdio>
#include <cstring>
#include <string>

#include "asset_tools.h"
#include "core/json_lite.h"
#include "native_prep.h"
#include "recomp/runtime/native_cache.h"

using namespace personarm;

namespace {

int Usage() {
    std::fprintf(stderr, "usage: personarm_native <disc.cue> [--json] [--force] [--check] [--cache-root DIR] [--emit-c DIR]\n"
                         "       personarm_native --scan <disc.cue>\n");
    return 2;
}

void Emit(const JsonValue& v) {
    std::printf("%s\n", v.Dump(-1).c_str());
    std::fflush(stdout);
}

int Scan(const std::string& disc) {
    recomp::NativeDisc d;
    std::string err;
    tools::DiscFiles files;
    if (!recomp::ReadNativeDisc(disc, d, &err) || !files.Open(disc)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < files.Count(); ++i) {
        if (!files.Read(i, data) || data.size() < 256 || data.size() > 0x180000) continue;
        data.resize(data.size() & ~size_t{3});
        const native::BaseGuess g = native::InferOverlayBase(data.data(), data.size(), 0x80010000u, 0x80200000u);
        if (g.hits) std::printf("F%04u size=%zu base=0x%08X hits=%u in_range=%u %s\n", i, data.size(), g.base, g.hits,
                                g.in_range, g.accepted ? "code" : "-");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--scan") == 0) return Scan(argv[2]);
    if (argc < 2 || argv[1][0] == '-') return Usage();
    native::PrepOptions opt;
    opt.disc = argv[1];
    bool json = false, check = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--force") opt.force = true;
        else if (a == "--check") check = true;
        else if (a == "--cache-root" && i + 1 < argc) opt.cache_root = argv[++i];
        else if (a == "--emit-c" && i + 1 < argc) opt.emit_dir = argv[++i];
        else return Usage();
    }
    if (check) {
        std::string dir, why;
        const bool ready = native::CacheReady(opt.disc, opt.cache_root, dir, why);
        std::printf("%s%s%s\n", ready ? "ready " : "not ready: ", ready ? dir.c_str() : why.c_str(), "");
        return ready ? 0 : 3;
    }
    if (json) {
        opt.progress = [](const std::string& stage, double p, const std::string& msg) {
            JsonValue v = JsonValue::MakeObject();
            v.Set("stage", stage);
            v.Set("progress", p);
            v.Set("message", msg);
            Emit(v);
        };
    } else {
        opt.progress = [](const std::string& stage, double p, const std::string& msg) {
            std::fprintf(stderr, "[%3d%%] %s: %s\n", static_cast<int>(p * 100), stage.c_str(), msg.c_str());
        };
    }
    const native::PrepResult r = native::Prepare(opt);
    if (json) {
        JsonValue v = JsonValue::MakeObject();
        v.Set("stage", r.ok ? "done" : "error");
        v.Set("ok", r.ok);
        if (r.ok) {
            v.Set("cached", r.cached);
            v.Set("dir", r.dir);
            v.Set("seconds", r.seconds);
            v.Set("compile_seconds", r.compile_seconds);
            v.Set("modules", static_cast<double>(r.modules));
            v.Set("functions", static_cast<double>(r.functions));
            v.Set("library_bytes", static_cast<double>(r.library_bytes));
        } else {
            v.Set("message", r.error);
        }
        Emit(v);
    } else if (r.ok) {
        if (r.cached) std::printf("native code ready: %s\n", r.dir.c_str());
        else std::printf("native code %s: %zu modules, %zu functions, %.1f s (compile %.1f s), %zu bytes\n", r.dir.c_str(),
                         r.modules, r.functions, r.seconds, r.compile_seconds, r.library_bytes);
    } else {
        std::fprintf(stderr, "personarm_native: %s\n", r.error.c_str());
    }
    return r.ok ? 0 : 1;
}
