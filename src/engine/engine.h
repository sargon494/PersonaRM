#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "cpu/cpu_context.h"
#include "rendering/ps1_gpu.h"
#include "audio/spu_emulator.h"
#include "loader/ps1_bios_hle.h"
#include "loader/disc_reader.h"
#include "loader/exe_loader.h"
#include "core/memory_manager.h"
#include "input/input_manager.h"

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 Hardware Clock Rates & Timings
// ───────────────────────────────────────────────────────────────────────────

constexpr uint32_t PS1_CPU_CLOCK_HZ    = 33868800; // 33.8688 MHz
constexpr uint32_t PS1_CYCLES_PER_FRAME_NTSC = 564480; // ~60 Hz (33868800 / 60)
constexpr size_t   PS1_MAIN_RAM_SIZE   = 2 * 1024 * 1024; // 2 MB Main RAM
constexpr size_t   PS1_SCRATCHPAD_SIZE = 1024; // 1 KB Data Scratchpad (D-Cache)

enum class EngineState {
    Uninitialized,
    Idle,
    Running,
    Paused,
    Stopped
};

struct EngineBootConfig {
    std::string disc_path;
    bool enable_bios_hle = true;
    bool fast_boot = true;
};

// ───────────────────────────────────────────────────────────────────────────
// PersonaRM Engine — Core Central System Coordinator
// ───────────────────────────────────────────────────────────────────────────

class Engine {
public:
    static Engine& Instance();

    bool Initialize(const EngineBootConfig& config = EngineBootConfig{});
    void Shutdown();
    void Reset();

    /// Boot game from optical disc image (.iso / .bin / .cue)
    bool BootFromDisc(const std::string& disc_path);

    /// Boot directly from standalone PS-X executable (useful for unit tests & homebrew)
    bool BootExecutable(const uint8_t* exe_buffer, size_t exe_size);

    /// Executes single hardware frame (~564,480 CPU cycles)
    void RunFrame();

    /// Current execution state
    EngineState GetState() const { return m_state; }
    void SetPaused(bool paused);

    /// Hardware subsystem accessors
    CpuContext& GetCpu() { return m_cpu; }
    Ps1Gpu& GetGpu() { return m_gpu; }
    SpuEmulator& GetSpu() { return m_spu; }
    Ps1BiosHle& GetBios() { return m_bios; }

    /// Framebuffer retrieval for rendering presentation
    void GetCurrentFrame(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h);

    uint64_t GetFrameCount() const { return m_frame_counter; }

private:
    Engine();
    ~Engine();

    void SetupMemoryBus();

    // Memory read/write dispatchers mapped to MIPS address space
    uint32_t ReadBus(uint32_t address, uint32_t size);
    void WriteBus(uint32_t address, uint32_t value, uint32_t size);

    EngineState m_state = EngineState::Uninitialized;
    EngineBootConfig m_config;

    // Subsystems
    CpuContext m_cpu;
    Ps1Gpu m_gpu;
    SpuEmulator m_spu;
    Ps1BiosHle& m_bios;
    DiscReader m_disc_reader;

    // Physical Memory Buffers
    std::vector<uint8_t> m_ram;        // 2 MB Main RAM
    std::vector<uint8_t> m_scratchpad; // 1 KB D-Cache Scratchpad

    uint64_t m_frame_counter = 0;
};

} // namespace personarm
