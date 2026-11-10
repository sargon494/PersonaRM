#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <functional>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────
// MIPS CPU State (Emotion Engine / R3000A Core)
// ───────────────────────────────────────────────────────────────────────

struct CpuRegisters {
    std::array<uint32_t, 32> gpr{}; // General purpose registers (r0 is always 0)
    uint32_t pc = 0;                 // Program counter
    uint32_t hi = 0;                 // Multiply/Divide High result
    uint32_t lo = 0;                 // Multiply/Divide Low result
    
    // Coprocessor 0 (COP0) State
    uint32_t cp0_status = 0;
    uint32_t cp0_cause = 0;
    uint32_t cp0_epc = 0;
};

// ───────────────────────────────────────────────────────────────────────
// CpuContext Class
// ───────────────────────────────────────────────────────────────────────

class CpuContext {
public:
    CpuContext();
    ~CpuContext() = default;

    void Reset();
    void SetPC(uint32_t address) { m_regs.pc = address; }
    uint32_t GetPC() const { return m_regs.pc; }

    // Register access
    uint32_t GetGPR(uint32_t index) const;
    void SetGPR(uint32_t index, uint32_t value);

    // Memory Bus Hooks
    using ReadMemoryFunc = std::function<uint32_t(uint32_t address, uint32_t size)>;
    using WriteMemoryFunc = std::function<void(uint32_t address, uint32_t value, uint32_t size)>;

    void SetMemoryHooks(ReadMemoryFunc read_fn, WriteMemoryFunc write_fn);

    // Execution
    void Step();
    uint64_t GetCycleCount() const { return m_cycles; }

    const CpuRegisters& GetRegisters() const { return m_regs; }

private:
    void ExecuteInstruction(uint32_t opcode);

    CpuRegisters m_regs;
    uint64_t m_cycles = 0;

    ReadMemoryFunc m_read_memory;
    WriteMemoryFunc m_write_memory;
};

} // namespace personarm
