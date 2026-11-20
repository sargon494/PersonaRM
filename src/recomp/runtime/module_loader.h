#pragma once

// Run-time loading of generated modules (prm_abi.h): a DLL/.so built on the player's PC by
// personarm_native, or any library exporting `prm_modules`. The link-time path
// (generated_modules.h) stays for developers; both hand the engine the same Module objects.

#include <string>
#include <vector>

#include "recomp/runtime/recomp_runtime.h"

namespace personarm::recomp {

/// Loads `path` and appends its modules to `out`. The library is never unloaded (the engine keeps
/// pointers into it). False, with `error`, when it cannot be loaded, has no `prm_modules`, or was
/// built for another PRM_ABI_VERSION.
bool LoadModuleLibrary(const std::string& path, std::vector<const Module*>& out, std::string* error);

} // namespace personarm::recomp
