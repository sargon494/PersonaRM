#pragma once

// On-device native code cache. Recompiled code is derived from the player's own disc, so it is
// never shipped: personarm_native builds it on the player's PC at first launch and writes it here,
// one folder per disc executable:
//
//   <root>/<boot exe>-<FNV-1a 64 of the boot exe>/modules.dll (.so)  + manifest.json
//
// The manifest records what the library was built from and with. Anything different now (module
// ABI, generator version, PersonaRM build, executable hash) makes the cache stale: it is rebuilt,
// and until then the game runs interpreted.

#include <cstdint>
#include <string>
#include <vector>

namespace personarm::recomp {

struct NativeDisc {
    std::string boot_exe;        // SYSTEM.CNF name, e.g. "SLPS_027.85"
    uint64_t exe_hash = 0;       // FNV-1a 64 of the whole boot executable file
    std::vector<uint8_t> exe;    // the boot executable (PS-X EXE)
};

/// Reads the boot executable of the disc image (.cue/.bin/.iso). False with `error` if unreadable.
bool ReadNativeDisc(const std::string& disc_path, NativeDisc& out, std::string* error);

/// PRM_NATIVE_CACHE if set, else %LOCALAPPDATA%/PersonaRM/native (Windows) or
/// $XDG_CACHE_HOME (or ~/.cache)/personarm/native.
std::string NativeCacheRoot();
std::string NativeCacheDir(const std::string& root, const NativeDisc& disc);
std::string NativeLibraryName();   // modules.dll / modules.so

/// The manifest for a library built now for `disc` with `compiler` (free text).
std::string NativeManifest(const NativeDisc& disc, const std::string& compiler);

/// The library in `dir` exists and its manifest matches this build and this disc (`manifest` is
/// the text of manifest.json; empty = read it from `dir`). `why` says what is missing or stale.
bool NativeCacheReady(const std::string& dir, const NativeDisc& disc, std::string* why, const std::string& manifest = "");

} // namespace personarm::recomp
