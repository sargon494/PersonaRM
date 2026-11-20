#include "recomp/runtime/module_loader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace personarm::recomp {

bool LoadModuleLibrary(const std::string& path, std::vector<const Module*>& out, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = path + ": " + why; return false; };
#ifdef _WIN32
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
    // No "cannot find the DLL" message box: a missing or broken cache falls back to the interpreter.
    const UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE lib = LoadLibraryW(wide.c_str());
    SetErrorMode(old_mode);
    if (!lib) return fail("cannot load (error " + std::to_string(GetLastError()) + ")");
    auto list = reinterpret_cast<prm_modules_fn>(reinterpret_cast<void*>(GetProcAddress(lib, "prm_modules")));
#else
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) return fail(std::string("cannot load: ") + dlerror());
    auto list = reinterpret_cast<prm_modules_fn>(dlsym(lib, "prm_modules"));
#endif
    if (!list) return fail("no prm_modules export (not a PersonaRM module library)");
    prm_size count = 0;
    const prm_module* const* mods = list(PRM_ABI_VERSION, &count);
    if (!mods) return fail("built for another module ABI (this build: " + std::to_string(PRM_ABI_VERSION) + ")");
    for (prm_size i = 0; i < count; ++i) out.push_back(FromAbi(mods[i]));
    return true;
}

} // namespace personarm::recomp
