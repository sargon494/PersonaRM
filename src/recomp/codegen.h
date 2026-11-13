#pragma once

// Static recompiler, code generator v0 (roadmap step 4).
//
// Translates MIPS-I functions of a raw image into portable C++ (one C++ function per guest
// function) over recomp::CpuState (src/recomp/runtime/recomp_runtime.h), with the interpreter's
// semantics: delay slots, overflow/address traps, SYSCALL/BREAK, COP0 moves, GTE through the same
// Gte class, LWL/LWR/SWL/SWR and the DIV edge cases.
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

struct GeneratedFunction {
    uint32_t entry = 0;
    bool generated = false;
    std::string reason;          // why it was refused, when !generated
    uint32_t instructions = 0;   // distinct instruction starts translated
    uint32_t resume_points = 0;  // mid-function addresses the host may enter at (kRecomp_<module>_resume)
};

struct CodegenResult {
    std::string source;          // complete .cpp
    std::vector<GeneratedFunction> functions;
};

/// `image` is loaded at `base`. `module` names the exported table:
/// `extern const personarm::recomp::FunctionEntry kRecomp_<module>[]`, `kRecomp_<module>_count`
/// and `extern const personarm::recomp::Module kRecompModule_<module>` (image base/size/hash).
/// `exclude`: entries kept as function boundaries but left to the interpreter (host PC hooks).
CodegenResult GenerateCpp(const uint8_t* image, size_t size, uint32_t base,
                          const std::vector<uint32_t>& entries, const std::string& module,
                          const std::vector<uint32_t>& exclude = {});

} // namespace personarm::recomp
