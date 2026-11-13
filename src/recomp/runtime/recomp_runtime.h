#pragma once

// Runtime shared by statically recompiled code (roadmap step 4, RC-*).
//
// Generated functions (`void f_XXXXXXXX(CpuState&)`) operate on a plain CpuState and reach memory
// through two callbacks with the same (address, size) contract as the interpreter's hooks.
// Calling protocol: a function returns with `s.pc` set to the address execution continues at
// (the guest $ra on a normal return, an exception vector, or a target nobody compiled). A caller
// resumes after a call only when `s.pc` equals its own return address; anything else unwinds to
// the interpreter, which continues from `s.pc`.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cpu/gte.h"

namespace personarm::recomp {

struct CpuState;
using RecompFn = void (*)(CpuState&);

struct FunctionEntry {
    uint32_t address;
    RecompFn fn;
};

/// Address -> compiled function. Lookups miss for anything not generated (the interpreter runs it).
class Dispatcher {
public:
    void Add(const FunctionEntry* entries, size_t count);
    RecompFn Find(uint32_t address) const;
    size_t Size() const { return m_entries.size(); }

private:
    std::vector<FunctionEntry> m_entries;  // sorted by address
};

struct CpuState {
    uint32_t gpr[32] = {};  // gpr[0] is never written by generated code
    uint32_t hi = 0, lo = 0, pc = 0;
    uint32_t sr = 0, cause = 0, epc = 0, badvaddr = 0;  // COP0 r12, r13, r14, r8
    Gte* gte = nullptr;

    void* mem = nullptr;
    uint32_t (*read)(void* mem, uint32_t address, uint32_t size) = nullptr;
    void (*write)(void* mem, uint32_t address, uint32_t value, uint32_t size) = nullptr;

    const Dispatcher* dispatcher = nullptr;
    uint64_t fallbacks = 0;  // dispatches that found no compiled function
};

inline uint32_t Read(CpuState& s, uint32_t address, uint32_t size) { return s.read(s.mem, address, size); }
inline void Write(CpuState& s, uint32_t address, uint32_t value, uint32_t size) { s.write(s.mem, address, value, size); }

/// Same as CpuContext::RaiseException: EPC/BD, ExcCode, KU/IE push, pc = vector (BEV).
/// `pc` is the address of the faulting instruction; `delay_slot` when it sits in a delay slot.
void RaiseException(CpuState& s, uint32_t exc_code, uint32_t pc, bool delay_slot);

/// Continue at `target`: run its compiled function if there is one, otherwise leave `s.pc` there
/// for the interpreter and count a fallback.
void Dispatch(CpuState& s, uint32_t target);

/// DIV/DIVU with the R3000A results for the edge cases (no trap), as the interpreter computes them.
inline void Div(CpuState& s, uint32_t a, uint32_t b) {
    if (b == 0) {
        s.lo = (static_cast<int32_t>(a) < 0) ? 1u : 0xFFFFFFFFu;
        s.hi = a;
    } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
        s.lo = 0x80000000u;
        s.hi = 0;
    } else {
        s.lo = static_cast<uint32_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
        s.hi = static_cast<uint32_t>(static_cast<int32_t>(a) % static_cast<int32_t>(b));
    }
}

inline void Divu(CpuState& s, uint32_t a, uint32_t b) {
    if (b == 0) {
        s.lo = 0xFFFFFFFFu;
        s.hi = a;
    } else {
        s.lo = a / b;
        s.hi = a % b;
    }
}

} // namespace personarm::recomp
