#include "engine/engine.h"
#include "core/logger.h"
#include <cstring>
#include <algorithm>

namespace personarm {

Engine::Engine()
    : m_bios(Ps1BiosHle::Instance()),
      m_input(InputManager::Instance()) {
    m_ram.resize(PS1_MAIN_RAM_SIZE, 0);
    m_scratchpad.resize(PS1_SCRATCHPAD_SIZE, 0);
}

Engine::~Engine() {
    Shutdown();
}

Engine& Engine::Instance() {
    static Engine s_engine;
    return s_engine;
}

bool Engine::Initialize(const EngineBootConfig& config) {
    m_config = config;
    Reset();
    SetupMemoryBus();

    if (m_config.enable_bios_hle) {
        m_bios.Initialize();
        m_bios.SetDiscReader(&m_disc_reader);
    }

    m_state = EngineState::Idle;
    PRM_LOG_INFO("ENGINE", "PersonaRM Core Engine initialized successfully.");
    return true;
}

void Engine::Reset() {
    std::fill(m_ram.begin(), m_ram.end(), static_cast<uint8_t>(0));
    std::fill(m_scratchpad.begin(), m_scratchpad.end(), static_cast<uint8_t>(0));

    m_cpu.Reset();
    m_gpu.Reset();
    m_spu.Reset();
    m_bios.Reset();
    m_frame_counter = 0;
    m_joy_step = 0;
    m_joy_rx_fifo.clear();
    m_i_stat = 0;
    m_i_mask = 0;
    m_timers = {};
    m_timer_cycles = 0;
    m_state = EngineState::Idle;
}

void Engine::Shutdown() {
    m_state = EngineState::Stopped;
    PRM_LOG_INFO("ENGINE", "PersonaRM Core Engine shutdown.");
}

void Engine::SetPaused(bool paused) {
    if (m_state == EngineState::Running && paused) {
        m_state = EngineState::Paused;
    } else if (m_state == EngineState::Paused && !paused) {
        m_state = EngineState::Running;
    }
}

void Engine::SetupMemoryBus() {
    m_cpu.SetMemoryHooks(
        [this](uint32_t addr, uint32_t size) -> uint32_t {
            return this->ReadBus(addr, size);
        },
        [this](uint32_t addr, uint32_t val, uint32_t size) {
            this->WriteBus(addr, val, size);
        }
    );
}

// ───────────────────────────────────────────────────────────────────────────
// MIPS Physical Memory Map & Bus Routing (PS1 Architecture)
// KUSEG: 0x00000000..0x7FFFFFFF (User memory)
// KSEG0: 0x80000000..0x9FFFFFFF (Kernel cached)
// KSEG1: 0xA0000000..0xBFFFFFFF (Kernel uncached & I/O ports)
// ───────────────────────────────────────────────────────────────────────────

uint32_t Engine::ReadBus(uint32_t address, uint32_t size) {
    uint32_t physical = address & 0x1FFFFFFF; // Strip segment bits

    // 1. Main RAM (2 MB, mirrored across first 8 MB)
    if (physical < 0x00800000) {
        uint32_t ram_offset = physical & (PS1_MAIN_RAM_SIZE - 1);
        if (size == 1) return m_ram[ram_offset];
        if (size == 2) {
            return m_ram[ram_offset] | (static_cast<uint32_t>(m_ram[(ram_offset + 1) & (PS1_MAIN_RAM_SIZE - 1)]) << 8);
        }
        if (size == 4) {
            return m_ram[ram_offset] |
                   (static_cast<uint32_t>(m_ram[(ram_offset + 1) & (PS1_MAIN_RAM_SIZE - 1)]) << 8) |
                   (static_cast<uint32_t>(m_ram[(ram_offset + 2) & (PS1_MAIN_RAM_SIZE - 1)]) << 16) |
                   (static_cast<uint32_t>(m_ram[(ram_offset + 3) & (PS1_MAIN_RAM_SIZE - 1)]) << 24);
        }
    }

    // 2. Data Scratchpad (D-Cache at 0x1F800000..0x1F8003FF)
    if (physical >= 0x1F800000 && physical < 0x1F800400) {
        uint32_t sp_offset = physical & (PS1_SCRATCHPAD_SIZE - 1);
        if (size == 1) return m_scratchpad[sp_offset];
        if (size == 2) {
            return m_scratchpad[sp_offset] | (static_cast<uint32_t>(m_scratchpad[sp_offset + 1]) << 8);
        }
        if (size == 4) {
            return m_scratchpad[sp_offset] |
                   (static_cast<uint32_t>(m_scratchpad[sp_offset + 1]) << 8) |
                   (static_cast<uint32_t>(m_scratchpad[sp_offset + 2]) << 16) |
                   (static_cast<uint32_t>(m_scratchpad[sp_offset + 3]) << 24);
        }
    }

    // 3. Controller / Memory Card Ports (0x1F801040: JOY_DATA, 0x1F801044: JOY_STAT)
    if (physical == 0x1F801040) {
        if (!m_joy_rx_fifo.empty()) {
            uint8_t byte = m_joy_rx_fifo.front();
            m_joy_rx_fifo.erase(m_joy_rx_fifo.begin());
            return byte;
        }
        return 0xFF;
    }
    if (physical == 0x1F801044) {
        // Bit 0: TX Ready (1), Bit 1: RX FIFO Not Empty, Bit 2: TX Empty (1), Bit 7: ACK
        uint32_t stat = 0x05;
        if (!m_joy_rx_fifo.empty()) {
            stat |= 0x02; // RX FIFO has data
            stat |= 0x80; // ACK flag
        }
        return stat;
    }

    // Interrupt controller (psx-spx "Interrupt Control")
    if (physical == 0x1F801070) return m_i_stat;
    if (physical == 0x1F801074) return m_i_mask;

    // Root counters 0-2 (psx-spx "Timers"): +0 counter, +4 mode, +8 target
    if (physical >= 0x1F801100 && physical < 0x1F801130) {
        RootCounter& t = m_timers[(physical >> 4) & 3];
        switch (physical & 0xC) {
            case 0x0: return t.value;
            case 0x4: { uint32_t mode = t.mode; t.mode &= ~0x1800u; return mode; } // bits 11-12 reset after read
            case 0x8: return t.target;
        }
        return 0;
    }

    // 4. GPU Hardware Registers (0x1F801810 / 0x1F801814)
    if (physical == 0x1F801810) {
        return m_gpu.ReadGP0();
    }
    if (physical == 0x1F801814) {
        return m_gpu.ReadGPUSTAT();
    }

    // 5. BIOS ROM Area (512 KB at 0x1FC00000..0x1FC7FFFF)
    if (physical >= 0x1FC00000 && physical < 0x1FC80000) {
        // Return dummy NOP instruction (0x00000000) for unhandled ROM calls
        return 0;
    }

    return 0;
}

void Engine::WriteBus(uint32_t address, uint32_t value, uint32_t size) {
    uint32_t physical = address & 0x1FFFFFFF;

    // 1. Main RAM (2 MB)
    if (physical < 0x00800000) {
        uint32_t ram_offset = physical & (PS1_MAIN_RAM_SIZE - 1);
        if (size == 1) {
            m_ram[ram_offset] = static_cast<uint8_t>(value & 0xFF);
        } else if (size == 2) {
            m_ram[ram_offset] = static_cast<uint8_t>(value & 0xFF);
            m_ram[(ram_offset + 1) & (PS1_MAIN_RAM_SIZE - 1)] = static_cast<uint8_t>((value >> 8) & 0xFF);
        } else if (size == 4) {
            m_ram[ram_offset] = static_cast<uint8_t>(value & 0xFF);
            m_ram[(ram_offset + 1) & (PS1_MAIN_RAM_SIZE - 1)] = static_cast<uint8_t>((value >> 8) & 0xFF);
            m_ram[(ram_offset + 2) & (PS1_MAIN_RAM_SIZE - 1)] = static_cast<uint8_t>((value >> 16) & 0xFF);
            m_ram[(ram_offset + 3) & (PS1_MAIN_RAM_SIZE - 1)] = static_cast<uint8_t>((value >> 24) & 0xFF);
        }
        return;
    }

    // 2. Data Scratchpad
    if (physical >= 0x1F800000 && physical < 0x1F800400) {
        uint32_t sp_offset = physical & (PS1_SCRATCHPAD_SIZE - 1);
        if (size == 1) {
            m_scratchpad[sp_offset] = static_cast<uint8_t>(value & 0xFF);
        } else if (size == 2) {
            m_scratchpad[sp_offset] = static_cast<uint8_t>(value & 0xFF);
            m_scratchpad[sp_offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        } else if (size == 4) {
            m_scratchpad[sp_offset] = static_cast<uint8_t>(value & 0xFF);
            m_scratchpad[sp_offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            m_scratchpad[sp_offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            m_scratchpad[sp_offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        }
        return;
    }

    // 3. Controller Port JOY_DATA (0x1F801040)
    if (physical == 0x1F801040) {
        uint8_t tx = static_cast<uint8_t>(value & 0xFF);
        if (tx == 0x01) {
            m_joy_step = 1;
            m_joy_rx_fifo.push_back(0xFF);
        } else if (tx == 0x42 && m_joy_step == 1) {
            m_joy_step = 2;
            m_joy_rx_fifo.push_back(0x41); // Standard Digital Controller ID
        } else if (m_joy_step == 2) {
            m_joy_step = 3;
            m_joy_rx_fifo.push_back(0x5A); // Header end marker
        } else if (m_joy_step == 3) {
            // Buttons Byte 1 (active low 0=pressed)
            uint8_t b1 = 0xFF;
            if (m_input.IsActionPressed(VirtualAction::MoveUp))    b1 &= ~(1 << 4);
            if (m_input.IsActionPressed(VirtualAction::MoveRight)) b1 &= ~(1 << 5);
            if (m_input.IsActionPressed(VirtualAction::MoveDown))  b1 &= ~(1 << 6);
            if (m_input.IsActionPressed(VirtualAction::MoveLeft))  b1 &= ~(1 << 7);
            m_joy_step = 4;
            m_joy_rx_fifo.push_back(b1);
        } else if (m_joy_step == 4) {
            // Buttons Byte 2 (active low 0=pressed)
            uint8_t b2 = 0xFF;
            if (m_input.IsActionPressed(VirtualAction::CameraRotateLeft))  b2 &= ~(1 << 2); // L1
            if (m_input.IsActionPressed(VirtualAction::CameraRotateRight)) b2 &= ~(1 << 3); // R1
            if (m_input.IsActionPressed(VirtualAction::Menu))              b2 &= ~(1 << 4); // Triangle
            if (m_input.IsActionPressed(VirtualAction::Cancel))            b2 &= ~(1 << 5); // Circle
            if (m_input.IsActionPressed(VirtualAction::Confirm))           b2 &= ~(1 << 6); // Cross
            if (m_input.IsActionPressed(VirtualAction::Map))               b2 &= ~(1 << 7); // Square
            m_joy_step = 0;
            m_joy_rx_fifo.push_back(b2);
        }
        return;
    }

    // Interrupt controller: writing 0 to an I_STAT bit acknowledges it; I_MASK has 11 sources.
    if (physical == 0x1F801070) { m_i_stat &= value; UpdateIrqLine(); return; }
    if (physical == 0x1F801074) { m_i_mask = value & 0x7FFu; UpdateIrqLine(); return; }

    // Root counters: writing MODE resets the counter and sets bit 10 (no IRQ request).
    if (physical >= 0x1F801100 && physical < 0x1F801130) {
        RootCounter& t = m_timers[(physical >> 4) & 3];
        switch (physical & 0xC) {
            case 0x0: t.value = value & 0xFFFFu; break;
            case 0x4: t = RootCounter{0, (value & 0x3FFu) | 0x400u, t.target}; break;
            case 0x8: t.target = value & 0xFFFFu; break;
        }
        return;
    }

    // 4. GPU Hardware Registers (0x1F801810 / 0x1F801814)
    if (physical == 0x1F801810) {
        m_gpu.WriteGP0(value);
        return;
    }
    if (physical == 0x1F801814) {
        m_gpu.WriteGP1(value);
        return;
    }

    // 5. SPU Hardware Registers (0x1F801C00..0x1F801DFF)
    if (physical >= 0x1F801C00 && physical < 0x1F801E00) {
        // SPU register writes
        return;
    }
}

bool Engine::BootFromDisc(const std::string& disc_path) {
    if (!m_disc_reader.Open(disc_path)) {
        PRM_LOG_ERROR("ENGINE", "Failed to open optical disc image: " + disc_path);
        return false;
    }

    // Attempt to detect boot executable directly from SYSTEM.CNF via DiscReader
    std::string boot_exe_name = m_disc_reader.DetectBootExecutable();

    std::vector<uint8_t> exe_payload;
    if (!boot_exe_name.empty()) {
        m_disc_reader.ReadFile(boot_exe_name, exe_payload);
    }

    if (exe_payload.empty()) {
        // SYSTEM.CNF was missing/unreadable: probe the known Persona 2 EP executables
        for (const std::string& candidate : DiscReader::KnownPersona2BootExecutables()) {
            if (m_disc_reader.ReadFile(candidate, exe_payload) && !exe_payload.empty()) {
                PRM_LOG_WARN("ENGINE", "SYSTEM.CNF BOOT unusable; booting known executable " + candidate);
                boot_exe_name = candidate;
                break;
            }
        }
    }

    {
        Persona2DiscInfo disc = DiscReader::IdentifyPersona2(boot_exe_name);
        if (disc.recognized) {
            PRM_LOG_INFO("ENGINE", "Recognized Persona 2 EP disc " + disc.product_id + " (" + disc.region + ", " +
                                   disc.edition + ", disc " + (disc.disc_number ? std::to_string(disc.disc_number) : std::string("?")) + " of 2)");
        }
    }

    if (exe_payload.empty()) {
        PRM_LOG_ERROR("ENGINE", "Failed to read boot executable from disc image.");
        return false;
    }

    return BootExecutable(exe_payload.data(), exe_payload.size());
}

bool Engine::BootExecutable(const uint8_t* exe_buffer, size_t exe_size) {
    Reset();
    SetupMemoryBus();

    PsxExeInfo info{};
    if (!ExeLoader::LoadExe(exe_buffer, exe_size, m_cpu, m_ram, info)) {
        PRM_LOG_ERROR("ENGINE", "Failed to parse and load PS-X executable into RAM.");
        return false;
    }

    m_state = EngineState::Running;
    PRM_LOG_INFO("ENGINE", "Boot sequence completed. Engine state: RUNNING. Entry Point: 0x" +
                           std::to_string(info.entry_point));
    return true;
}

void Engine::RunFrame() {
    if (m_state != EngineState::Running) {
        return;
    }

    // Dynamic Turbo Multiplier: if VirtualAction::Turbo is active, boost by 2x
    float effective_multiplier = m_speed_multiplier;
    if (m_input.IsActionPressed(VirtualAction::Turbo)) {
        effective_multiplier *= 2.0f;
    }

    // Emulate scaled CPU clock cycles (~564,480 at 1x 60 Hz NTSC)
    uint64_t start_cycles = m_cpu.GetCycleCount();
    uint64_t target_cycles = start_cycles +
        static_cast<uint64_t>(PS1_CYCLES_PER_FRAME_NTSC * effective_multiplier);

    while (m_cpu.GetCycleCount() < target_cycles && m_state == EngineState::Running) {
        // Fast BIOS HLE hook check: intercept at the exception vector 0x80 and at 0xA0, 0xB0, 0xC0
        if (m_config.enable_bios_hle) {
            uint32_t masked_pc = m_cpu.GetPC() & 0x1FFFFFFF;
            if (masked_pc == 0x00000080 && ((m_cpu.GetRegisters().cp0_cause >> 2) & 0x1Fu) == 0) {
                // shortcut: no kernel IRQ chain yet, so every enabled pending IRQ is acknowledged
                // and dropped here (else Int would re-fire forever); upgrade with SVC-031.
                m_i_stat &= ~m_i_mask;
                UpdateIrqLine();
            }
            if (masked_pc == 0x00000080 || masked_pc == 0x000000A0 || masked_pc == 0x000000B0 || masked_pc == 0x000000C0) {
                m_bios.HandleBiosHook(m_cpu, m_ram.data(), m_ram.size());
            }
        }

        m_cpu.Step();
        uint64_t now = m_cpu.GetCycleCount();
        TickTimers(static_cast<uint32_t>(now - m_timer_cycles));
        m_timer_cycles = now;
    }

    // VBlank IRQ0 once per frame. shortcut: raised at the end of the frame's cycle budget rather
    // than at scanline 240 of 263, and GPUSTAT bit 31 (interlace field) does not toggle;
    // upgrade with a scanline-based scheduler.
    RaiseIrq(0);
    m_frame_counter++;
}

void Engine::TickTimers(uint32_t cycles) {
    for (uint32_t i = 0; i < 3; ++i) {
        RootCounter& t = m_timers[i];
        uint32_t ticks = cycles;
        if (i == 2 && (t.mode & 0x200u)) { // Timer 2 clock source 2/3 = sysclk/8
            t.prescale += cycles;
            ticks = t.prescale / 8;
            t.prescale %= 8;
        }
        // shortcut: Timer0 dotclock / Timer1 hblank sources (mode bit 8) and sync modes (bits 0-2)
        // count as plain sysclk; upgrade when the GPU exposes a dot/line clock.
        while (ticks--) {
            t.value = (t.value + 1) & 0xFFFFu;
            bool irq = false;
            if (t.value == 0xFFFFu) { t.mode |= 0x1000u; irq |= (t.mode & 0x20u) != 0; }
            if (t.value == t.target) {
                t.mode |= 0x800u;
                irq |= (t.mode & 0x10u) != 0;
                // shortcut (from memory, DuckStation-like): reset-at-target wraps to 0 on the tick
                // that reaches target (period = target); verify against hardware tests.
                if (t.mode & 0x8u) t.value = 0;
            }
            if (!irq || (!(t.mode & 0x40u) && t.irq_done)) continue; // bit 6 = 0: one-shot
            t.irq_done = true;
            // bit 7: 0 = short pulse (bit 10 dips to 0 and returns to 1), 1 = toggle bit 10
            if (t.mode & 0x80u) t.mode ^= 0x400u; else t.mode &= ~0x400u;
            if (!(t.mode & 0x400u)) RaiseIrq(4 + i);
            if (!(t.mode & 0x80u)) t.mode |= 0x400u;
        }
    }
}

void Engine::GetCurrentFrame(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) {
    m_gpu.OutputFrameToRGBA(out_rgba, out_w, out_h);
}

} // namespace personarm

