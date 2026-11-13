#include "recomp_runtime.h"

#include <algorithm>

namespace personarm::recomp {

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
