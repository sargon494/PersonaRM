#pragma once

// Runtime shared by statically recompiled code (roadmap step 4, RC-*).
//
// Generated functions (`void f_XXXXXXXX(CpuState&)`) operate on a plain CpuState and reach memory
// through two callbacks with the same (address, size) contract as the interpreter's hooks.
// Calling protocol: a function returns with `s.pc` set to the address execution continues at
// (the guest $ra on a normal return, an exception vector, or a target nobody compiled). A caller
// resumes after a call only when `s.pc` equals its own return address; anything else unwinds to
// the interpreter, which continues from `s.pc`.
//
// Time (engine integration): every translated instruction adds 1 to `s.cycles` after it executes,
// and a data load from main RAM adds `ram_stall` (the interpreter's model). When `cycles` reaches
// `deadline` the code calls the host's `service` (devices, IRQs, frame end); if it asks to stop,
// `s.pc` (and `next_pc` for a stop inside a delay slot) says where the interpreter resumes and
// `stopped` makes every caller unwind.

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

/// One generated image: `hash` (FNV-1a 64) of the `size` bytes it was generated from at `base`.
/// The code only depends on the words it translated (`code`: [begin, end) address pairs, delay
/// slots included; `words`: their values, in order), so the host keys it by `code_hash`, the
/// CodeHash of those words: data the program changes inside the image does not retire the code.
struct Module {
    const char* name;
    uint32_t base, size;
    uint64_t hash;
    const FunctionEntry* entries;   // function entries (f_*)
    size_t count;
    const FunctionEntry* resume;    // mid-function resume points (r_*: continue at s.pc)
    size_t resume_count;
    const uint32_t* code;           // translated words: code_count [begin, end) pairs
    size_t code_count;
    const uint32_t* words;          // the translated words as generated, range after range
    uint64_t code_hash;
};

inline uint64_t Fnv1a64(const uint8_t* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

/// FNV-1a 64 over the translated words of `m` as they are in `image` (loaded at m.base).
inline uint64_t CodeHash(const Module& m, const uint8_t* image) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < m.code_count; ++i)
        h = Fnv1a64(image + (m.code[2 * i] - m.base), m.code[2 * i + 1] - m.code[2 * i], h);
    return h;
}

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
    // Flat address -> code map (wins over `dispatcher`): map[(pc - map_lo) / 4] for map_words words.
    // The host keeps it to the modules whose code is in RAM right now (overlays come and go).
    const RecompFn* map = nullptr;
    uint32_t map_lo = 0, map_words = 0;
    uint64_t fallbacks = 0;  // dispatches that found no compiled function

    // Fast paths (null = everything goes through read/write): 2 MB main RAM mirrored over the
    // first 8 MB of each segment, and the 1 KB scratchpad at 1F800000h.
    uint8_t* ram = nullptr;
    uint8_t* scratch = nullptr;
    // One bit per RAM word (bit w & 31 of watch[w >> 5]): translated code of a live module. A store
    // there calls code_write(offset) after it lands, so the host can retire that code.
    const uint32_t* watch = nullptr;
    void (*code_write)(CpuState& s, uint32_t ram_offset) = nullptr;
    uint32_t ram_stall = 0;   // extra cycles per data load from main RAM
    uint64_t stalls = 0;      // stall cycles added so far (part of `cycles`)

    uint64_t cycles = 0;
    uint64_t deadline = ~0ull;
    bool (*service)(CpuState&) = nullptr;  // true = stop here
    // Dispatch miss: the host may run `pc` itself (HLE vector) and return true with `pc` where
    // execution continues (and `stopped` if it must hand back now).
    bool (*external)(CpuState&) = nullptr;
    void* host = nullptr;
    bool stopped = false;
    uint32_t next_pc = 0;     // nonzero: stopped in a delay slot at `pc`, the branch continues here
    uint32_t load_reg = 0, load_val = 0;  // stopped right after a delayed load: still in flight
    uint32_t pair_prev = 0;   // register value before a chain of loads into it (LWL/LWR pairs)
};

