#include "engine/engine.h"
#include "core/logger.h"
#include <cstring>
#include <algorithm>

namespace personarm {

Engine::Engine()
    : m_bios(Ps1BiosHle::Instance()) {
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
            if (m_input.IsActionActive(VirtualAction::MoveUp))    b1 &= ~(1 << 4);
            if (m_input.IsActionActive(VirtualAction::MoveRight)) b1 &= ~(1 << 5);
            if (m_input.IsActionActive(VirtualAction::MoveDown))  b1 &= ~(1 << 6);
            if (m_input.IsActionActive(VirtualAction::MoveLeft))  b1 &= ~(1 << 7);
            m_joy_step = 4;
            m_joy_rx_fifo.push_back(b1);
        } else if (m_joy_step == 4) {
            // Buttons Byte 2 (active low 0=pressed)
            uint8_t b2 = 0xFF;
            if (m_input.IsActionActive(VirtualAction::CameraRotateLeft))  b2 &= ~(1 << 2); // L1
            if (m_input.IsActionActive(VirtualAction::CameraRotateRight)) b2 &= ~(1 << 3); // R1
            if (m_input.IsActionActive(VirtualAction::Menu))              b2 &= ~(1 << 4); // Triangle
            if (m_input.IsActionActive(VirtualAction::Cancel))            b2 &= ~(1 << 5); // Circle
            if (m_input.IsActionActive(VirtualAction::Confirm))           b2 &= ~(1 << 6); // Cross
            if (m_input.IsActionActive(VirtualAction::Map))               b2 &= ~(1 << 7); // Square
            m_joy_step = 0;
            m_joy_rx_fifo.push_back(b2);
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

    if (boot_exe_name.empty()) {
        // Fallback standard for Persona 2: Eternal Punishment NA and JP
        boot_exe_name = "SLUS_011.58"; // NA PS1 product ID
        PRM_LOG_WARN("ENGINE", "Could not parse BOOT from SYSTEM.CNF; defaulting to " + boot_exe_name);
    }

    std::vector<uint8_t> exe_payload;
    if (!m_disc_reader.ReadFile(boot_exe_name, exe_payload) || exe_payload.empty()) {
        // Try Japanese version ID if NA was not found
        boot_exe_name = "SLPS_027.70";
        m_disc_reader.ReadFile(boot_exe_name, exe_payload);
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
    if (m_input.IsActionActive(VirtualAction::Turbo)) {
        effective_multiplier *= 2.0f;
    }

    // Emulate scaled CPU clock cycles (~564,480 at 1x 60 Hz NTSC)
    uint64_t start_cycles = m_cpu.GetCycleCount();
    uint64_t target_cycles = start_cycles +
        static_cast<uint64_t>(PS1_CYCLES_PER_FRAME_NTSC * effective_multiplier);

    while (m_cpu.GetCycleCount() < target_cycles && m_state == EngineState::Running) {
        // Fast BIOS HLE hook check: only intercept at vectors 0xA0, 0xB0, 0xC0
        if (m_config.enable_bios_hle) {
            uint32_t masked_pc = m_cpu.GetPC() & 0x1FFFFFFF;
            if (masked_pc == 0x000000A0 || masked_pc == 0x000000B0 || masked_pc == 0x000000C0) {
                m_bios.HandleBiosHook(m_cpu, m_ram.data(), m_ram.size());
            }
        }

        m_cpu.Step();
    }

    m_frame_counter++;
}

void Engine::GetCurrentFrame(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) {
    m_gpu.OutputFrameToRGBA(out_rgba, out_w, out_h);
}

} // namespace personarm

