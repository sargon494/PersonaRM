#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
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
#include "trace/trace.h"
#include "hw/cdrom.h"
#include "hw/dma.h"
#include "hw/mdec.h"
#include "audio/xa_adpcm.h"

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 Hardware Clock Rates & Timings
// ───────────────────────────────────────────────────────────────────────────

constexpr uint32_t PS1_CPU_CLOCK_HZ    = 33868800; // 33.8688 MHz
constexpr uint32_t PS1_CYCLES_PER_FRAME_NTSC = 564480; // ~60 Hz (33868800 / 60)
// shortcut: 263 NTSC lines spread over the engine's frame budget (hardware: ~2172 cycles/line);
// keeps Timer1 Hblank counts consistent with the end-of-frame VBlank.
constexpr uint32_t kCyclesPerHblank = PS1_CYCLES_PER_FRAME_NTSC / 263;
constexpr size_t   PS1_MAIN_RAM_SIZE   = 2 * 1024 * 1024; // 2 MB Main RAM
constexpr size_t   PS1_SCRATCHPAD_SIZE = 1024; // 1 KB Data Scratchpad (D-Cache)

/// STACK value of a SYSTEM.CNF text (hex), or `fallback` when absent/unparsable.
uint32_t ParseSystemCnfStack(const std::string& cnf, uint32_t fallback);

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
    const CpuContext& GetCpu() const { return m_cpu; }
    Ps1Gpu& GetGpu() { return m_gpu; }
    const Ps1Gpu& GetGpu() const { return m_gpu; }
    SpuEmulator& GetSpu() { return m_spu; }
    const SpuEmulator& GetSpu() const { return m_spu; }
    Ps1BiosHle& GetBios() { return m_bios; }
    hw::CdRom& GetCdRom() { return *m_cdrom; }
    hw::Dma& GetDma() { return *m_dma; }
    hw::Mdec& GetMdec() { return *m_mdec; }

    /// CD XA-ADPCM audio decoded since the last call: 44.1 kHz stereo (L,R interleaved), scaled by
    /// SPUCNT bit 0 (CD audio enable) and the SPU CD volume (1F801DB0h/B2h). Moves the data out.
    // shortcut: kept apart from the SPU voice mix (SpuEmulator::GenerateSamples is not clocked by the
    // engine yet); mix it in there once the SPU runs on emulated time.
    std::vector<int16_t> TakeXaAudio() { return std::move(m_xa_audio); }

    /// Physical bus access as the CPU sees it (tests, tooling).
    uint32_t Read(uint32_t address, uint32_t size) { return ReadBus(address, size); }
    void Write(uint32_t address, uint32_t value, uint32_t size) { WriteBus(address, value, size); }

    /// Called before every guest instruction (null = off). Return false to stop the run
    /// (state becomes Stopped, RunFrame returns at once). For boot instrumentation, not gameplay.
    using StepHook = std::function<bool(CpuContext&)>;
    void SetStepHook(StepHook hook) { m_step_hook = std::move(hook); }
    /// Called on each SPU key-on write (KON 1F801D88h/8Ah) with the voice mask; and on each GP0 word.
    void SetSpuKeyOnHook(std::function<void(uint32_t)> hook) { m_kon_hook = std::move(hook); }

    trace::ITraceSink* GetTraceSink() const { return m_trace; }

    /// Scripted pad input (boot harness): `mask` (digital pad bits, psx-spx "Controllers": bit 0 Select,
    /// 3 Start, 4-7 Up/Right/Down/Left, 8-11 L2/R2/L1/R1, 12-15 Triangle/Circle/Cross/Square) is held
    /// during frames [frame, frame + frames). Merged with the InputManager state.
    struct PadPress { uint16_t mask = 0; uint64_t frame = 0, frames = 5; };
    void SetPadScript(std::vector<PadPress> script) { m_pad_script = std::move(script); }
    /// "<button>@<frame>[+<frames>]", e.g. "start@300" or "cross@600+10". False on bad syntax.
    static bool ParsePadPress(const std::string& text, PadPress& out);
    /// --input-script: one "<frame> <button> [<hold>]" per line, '#' comments. False (and the 1-based
    /// line in bad_line) on the first bad line.
    static bool ParsePadScript(std::istream& in, std::vector<PadPress>& out, size_t& bad_line);
    /// Buttons held by the script at the current frame (bit set = pressed).
    uint16_t ScriptedButtons() const;

    const std::vector<uint8_t>& GetRam() const { return m_ram; }
    void SetRam(const std::vector<uint8_t>& ram) { m_ram = ram; }
    const std::vector<uint8_t>& GetScratchpad() const { return m_scratchpad; }
    void SetScratchpad(const std::vector<uint8_t>& sp) { m_scratchpad = sp; }
    void SetFrameCount(uint64_t count) { m_frame_counter = count; }

    /// Input manager accessor
    InputManager& GetInput() { return m_input; }
    const InputManager& GetInput() const { return m_input; }

    /// Speed & Turbo control (1.0 = normal, 2.0 = 2x, 4.0 = 4x)
    void SetSpeedMultiplier(float mult) { m_speed_multiplier = std::max(0.1f, std::min(mult, 16.0f)); }
    float GetSpeedMultiplier() const { return m_speed_multiplier; }

    /// Framebuffer retrieval for rendering presentation
    void GetCurrentFrame(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h);

    uint64_t GetFrameCount() const { return m_frame_counter; }
    /// Emulated time in CPU cycles: instructions plus memory stall cycles.
    uint64_t GetCycles() const { return m_cpu.GetCycleCount() + m_stall_cycles; }

    /// Execution trace (docs/TRACE_FORMAT.md). Null = off. The sink must outlive its use;
    /// Shutdown() emits `end` and detaches it.
    void SetTraceSink(trace::ITraceSink* sink) { m_trace = sink; m_bios.SetTraceSink(sink); }
    /// Write a PRMTRACE file; it is (re)created with its header at the next boot.
    void SetTraceOutput(const std::string& path) { m_trace_path = path; }

