#include "recomp/runtime/native_cache.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/json_lite.h"
#include "engine/engine.h"
#include "loader/disc_reader.h"
#include "recomp/codegen.h"
#include "recomp/runtime/module_loader.h"
#include "recomp/runtime/recomp_runtime.h"

namespace personarm::recomp {

namespace fs = std::filesystem;

bool ReadNativeDisc(const std::string& disc_path, NativeDisc& out, std::string* error) {
    DiscReader d;
    if (!d.Open(disc_path)) { if (error) *error = "cannot open " + disc_path; return false; }
    out.boot_exe = d.DetectBootExecutable();
    if (out.boot_exe.empty() || !d.ReadFile(out.boot_exe, out.exe) || out.exe.size() < 0x800 ||
        std::string(out.exe.begin(), out.exe.begin() + 8) != "PS-X EXE") {
        if (error) *error = "no PS-X boot executable on " + disc_path;
        return false;
    }
    for (char& c : out.boot_exe)  // a folder name: keep it to [A-Za-z0-9_.-]
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') c = '_';
    out.exe_hash = Fnv1a64(out.exe.data(), out.exe.size());
    return true;
}

std::string NativeCacheRoot() {
    if (const char* v = std::getenv("PRM_NATIVE_CACHE"); v && *v) return v;
#ifdef _WIN32
    if (const char* v = std::getenv("LOCALAPPDATA"); v && *v) return (fs::path(v) / "PersonaRM" / "native").string();
#else
    if (const char* v = std::getenv("XDG_CACHE_HOME"); v && *v) return (fs::path(v) / "personarm" / "native").string();
    if (const char* v = std::getenv("HOME"); v && *v) return (fs::path(v) / ".cache" / "personarm" / "native").string();
#endif
    return (fs::temp_directory_path() / "PersonaRM-native").string();
}

static std::string Hex64(uint64_t v) { char b[20]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b; }

std::string NativeCacheDir(const std::string& root, const NativeDisc& disc) {
    return (fs::path(root) / (disc.boot_exe + "-" + Hex64(disc.exe_hash))).string();
}

std::string NativeLibraryName() {
#ifdef _WIN32
    return "modules.dll";
#else
    return "modules.so";
#endif
}

std::string NativeManifest(const NativeDisc& disc, const std::string& compiler) {
    JsonValue m = JsonValue::MakeObject();
    m.Set("abi", static_cast<uint32_t>(PRM_ABI_VERSION));
    m.Set("generator", kGeneratorVersion);
    m.Set("build", std::string(Engine::BuildId()));
    m.Set("boot_exe", disc.boot_exe);
    m.Set("exe_hash", Hex64(disc.exe_hash));
    m.Set("library", NativeLibraryName());
    m.Set("compiler", compiler);
    return m.Dump(2);
}

bool NativeCacheReady(const std::string& dir, const NativeDisc& disc, std::string* why, const std::string& manifest) {
    auto stale = [&](const std::string& w) { if (why) *why = w; return false; };
    std::string text = manifest;
    if (text.empty()) {
        std::ifstream f(fs::path(dir) / "manifest.json", std::ios::binary);
        if (!f) return stale("no native code for this disc yet");
        text.assign(std::istreambuf_iterator<char>(f), {});
    }
    const auto m = JsonValue::Parse(text);
    if (!m || !m->IsObject()) return stale("unreadable manifest");
    if (m->NumberOr("abi", -1) != PRM_ABI_VERSION) return stale("built for another module ABI");
    if (m->NumberOr("generator", -1) != kGeneratorVersion) return stale("built by another generator version");
    if (m->StringOr("build", "") != Engine::BuildId()) return stale("built by another PersonaRM build");
    if (m->StringOr("exe_hash", "") != Hex64(disc.exe_hash)) return stale("built from another executable");
    if (manifest.empty() && !fs::exists(fs::path(dir) / NativeLibraryName())) return stale("library missing");
    return true;
}

std::vector<const Module*> NativeModulesFor(const std::string& disc_path, std::vector<const Module*> linked, std::string& note) {
    std::vector<const Module*> mods = std::move(linked);
    if (!mods.empty()) { note = "linked into this build"; return mods; }
    NativeDisc d;
    if (!ReadNativeDisc(disc_path, d, &note)) return {};
    const std::string dir = NativeCacheDir(NativeCacheRoot(), d);
    if (!NativeCacheReady(dir, d, &note)) return {};
    if (!LoadModuleLibrary((fs::path(dir) / NativeLibraryName()).string(), mods, &note)) return {};
    note = dir;
    return mods;
}

} // namespace personarm::recomp
