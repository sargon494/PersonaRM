#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <functional>
#include <optional>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// MIPS R3000A CPU State (PlayStation 1 Architecture)
// ───────────────────────────────────────────────────────────────────────────

struct CpuRegisters {
    std::array<uint32_t, 32> gpr{}; // General purpose registers (r0 is always 0)
    uint32_t pc = 0;                 // Program counter
    uint32_t hi = 0;                 // Multiply/Divide High result
    uint32_t lo = 0;                 // Multiply/Divide Low result

    // Coprocessor 0 (COP0 - System Control) State
    uint32_t cp0_status = 0;
    uint32_t cp0_cause = 0;
    uint32_t cp0_epc = 0;
};

// ───────────────────────────────────────────────────────────────────────────
// CpuContext Class — Full MIPS-I Core Interpreter with Delay Slot Support
// ───────────────────────────────────────────────────────────────────────────

class CpuContext {
public:
    CpuContext();
    ~CpuContext() = default;

    void Reset();
    void SetPC(uint32_t address) { m_regs.pc = address; m_next_pc = address + 4; }
    uint32_t GetPC() const { return m_regs.pc; }

    // Register access
    uint32_t GetGPR(uint32_t index) const;
    void SetGPR(uint32_t index, uint32_t value);

    uint32_t GetHI() const { return m_regs.hi; }
    uint32_t GetLO() const { return m_regs.lo; }
    void SetHI(uint32_t val) { m_regs.hi = val; }
    void SetLO(uint32_t val) { m_regs.lo = val; }

    // Memory Bus Hooks (8-bit, 16-bit, 32-bit reads/writes)
    using ReadMemoryFunc = std::function<uint32_t(uint32_t address, uint32_t size)>;
    using WriteMemoryFunc = std::function<void(uint32_t address, uint32_t value, uint32_t size)>;

    void SetMemoryHooks(ReadMemoryFunc read_fn, WriteMemoryFunc write_fn);

    // Execution step
    void Step();
    uint64_t GetCycleCount() const { return m_cycles; }

    const CpuRegisters& GetRegisters() const { return m_regs; }

private:
    void ExecuteInstruction(uint32_t opcode);

    // Branch / Jump delay slot pipeline handling
    void ScheduleBranch(uint32_t target_address);

    CpuRegisters m_regs;
    uint64_t m_cycles = 0;

    uint32_t m_next_pc = 0;
    std::optional<uint32_t> m_branch_target;
    bool m_in_delay_slot = false;

    // Load Delay Slot simulation
    uint32_t m_load_delay_reg = 0;
    uint32_t m_load_delay_val = 0;

    ReadMemoryFunc m_read_memory;
    WriteMemoryFunc m_write_memory;
};

} // namespace personarm
