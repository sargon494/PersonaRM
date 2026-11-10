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

    // 3. GPU Hardware Registers (0x1F801810 / 0x1F801814)
    if (physical == 0x1F801810) {
        return m_gpu.ReadGP0();
    }
    if (physical == 0x1F801814) {
        return m_gpu.ReadGPUSTAT();
    }

    // 4. BIOS ROM Area (512 KB at 0x1FC00000..0x1FC7FFFF)
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

    // 3. GPU Hardware Registers (0x1F801810 / 0x1F801814)
    if (physical == 0x1F801810) {
        m_gpu.WriteGP0(value);
        return;
    }
    if (physical == 0x1F801814) {
        m_gpu.WriteGP1(value);
        return;
    }

    // 4. SPU Hardware Registers (0x1F801C00..0x1F801DFF)
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

    // Emulate ~564,480 CPU clock cycles (one 60 Hz NTSC video frame)
    uint64_t start_cycles = m_cpu.GetCycleCount();
    uint64_t target_cycles = start_cycles + PS1_CYCLES_PER_FRAME_NTSC;

    while (m_cpu.GetCycleCount() < target_cycles && m_state == EngineState::Running) {
        // Check BIOS HLE hooks
        if (m_config.enable_bios_hle) {
            MemoryManager mem_mgr; // HLE wrapper
            m_bios.HandleBiosHook(m_cpu, mem_mgr);
        }

        m_cpu.Step();
    }

    m_frame_counter++;
}

void Engine::GetCurrentFrame(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) {
    m_gpu.OutputFrameToRGBA(out_rgba, out_w, out_h);
}

} // namespace personarm

