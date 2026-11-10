#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include "cpu/cpu_context.h"
#include "core/memory_manager.h"

namespace personarm {

enum class BiosVector : uint32_t {
    VectorA = 0xA0, // C Runtime & Low-level File I/O
    VectorB = 0xB0, // Memory heap & String formatting
    VectorC = 0xC0  // Hardware timers & Controller setup
};

struct BiosCallInfo {
    uint32_t vector_addr = 0;
    uint32_t function_code = 0;
    std::string function_name;
};

// ───────────────────────────────────────────────────────────────────────────
// Ps1BiosHle — High-Level Emulation of PlayStation 1 Kernel & BIOS Services
// ───────────────────────────────────────────────────────────────────────────

class Ps1BiosHle {
public:
    static Ps1BiosHle& Instance();

    void Initialize();
    void Reset();

    /// Intercept execution if PC matches a known BIOS entry point (0xA0, 0xB0, 0xC0)
    /// Returns true if a BIOS call was intercepted and handled via HLE.
    bool HandleBiosHook(CpuContext& cpu, MemoryManager& memory);

    /// Manually execute a BIOS routine by vector and opcode
    bool ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, MemoryManager& memory);

    /// Get total calls intercepted since boot
    uint64_t GetInterceptedCallCount() const { return m_intercept_count; }

private:
    Ps1BiosHle() = default;
    ~Ps1BiosHle() = default;

    // Vector A Handlers (C Runtime & File I/O)
    void HandleFileOpen(CpuContext& cpu);
    void HandleFileRead(CpuContext& cpu, MemoryManager& memory);
    void HandleFileClose(CpuContext& cpu);
    void HandleMemCopy(CpuContext& cpu, MemoryManager& memory);
    void HandleMemSet(CpuContext& cpu, MemoryManager& memory);

    // Vector B Handlers (Memory heap & Strings)
    void HandleHeapInit(CpuContext& cpu, MemoryManager& memory);
    void HandleStdPuts(CpuContext& cpu, MemoryManager& memory);

    // Vector C Handlers (Peripherals)
    void HandlePadInit(CpuContext& cpu);

    uint64_t m_intercept_count = 0;
    bool m_initialized = false;
};

} // namespace personarm

