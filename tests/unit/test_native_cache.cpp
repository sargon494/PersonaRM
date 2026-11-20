// On-device native cache (recomp/runtime/native_cache.h): the cache key and when it goes stale.
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/json_lite.h"
#include "engine/engine.h"
#include "recomp/codegen.h"
#include "recomp/runtime/native_cache.h"

using namespace personarm;
namespace fs = std::filesystem;

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// The manifest with one field replaced.
std::string With(const std::string& manifest, const std::string& key, JsonValue v) {
    JsonValue m = *JsonValue::Parse(manifest);
    m.Set(key, std::move(v));
    return m.Dump(2);
}

} // namespace

int main() {
    recomp::NativeDisc disc;
    disc.boot_exe = "SLPS_027.85";
    disc.exe_hash = 0x0123456789ABCDEFull;
    const std::string m = recomp::NativeManifest(disc, "test");
    std::string why;
    CHECK(recomp::NativeCacheReady("", disc, &why, m));

    // Each part of the key invalidates the cache on its own.
    CHECK(!recomp::NativeCacheReady("", disc, &why, With(m, "abi", PRM_ABI_VERSION + 1)) && why.find("ABI") != std::string::npos);
    CHECK(!recomp::NativeCacheReady("", disc, &why, With(m, "generator", recomp::kGeneratorVersion + 1)) && why.find("generator") != std::string::npos);
    CHECK(!recomp::NativeCacheReady("", disc, &why, With(m, "build", "0.0.0-other")) && why.find("build") != std::string::npos);
    recomp::NativeDisc other = disc;
    other.exe_hash ^= 1;
    CHECK(!recomp::NativeCacheReady("", other, &why, m) && why.find("executable") != std::string::npos);
    CHECK(!recomp::NativeCacheReady("", disc, &why, "{not json"));
    CHECK(With(m, "compiler", "another") != m && recomp::NativeCacheReady("", disc, &why, With(m, "compiler", "another")));

    // One folder per executable: another hash is another folder.
    CHECK(recomp::NativeCacheDir("root", disc) != recomp::NativeCacheDir("root", other));
    CHECK(recomp::NativeCacheDir("root", disc).find("SLPS_027.85-0123456789abcdef") != std::string::npos);

    // On disk: the manifest alone is not enough, the library must be there too.
    const fs::path dir = fs::temp_directory_path() / "prm_test_native_cache";
    fs::remove_all(dir);
    fs::create_directories(dir);
    CHECK(!recomp::NativeCacheReady(dir.string(), disc, &why));
    std::ofstream(dir / "manifest.json") << m;
    CHECK(!recomp::NativeCacheReady(dir.string(), disc, &why) && why.find("library") != std::string::npos);
    std::ofstream(dir / recomp::NativeLibraryName()) << "x";
    CHECK(recomp::NativeCacheReady(dir.string(), disc, &why));
    fs::remove_all(dir);

    recomp::NativeDisc none;
    CHECK(!recomp::ReadNativeDisc("no_such_disc.cue", none, &why) && !why.empty());

    if (g_fail) { std::fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    std::printf("native cache: ok\n");
    return 0;
}