private:
    Engine();
    ~Engine();

    void SetupMemoryBus();

    // Memory read/write dispatchers mapped to MIPS address space
    uint32_t ReadBus(uint32_t address, uint32_t size);
    void WriteBus(uint32_t address, uint32_t value, uint32_t size);

    // Interrupt controller (psx-spx "Interrupt Control") and root counters (psx-spx "Timers").
    void RaiseIrq(uint32_t bit) { m_i_stat |= 1u << bit; UpdateIrqLine(); }
    void UpdateIrqLine() { m_cpu.SetIrqLine((m_i_stat & m_i_mask) != 0); }
    void TickTimers(uint32_t cycles);

    struct RootCounter {
        uint32_t value = 0, mode = 0, target = 0;
        uint32_t prescale = 0;  // sysclk/8 remainder (Timer 2)
        bool irq_done = false;  // one-shot mode already fired since the last MODE write
    };
    uint32_t m_i_stat = 0; // 1F801070h
    uint32_t m_i_mask = 0; // 1F801074h
    std::array<RootCounter, 3> m_timers{};
    void TickDevices(uint32_t cycles);
    void WireDevices();

    // DMA view of main RAM (word access, physical address masked to 2 MB).
    struct RamDma : hw::DmaMemory {
        std::vector<uint8_t>& ram;
        explicit RamDma(std::vector<uint8_t>& r) : ram(r) {}
        uint32_t ReadWord(uint32_t a) override;
        void WriteWord(uint32_t a, uint32_t v) override;
    };
    uint64_t m_timer_cycles = 0; // GetCycles() value the timers were last advanced to
    // CpuContext counts 1 cycle per instruction; main-RAM data loads stall the R3000 (no D-cache).
    // Without this, libetc VSync()'s spin timeout (n<<15 loop passes) expires before the next
    // VBlank and prints "VSync: timeout" during FMVs. Instruction fetches are taken as I-cache hits.
    // unverified: 4 extra cycles per RAM load is an emulator-folklore figure, not measured.
    static constexpr uint32_t kRamLoadStallCycles = 4;
    uint64_t m_stall_cycles = 0;

    EngineState m_state = EngineState::Uninitialized;
    EngineBootConfig m_config;

    // Subsystems
    CpuContext m_cpu;
    Ps1Gpu m_gpu;
    SpuEmulator m_spu;
    Ps1BiosHle& m_bios;
    DiscReader m_disc_reader;
    InputManager& m_input;
    hw::DiscSectorSource m_cd_source{m_disc_reader};
    std::unique_ptr<RamDma> m_dma_mem;
    std::unique_ptr<hw::CdRom> m_cdrom;
    std::unique_ptr<hw::Dma> m_dma;
    std::unique_ptr<hw::Mdec> m_mdec;
    XaAdpcmDecoder m_xa;
    XaResampler m_xa_resampler;
    std::vector<int16_t> m_xa_pcm, m_xa_out, m_xa_audio;
    void OnXaSector(const uint8_t* raw2352);
    StepHook m_step_hook;
    std::function<void(uint32_t)> m_kon_hook;
    uint32_t m_boot_sp = 0x801FFFF0; // SYSTEM.CNF STACK (BIOS default) when the EXE header has none

    // Physical Memory Buffers
    std::vector<uint8_t> m_ram;        // 2 MB Main RAM
    std::vector<uint8_t> m_scratchpad; // 1 KB D-Cache Scratchpad

    // Quality of Life & Timing
    float m_speed_multiplier = 1.0f;
    // SIO0 (controller / memory card port)
    static constexpr uint32_t kJoyAckCycles = 0x88 * 8 + 0x100;
    uint16_t PadButtons() const;  // scripted + InputManager, bit set = pressed
    void WriteJoyCtrl(uint16_t v);
    void TickJoy(uint32_t cycles);
    uint32_t m_joy_step = 0;  // 0 idle, 1-4 pad read sequence, 5 ignore until deselect
    std::vector<uint8_t> m_joy_rx_fifo;
    uint32_t m_joy_ctrl = 0, m_joy_mode = 0, m_joy_baud = 0;
    bool m_joy_ack_low = false, m_joy_irq = false;
    uint32_t m_joy_ack_timer = 0;  // cycles until /ACK (0 = none pending)
    std::vector<PadPress> m_pad_script;

    uint64_t m_frame_counter = 0;

    trace::ITraceSink* m_trace = nullptr;
    std::string m_trace_path;  // --trace-out
    std::string m_trace_disc;  // header "disc" (product id when recognized)
    std::unique_ptr<std::ofstream> m_trace_file;
    std::unique_ptr<trace::TraceWriter> m_trace_writer;
};

} // namespace personarm

