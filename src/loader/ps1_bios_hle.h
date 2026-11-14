#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include <set>
#include <vector>
#include "cpu/cpu_context.h"
#include "core/memory_manager.h"

namespace personarm {

class DiscReader;
class MemoryCard;
namespace trace { class ITraceSink; }

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
    /// Memory card behind port 00h/"bu00:" (slot 0) or 10h/"bu10:" (slot 1); null = no card inserted.
    void SetMemoryCard(int slot, MemoryCard* card) { m_cards[slot & 1] = card; }

    /// True for every physical address the HLE intercepts before the CPU fetches it
    /// (exception vector 0x80, call vectors A0/B0/C0, kernel stubs/trampolines in low RAM).
    static bool IsHleAddress(uint32_t phys);

    /// Writes the kernel tables (A/B/C function tables, event/interrupt control blocks,
    /// TCB, default exit hook) into guest RAM. Call after RAM is cleared at boot.
    void InstallKernel(uint8_t* ram, size_t ram_size);

    /// I/O register access (I_STAT/I_MASK, GP0) for the kernel's default IRQ handlers. Optional.
    void SetIoHooks(CpuContext::ReadMemoryFunc read_fn, CpuContext::WriteMemoryFunc write_fn) {
        m_io_read = std::move(read_fn); m_io_write = std::move(write_fn);
    }

    /// Optional trace sink (null = off); receives OnBiosCall for every A0/B0/C0 vector call.
    void SetTraceSink(trace::ITraceSink* sink) { m_trace = sink; }

    /// Get total calls intercepted since boot
    uint64_t GetInterceptedCallCount() const { return m_intercept_count; }
    /// Unimplemented calls seen so far, as (vector << 8) | function (e.g. 0xB04A).
    const std::set<uint32_t>& GetUnimplementedCalls() const { return m_logged_unimpl; }

private:
    Ps1BiosHle() = default;
    ~Ps1BiosHle() = default;

    // A guest function the kernel calls (event callbacks, interrupt chain) and how to resume.
    struct Frame {
        bool irq_walk = false;
        // irq_walk: SysEnqIntRP chain position
        int prio = -1;
        uint32_t next = 0, func2 = 0;
        int stage = 0; // 0 = pick next element, 1 = func1 running, 2 = func2 running
        uint32_t visited = 0;
        // call list: run funcs in order, then return ret_v0 to ret_pc
        std::vector<uint32_t> funcs;
        size_t idx = 0;
        uint32_t ret_pc = 0, ret_v0 = 0;
    };

    void EnterInterrupt(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void Resume(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void CallGuest(CpuContext& cpu, uint32_t func, uint32_t a0);
    void FinishCall(CpuContext& cpu, std::vector<uint32_t>& callbacks);
    void RunExitHook(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void ReturnFromException(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void RcntIrq(uint32_t n, CpuContext& cpu, uint8_t* ram, size_t ram_size);
    uint32_t DeliverEvent(uint32_t cls, uint32_t spec, uint8_t* ram, size_t ram_size, std::vector<uint32_t>& callbacks);
    uint32_t EventCall(uint32_t fn, CpuContext& cpu, uint8_t* ram, size_t ram_size, bool& blocked);
    void LogUnimplemented(BiosVector vec, uint32_t fn);
    void CardNoCard(uint32_t cls, uint8_t* ram, size_t ram_size, std::vector<uint32_t>& callbacks);
    MemoryCard* CardAtPort(uint32_t port) const { return (port & 0xF) == 0 && port < 0x20 ? m_cards[port >> 4] : nullptr; }
    void CardSector(bool write, CpuContext& cpu, uint8_t* ram, size_t ram_size, std::vector<uint32_t>& callbacks);
    // "buXY:" files (psx-spx "Memory Card BIOS Functions"): open/create, 128-byte unit read/write, find.
    bool CardFileOpen(const std::string& name, uint32_t mode, CpuContext& cpu);
    bool CardFileIo(bool write, CpuContext& cpu, uint8_t* ram, size_t ram_size, std::vector<uint32_t>& callbacks);
    void CardFind(bool first, CpuContext& cpu, uint8_t* ram, size_t ram_size, std::vector<uint32_t>& callbacks);

    // Vector A Handlers (C Runtime & File I/O)
    void HandleFileOpen(CpuContext& cpu, const uint8_t* ram, size_t ram_size);
    void HandleFileRead(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void HandleFileSeek(CpuContext& cpu);
    void HandleFileWrite(CpuContext& cpu, const uint8_t* ram, size_t ram_size);
    void HandleFileClose(CpuContext& cpu);
    void HandleMemCopy(CpuContext& cpu, uint8_t* ram, size_t ram_size);
    void HandleMemSet(CpuContext& cpu, uint8_t* ram, size_t ram_size);

    void HandleHeapInit(CpuContext& cpu);
    void HandleStdPuts(CpuContext& cpu, const uint8_t* ram, size_t ram_size);
    void HandlePadInit(CpuContext& cpu);

    uint64_t m_intercept_count = 0;
    bool m_initialized = false;
    DiscReader* m_disc_reader = nullptr;
    struct OpenFile { std::vector<uint8_t> data; size_t pos = 0; };
    std::unordered_map<int, OpenFile> m_open_files;
    MemoryCard* m_cards[2] = {nullptr, nullptr};
    struct CardFile { MemoryCard* card = nullptr; std::vector<uint32_t> blocks; uint32_t pos = 0; bool async = false; };
    std::unordered_map<int, CardFile> m_card_files;
    struct CardFindState { MemoryCard* card = nullptr; std::string pattern; uint32_t next = 1; } m_find;
    int m_next_fd = 2;

    std::vector<Frame> m_frames;
    size_t m_irq_frame_base = 0;
    bool m_in_irq = false;
    uint32_t m_irq_bits = 0; // I_STAT & I_MASK at interrupt entry
    std::set<uint32_t> m_logged_unimpl;
    CpuContext::ReadMemoryFunc m_io_read;
    CpuContext::WriteMemoryFunc m_io_write;
    trace::ITraceSink* m_trace = nullptr;
};

} // namespace personarm

