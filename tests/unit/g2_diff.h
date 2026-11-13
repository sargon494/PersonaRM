#pragma once

// Gate G2 differential harness: the same initial state runs (a) in the interpreter and (b) in
// recompiled code (falling back to the interpreter where nothing was compiled); final registers,
// COP0, GTE and RAM must match exactly. Used by unit_test_recomp_g2 and by local real-data checks.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cpu/cpu_context.h"
#include "recomp/runtime/recomp_runtime.h"

namespace g2 {

constexpr uint32_t kRamSize = 0x200000u;
constexpr uint32_t kSentinel = 0x801FFFF0u;   // $ra on entry: holds a nop, reaching it ends the run
constexpr uint32_t kVector = 0x80000080u;     // exception vector (BEV = 0): also ends the run
constexpr uint32_t kStack = 0x801FFE00u;

struct Final {
    personarm::CpuRegisters regs;
    uint32_t end_pc = 0;
    std::vector<uint8_t> ram;
    uint32_t gte_data[32] = {}, gte_ctrl[32] = {};
    uint64_t fallbacks = 0;
    bool timeout = false;
};

inline uint32_t RamRead(const std::vector<uint8_t>& ram, uint32_t a, uint32_t size) {
    a &= kRamSize - 1;
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; ++i) v |= static_cast<uint32_t>(ram[(a + i) & (kRamSize - 1)]) << (8 * i);
    return v;
}
inline void RamWrite(std::vector<uint8_t>& ram, uint32_t a, uint32_t v, uint32_t size) {
    a &= kRamSize - 1;
    for (uint32_t i = 0; i < size; ++i) ram[(a + i) & (kRamSize - 1)] = static_cast<uint8_t>(v >> (8 * i));
}

// Runs the interpreter from `cpu`'s state until the sentinel or the exception vector, then one
// more step so a load still in its delay is written back.
inline bool InterpretToEnd(personarm::CpuContext& cpu, uint32_t& end_pc, uint64_t max_steps = 2000000) {
    for (uint64_t i = 0; i < max_steps; ++i) {
        const uint32_t pc = cpu.GetPC();
        if (pc == kSentinel || pc == kVector) { end_pc = pc; cpu.Step(); return true; }
        cpu.Step();
    }
    return false;
}

inline void Hook(personarm::CpuContext& cpu, std::vector<uint8_t>& ram) {
    cpu.SetMemoryHooks([&ram](uint32_t a, uint32_t s) { return RamRead(ram, a, s); },
                       [&ram](uint32_t a, uint32_t v, uint32_t s) { RamWrite(ram, a, v, s); });
}

inline void Snapshot(const personarm::Gte& g, Final& f) {
    for (uint32_t i = 0; i < 32; ++i) { f.gte_data[i] = g.ReadDataRegister(i); f.gte_ctrl[i] = g.ReadControlRegister(i); }
}

inline Final RunInterpreter(const personarm::CpuRegisters& init, const std::vector<uint8_t>& ram0, const personarm::Gte& gte0) {
    Final f;
    f.ram = ram0;
    personarm::CpuContext cpu;
    Hook(cpu, f.ram);
    cpu.SetRegisters(init);
    cpu.GetGTE() = gte0;
    f.timeout = !InterpretToEnd(cpu, f.end_pc);
    f.regs = cpu.GetRegisters();
    Snapshot(cpu.GetGTE(), f);
    return f;
}

inline Final RunRecompiled(const personarm::CpuRegisters& init, const std::vector<uint8_t>& ram0, const personarm::Gte& gte0,
                           const personarm::recomp::Dispatcher& disp) {
    using namespace personarm::recomp;
    Final f;
    f.ram = ram0;
    personarm::Gte gte = gte0;
    CpuState s;
    std::memcpy(s.gpr, init.gpr.data(), sizeof s.gpr);
    s.hi = init.hi; s.lo = init.lo; s.sr = init.cp0_status; s.cause = init.cp0_cause; s.epc = init.cp0_epc;
    s.gte = &gte;
    s.mem = &f.ram;
    s.read = [](void* m, uint32_t a, uint32_t sz) { return RamRead(*static_cast<std::vector<uint8_t>*>(m), a, sz); };
    s.write = [](void* m, uint32_t a, uint32_t v, uint32_t sz) { RamWrite(*static_cast<std::vector<uint8_t>*>(m), a, v, sz); };
    s.dispatcher = &disp;
    Dispatch(s, init.pc);
    f.fallbacks = s.fallbacks;

    personarm::CpuRegisters r;
    std::memcpy(r.gpr.data(), s.gpr, sizeof s.gpr);
    r.hi = s.hi; r.lo = s.lo; r.pc = s.pc; r.cp0_status = s.sr; r.cp0_cause = s.cause; r.cp0_epc = s.epc;
    if (s.pc == kSentinel || s.pc == kVector) {
        f.end_pc = s.pc;
        f.regs = r;
        Snapshot(gte, f);
        return f;
    }
    // Nothing compiled at s.pc: the interpreter takes over from here (RC: interpreter fallback).
    personarm::CpuContext cpu;
    Hook(cpu, f.ram);
    cpu.SetRegisters(r);
    cpu.GetGTE() = gte;
    f.timeout = !InterpretToEnd(cpu, f.end_pc);
    f.regs = cpu.GetRegisters();
    Snapshot(cpu.GetGTE(), f);
    return f;
}

// Empty when equal; otherwise the first difference found.
inline std::string Diff(const Final& a, const Final& b) {
    auto hex = [](uint32_t v) { char s[12]; std::snprintf(s, sizeof s, "%08X", v); return std::string(s); };
    if (a.timeout || b.timeout) return "timeout";
    if (a.end_pc != b.end_pc) return "end pc " + hex(a.end_pc) + " vs " + hex(b.end_pc);
    for (int i = 0; i < 32; ++i)
        if (a.regs.gpr[i] != b.regs.gpr[i]) return "r" + std::to_string(i) + " " + hex(a.regs.gpr[i]) + " vs " + hex(b.regs.gpr[i]);
    if (a.regs.hi != b.regs.hi) return "hi";
    if (a.regs.lo != b.regs.lo) return "lo";
    if (a.regs.cp0_status != b.regs.cp0_status) return "sr";
    if (a.regs.cp0_cause != b.regs.cp0_cause) return "cause " + hex(a.regs.cp0_cause) + " vs " + hex(b.regs.cp0_cause);
    if (a.regs.cp0_epc != b.regs.cp0_epc) return "epc " + hex(a.regs.cp0_epc) + " vs " + hex(b.regs.cp0_epc);
    for (int i = 0; i < 32; ++i) {
        if (a.gte_data[i] != b.gte_data[i]) return "gte data " + std::to_string(i);
        if (a.gte_ctrl[i] != b.gte_ctrl[i]) return "gte ctrl " + std::to_string(i);
    }
    if (a.ram != b.ram) {
        for (uint32_t i = 0; i < kRamSize; ++i)
            if (a.ram[i] != b.ram[i]) return "ram at " + hex(0x80000000u + i);
    }
    return {};
}

} // namespace g2
