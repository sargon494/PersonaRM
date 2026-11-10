#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include "cpu/cpu_context.h"
#include "core/memory_manager.h"

namespace personarm {

class DiscReader;

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
    bool HandleBiosHook(CpuContext& cpu, uint8_t* ram, size_t ram_size);

    /// Manually execute a BIOS routine by vector and opcode
    bool ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, MemoryManager& memory);
    bool ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, uint8_t* ram, size_t ram_size);

    void SetDiscReader(DiscReader* disc_reader) { m_disc_reader = disc_reader; }

    /// Get total calls intercepted since boot
    uint64_t GetInterceptedCallCount() const { return m_intercept_count; }

private:
    Ps1BiosHle() = default;
    ~Ps1BiosHle() = default;

    // Vector A Handlers (C Runtime & File I/O)
    void HandleFileOpen(CpuContext& cpu, const uint8_t* ram, size_t ram_size);
    void HandleFileRead(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void HandleFileClose(CpuContext& cpu);
    void HandleMemCopy(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void HandleMemSet(CpuContext& cpu, uint8_t* ram, size_t ram_size);

    // Vector B Handlers (Memory heap & Strings)
    void HandleHeapInit(CpuContext& cpu);
    void HandleStdPuts(CpuContext& cpu, const uint8_t* ram, size_t ram_size);

    // Vector C Handlers (Peripherals)
    void HandlePadInit(CpuContext& cpu);

    uint64_t m_intercept_count = 0;
    bool m_initialized = false;
    DiscReader* m_disc_reader = nullptr;
    std::unordered_map<int, std::vector<uint8_t>> m_open_files;
    int m_next_fd = 1;
};

} // namespace personarm