inline uint32_t LoadLe(const uint8_t* p, uint32_t size) {
    uint32_t v = p[0];
    if (size >= 2) v |= static_cast<uint32_t>(p[1]) << 8;
    if (size == 4) v |= (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return v;
}
inline void StoreLe(uint8_t* p, uint32_t v, uint32_t size) {
    p[0] = static_cast<uint8_t>(v);
    if (size >= 2) p[1] = static_cast<uint8_t>(v >> 8);
    if (size == 4) { p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24); }
}

// LWC2/SWC2 and SB/LB need no alignment; an access that would cross the end of RAM wraps
// byte by byte, as Engine::ReadBus does, and one crossing the scratchpad end takes the callback.
inline uint32_t Read(CpuState& s, uint32_t address, uint32_t size) {
    const uint32_t p = address & 0x1FFFFFFFu;
    if (s.ram && p < 0x00800000u) {
        s.cycles += s.ram_stall;
        s.stalls += s.ram_stall;
        const uint32_t o = p & 0x1FFFFFu;
        if (o + size <= 0x200000u) return LoadLe(s.ram + o, size);
        uint32_t v = 0;
        for (uint32_t i = 0; i < size; ++i) v |= static_cast<uint32_t>(s.ram[(o + i) & 0x1FFFFFu]) << (8 * i);
        return v;
    }
    if (s.scratch && p - 0x1F800000u <= 0x400u - size) return LoadLe(s.scratch + (p & 0x3FFu), size);
    return s.read(s.mem, address, size);
}
inline void Write(CpuState& s, uint32_t address, uint32_t value, uint32_t size) {
    const uint32_t p = address & 0x1FFFFFFFu;
    if (s.ram && p < 0x00800000u) {
        const uint32_t o = p & 0x1FFFFFu;
        if (o + size <= 0x200000u) {
            StoreLe(s.ram + o, value, size);
            if (s.watch && ((s.watch[o >> 7] >> ((o >> 2) & 31u)) & 1u)) s.code_write(s, o);
            return;
        }
        for (uint32_t i = 0; i < size; ++i) {
            const uint32_t b = (o + i) & 0x1FFFFFu;
            s.ram[b] = static_cast<uint8_t>(value >> (8 * i));
            if (s.watch && ((s.watch[b >> 7] >> ((b >> 2) & 31u)) & 1u)) s.code_write(s, b);
        }
        return;
    }
    if (s.scratch && p - 0x1F800000u <= 0x400u - size) { StoreLe(s.scratch + (p & 0x3FFu), value, size); return; }
    s.write(s.mem, address, value, size);
}

/// Called by generated code once `cycles >= deadline`, before the instruction at `pc`
/// (`next`: nonzero when `pc` is a delay slot). True = return to the host now.
inline bool Stop(CpuState& s, uint32_t pc, uint32_t next = 0) {
    if (!s.service || !s.service(s)) return false;
    s.pc = pc;
    s.next_pc = next;
    s.stopped = true;
    return true;
}

/// Stop() right after a load into `reg` that the interpreter would still hold in its delay:
/// `reg` gets its value from before the load back and the loaded value is reported in flight.
inline bool StopLoad(CpuState& s, uint32_t pc, uint32_t reg, uint32_t prev) {
    if (!Stop(s, pc)) return false;
    s.load_reg = reg;
    s.load_val = s.gpr[reg];
    s.gpr[reg] = prev;
    return true;
}

/// Same as CpuContext::RaiseException: EPC/BD, ExcCode, KU/IE push, pc = vector (BEV).
/// `pc` is the address of the faulting instruction; `delay_slot` when it sits in a delay slot.
void RaiseException(CpuState& s, uint32_t exc_code, uint32_t pc, bool delay_slot);

/// Continue at `target`: run its compiled function if there is one (`map`, else `dispatcher`),
/// otherwise offer it to `external`, else leave `s.pc` there for the interpreter and count a fallback.
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
