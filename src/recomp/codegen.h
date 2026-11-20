#pragma once

// Static recompiler, code generator (roadmap step 4).
//
// Translates MIPS-I functions of a raw image into portable C (one C function per guest function)
// over prm_cpu (src/recomp/runtime/prm_abi.h, the C mirror of recomp::CpuState), with the
// interpreter's semantics: delay slots, overflow/address traps, SYSCALL/BREAK, COP0 moves, GTE
// through the host's Gte (CpuState::cop2), LWL/LWR/SWL/SWR and the DIV edge cases. The output needs
// only prm_abi.h, so it builds with MSVC/GCC/Clang into the binary or with TinyCC into a DLL on the
// player's PC (tools/personarm_native).
//
// Loads are translated literally (no load delay). A function where that would differ from the
// interpreter (a delayed load whose next instruction reads or overwrites the register), or that
// has a branch in a delay slot or an unsupported word, is not generated: the interpreter runs it.
// Known limit: a load in the delay slot of jr/jalr is not checked against the code it returns to.
//
// Every instruction counts its cycles into CpuState and offers the host a service point
// (recomp_runtime.h), so an engine can deliver IRQs and end frames at instruction boundaries.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace personarm::recomp {

/// Bump whenever the generated code changes for the same input: on-device caches are rebuilt.
constexpr uint32_t kGeneratorVersion = 1;

struct GeneratedFunction {
    uint32_t entry = 0;
    bool generated = false;
    std::string reason;          // why it was refused, when !generated
    uint32_t instructions = 0;   // distinct instruction starts translated
    uint32_t resume_points = 0;  // mid-function addresses the host may enter at
};

struct CodegenResult {
    std::string source;          // complete .c
    std::vector<GeneratedFunction> functions;
};

/// `image` is loaded at `base`. The output defines `const prm_module kRecompModule_<module>` (image
/// base/size/hash, entry and resume tables, translated code); everything else in it is static.
/// `exclude`: entries kept as function boundaries but left to the interpreter (host PC hooks).
/// `comments`: one line per guest instruction (readable, but much larger: off on the player's PC).
CodegenResult GenerateC(const uint8_t* image, size_t size, uint32_t base,
                        const std::vector<uint32_t>& entries, const std::string& module,
                        const std::vector<uint32_t>& exclude = {}, bool comments = true);

/// The registry of a module library: exports `prm_modules` (prm_abi.h) listing `modules`, each
/// defined by its own GenerateC output. Built with TinyCC -nostdlib it also defines the DLL entry.
std::string GenerateRegistryC(const std::vector<std::string>& modules);

} // namespace personarm::recomp
