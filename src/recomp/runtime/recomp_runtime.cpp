#include "recomp_runtime.h"

#include <algorithm>
#include <cstddef>

namespace personarm::recomp {

// prm_abi.h mirrors these structs for generated C code: same layout, field for field.
#define PRM_SAME(a, b, f) static_assert(offsetof(a, f) == offsetof(b, f), #f " moved: bump PRM_ABI_VERSION and fix prm_abi.h")
PRM_SAME(CpuState, prm_cpu, gpr); PRM_SAME(CpuState, prm_cpu, hi); PRM_SAME(CpuState, prm_cpu, pc);
PRM_SAME(CpuState, prm_cpu, sr); PRM_SAME(CpuState, prm_cpu, badvaddr); PRM_SAME(CpuState, prm_cpu, gte);
PRM_SAME(CpuState, prm_cpu, mem); PRM_SAME(CpuState, prm_cpu, read); PRM_SAME(CpuState, prm_cpu, write);
PRM_SAME(CpuState, prm_cpu, dispatcher); PRM_SAME(CpuState, prm_cpu, map); PRM_SAME(CpuState, prm_cpu, map_lo);
PRM_SAME(CpuState, prm_cpu, map_words); PRM_SAME(CpuState, prm_cpu, fallbacks); PRM_SAME(CpuState, prm_cpu, ram);
PRM_SAME(CpuState, prm_cpu, scratch); PRM_SAME(CpuState, prm_cpu, watch); PRM_SAME(CpuState, prm_cpu, code_write);
PRM_SAME(CpuState, prm_cpu, ram_stall); PRM_SAME(CpuState, prm_cpu, stalls); PRM_SAME(CpuState, prm_cpu, cycles);
PRM_SAME(CpuState, prm_cpu, deadline); PRM_SAME(CpuState, prm_cpu, service); PRM_SAME(CpuState, prm_cpu, external);
PRM_SAME(CpuState, prm_cpu, host); PRM_SAME(CpuState, prm_cpu, stopped); PRM_SAME(CpuState, prm_cpu, next_pc);
PRM_SAME(CpuState, prm_cpu, load_reg); PRM_SAME(CpuState, prm_cpu, load_val); PRM_SAME(CpuState, prm_cpu, pair_prev);
PRM_SAME(CpuState, prm_cpu, cop2); PRM_SAME(CpuState, prm_cpu, dispatch);
static_assert(sizeof(CpuState) == sizeof(prm_cpu) && sizeof(bool) == 1, "CpuState / prm_cpu size");
PRM_SAME(FunctionEntry, prm_entry, address); PRM_SAME(FunctionEntry, prm_entry, fn);
static_assert(sizeof(FunctionEntry) == sizeof(prm_entry), "FunctionEntry / prm_entry size");
PRM_SAME(Module, prm_module, name); PRM_SAME(Module, prm_module, base); PRM_SAME(Module, prm_module, size);
PRM_SAME(Module, prm_module, hash); PRM_SAME(Module, prm_module, entries); PRM_SAME(Module, prm_module, count);
PRM_SAME(Module, prm_module, resume); PRM_SAME(Module, prm_module, resume_count); PRM_SAME(Module, prm_module, code);
PRM_SAME(Module, prm_module, code_count); PRM_SAME(Module, prm_module, words); PRM_SAME(Module, prm_module, code_hash);
static_assert(sizeof(Module) == sizeof(prm_module), "Module / prm_module size");
#undef PRM_SAME

uint32_t Cop2(CpuState& s, uint32_t word, uint32_t value) {
    if (word & 0x02000000u) { s.gte->ExecuteCommand(word); return 0; }
    const uint32_t rd = (word >> 11) & 31u;
    switch ((word >> 21) & 31u) {
        case 0x00: return s.gte->ReadDataRegister(rd);
        case 0x02: return s.gte->ReadControlRegister(rd);
        case 0x04: s.gte->WriteDataRegister(rd, value); return 0;
        case 0x06: s.gte->WriteControlRegister(rd, value); return 0;
        default: return 0;  // no-op in the interpreter
    }
}

void Dispatcher::Add(const FunctionEntry* entries, size_t count) {
    m_entries.insert(m_entries.end(), entries, entries + count);
    std::sort(m_entries.begin(), m_entries.end(),
              [](const FunctionEntry& a, const FunctionEntry& b) { return a.address < b.address; });
}

RecompFn Dispatcher::Find(uint32_t address) const {
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), address,
                               [](const FunctionEntry& e, uint32_t a) { return e.address < a; });
    return (it != m_entries.end() && it->address == address) ? it->fn : nullptr;
}

void RaiseException(CpuState& s, uint32_t exc_code, uint32_t pc, bool delay_slot) {
    // psx-spx "COP0 - Exception Handling"; mirrors CpuContext::RaiseException exactly.
    s.epc = delay_slot ? pc - 4 : pc;
    s.cause = (s.cause & ~0x8000007Cu) | (exc_code << 2) | (delay_slot ? 0x80000000u : 0);
    s.sr = (s.sr & ~0x3Fu) | ((s.sr << 2) & 0x3Fu);
    s.pc = (s.sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
}

void Dispatch(CpuState& s, uint32_t target) {
    s.pc = target;
    RecompFn fn = nullptr;
    if (s.map) {
        const uint32_t i = (target - s.map_lo) >> 2;
        if (!(target & 3u) && i < s.map_words) fn = s.map[i];
    } else if (s.dispatcher) {
        fn = s.dispatcher->Find(target);
    }
    if (fn) fn(s);
    else if (!s.external || !s.external(s)) ++s.fallbacks;
}

} // namespace personarm::recomp
