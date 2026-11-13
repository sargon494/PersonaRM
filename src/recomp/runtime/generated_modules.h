#pragma once

// Recompiled modules linked into this binary. Code generated from the game never lives in the
// repository: the personarm target gets it only from -DPRM_RECOMP_GENERATED_DIR=<dir outside the
// repo> (one <module>.cpp per tools/recomp_gen --module <module>); CMake then writes the registry.
// Default builds link generated_modules_none.cpp and return an empty list.

#include <vector>

#include "recomp/runtime/recomp_runtime.h"

namespace personarm::recomp {

std::vector<const Module*> GeneratedModules();

} // namespace personarm::recomp
