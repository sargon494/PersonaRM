#include "engine/engine.h"
#include "core/logger.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <sstream>

namespace personarm {

uint32_t ParseSystemCnfStack(const std::string& cnf, uint32_t fallback) {
    // "STACK = 801FFFF0" (hex, psx-spx "CDROM File Formats" SYSTEM.CNF).
    size_t p = cnf.find("STACK");
    if (p == std::string::npos) return fallback;
    p = cnf.find('=', p);
    if (p == std::string::npos) return fallback;
    char* end = nullptr;
    unsigned long v = std::strtoul(cnf.c_str() + p + 1, &end, 16);
    return end == cnf.c_str() + p + 1 || v == 0 ? fallback : static_cast<uint32_t>(v);
}

Engine::Engine()
    : m_bios(Ps1BiosHle::Instance()),
      m_input(InputManager::Instance()) {
    m_ram.resize(PS1_MAIN_RAM_SIZE, 0);
    m_scratchpad.resize(PS1_SCRATCHPAD_SIZE, 0);
    WireDevices();
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
    m_joy_ctrl = m_joy_mode = m_joy_baud = 0;
    m_joy_ack_low = m_joy_irq = false;
    m_joy_ack_timer = 0;
    m_i_stat = 0;
    m_i_mask = 0;
    m_timers = {};
    m_timer_cycles = 0;
    m_due = m_next_event = 0;
    m_lag_at = ~0ull;
    m_stall_cycles = 0;
    m_in_native = false;
    m_recomp_on = false;   // ActivateRecomp rebuilds the registry once the EXE is in RAM
    m_mods.clear();
    m_watch.clear();
    m_native_map.clear();
    m_cand_at.clear();
    m_cand.clear();
    WireDevices();
    m_state = EngineState::Idle;
}

uint32_t Engine::RamDma::ReadWord(uint32_t a) {
    a &= PS1_MAIN_RAM_SIZE - 4;
    const auto& ram = engine.m_ram;
    return ram[a] | (ram[a + 1] << 8) | (ram[a + 2] << 16) | (uint32_t(ram[a + 3]) << 24);
}
void Engine::RamDma::WriteWord(uint32_t a, uint32_t v) {
    a &= PS1_MAIN_RAM_SIZE - 4;
    for (int i = 0; i < 4; ++i) engine.m_ram[a + i] = static_cast<uint8_t>(v >> (8 * i));
    if (!engine.m_watch.empty()) {
        ++engine.m_epoch;   // CD/MDEC data may be new code: modules that did not match may now
        engine.OnCodeWrite(a);
    }
}

// Wiring per the hw/cdrom.h and hw/dma.h header notes. Rebuilt on every Reset (fresh device state).
void Engine::WireDevices() {
    m_dma_mem = std::make_unique<RamDma>(*this);
    m_cdrom = std::make_unique<hw::CdRom>(&m_cd_source);
    m_dma = std::make_unique<hw::Dma>(*m_dma_mem);
    m_cdrom->SetIrqCallback([this] { RaiseIrq(2); });
    m_dma->SetIrqCallback([this] { RaiseIrq(3); });
    m_dma->SetPort(hw::Dma::kGpu, {[this] { return m_gpu.ReadGP0(); }, [this](uint32_t w) { m_gpu.WriteGP0(w); }});
    m_dma->SetPort(hw::Dma::kCdrom, {[this] { return m_cdrom->DmaRead32(); }, nullptr});
    m_dma->SetPort(hw::Dma::kSpu, {[this] {
        uint8_t b[4];
        m_spu.DmaRead(b, 4);
        return b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
    }, [this](uint32_t w) {
        const uint8_t b[4] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24)};
        m_spu.DmaWrite(b, 4);
    }});
    m_mdec = std::make_unique<hw::Mdec>();
    // MDEC DMA is paced by its DRQs: PsyQ starts DMA1 (out) before DMA0 (in).
    m_dma->SetPort(hw::Dma::kMdecIn, {nullptr, [this](uint32_t w) { m_mdec->DmaIn(w); },
                                      [this] { return m_mdec->DataInRequest(); }});
    m_dma->SetPort(hw::Dma::kMdecOut, {[this] { return m_mdec->DmaOut(); }, nullptr,
                                       [this] { return m_mdec->DataOutRequest(); }});
    m_xa.Reset();
    m_xa_resampler.Reset();
    m_cdrom->SetXaCallback([this](const uint8_t* raw) { OnXaSector(raw); });
}

void Engine::OnXaSector(const uint8_t* raw) {
    m_xa_pcm.clear();
    if (!m_xa.DecodeRawSector(raw, m_xa_pcm)) return;
    const XaFormat fmt = XaFormat::FromCodingInfo(raw[0x13]);
    const int ch = fmt.stereo ? 2 : 1;
    m_xa_out.clear();
    m_xa_resampler.Process(m_xa_pcm.data(), m_xa_pcm.size() / ch, ch, fmt.rate, m_xa_out);
    if (ch == 1) {  // mono: same sample on both sides
        m_xa_pcm.clear();
        for (int16_t v : m_xa_out) m_xa_pcm.insert(m_xa_pcm.end(), {v, v});
        m_xa_out.swap(m_xa_pcm);
    }
    m_spu.PushCdAudio(m_xa_out.data(), m_xa_out.size() / 2);  // volume/enable applied in the SPU mix
}

void Engine::TickDevices(uint32_t cycles) {
    TickTimers(cycles);
    TickJoy(cycles);
    m_spu.Tick(cycles);
    m_cdrom->Tick(cycles);
    m_mdec->Tick(cycles);
    m_dma->Tick(cycles);
}

void Engine::Shutdown() {
    if (m_trace) {
        m_trace->OnEnd("quit", static_cast<uint32_t>(m_frame_counter));
        SetTraceSink(nullptr);
    }
    m_trace_writer.reset();
    m_trace_file.reset();
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
    // The HLE kernel's default IRQ handlers read/acknowledge I_STAT like the BIOS code does.
    m_bios.SetIoHooks(
        [this](uint32_t addr, uint32_t size) { return this->ReadBus(addr, size); },
        [this](uint32_t addr, uint32_t val, uint32_t size) { this->WriteBus(addr, val, size); });
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
        if (address != m_cpu.GetPC()) m_stall_cycles += kRamLoadStallCycles;  // data load, not a fetch
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

    // Devices first catch up with the CPU (lazy device time), and may plan a new event afterwards.
    CatchUp();
    const uint32_t v = ReadIo(physical, size);
    ScheduleDevices();
    return v;
}

uint32_t Engine::ReadIo(uint32_t physical, uint32_t size) {
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
        // psx-spx "Controller and Memory Card I/O Ports": bit 0/2 TX ready, 1 RX FIFO not empty,
        // 7 /ACK input low, 9 IRQ7 request. shortcut: baud timer (bits 11-31) reads 0.
        uint32_t stat = 0x05;
        if (!m_joy_rx_fifo.empty()) stat |= 0x02;
        if (m_joy_ack_low) stat |= 0x80;
        if (m_joy_irq) stat |= 0x200;
        return stat;
    }
    if (physical == 0x1F801048) return m_joy_mode;
    if (physical == 0x1F80104A) return m_joy_ctrl;
    if (physical == 0x1F80104E) return m_joy_baud;

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

    // DMA controller 1F801080h..1F8010FFh
    if (physical >= hw::Dma::kBase && physical < hw::Dma::kBase + 0x80) {
        const uint32_t off = physical - hw::Dma::kBase;
        return size == 1 ? m_dma->Read8(off) : size == 2 ? m_dma->Read16(off) : m_dma->Read32(off);
    }
    if (physical >= hw::Mdec::kBase && physical < hw::Mdec::kBase + 8)  // 32-bit-only port
        return m_mdec->Read32(physical - hw::Mdec::kBase);
    // CD-ROM controller 1F801800h..1F801803h
    if (physical >= hw::CdRom::kBase && physical < hw::CdRom::kBase + 4) {
        const uint32_t off = physical - hw::CdRom::kBase;
        return size == 1 ? m_cdrom->Read8(off) : size == 2 ? m_cdrom->Read16(off) : m_cdrom->Read32(off);
    }
    // SPU 1F801C00h..1F801DFFh: 16-bit registers; a 32-bit access is two halfwords.
    if (physical >= 0x1F801C00 && physical < 0x1F801E00) {
        uint32_t v = m_spu.Read16(physical & ~1u);
        if (size == 4) v |= static_cast<uint32_t>(m_spu.Read16(physical + 2)) << 16;
        if (size == 1) v = (physical & 1) ? v >> 8 : v & 0xFF;
        return v;
    }

    // 4. GPU Hardware Registers (0x1F801810 GPUREAD / 0x1F801814 GPUSTAT)
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
        OnRamWord(ram_offset);
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

    CatchUp();
    WriteIo(physical, value, size);
    ScheduleDevices();
}

void Engine::WriteIo(uint32_t physical, uint32_t value, uint32_t size) {
    // 3. SIO0 (psx-spx "Controller and Memory Card I/O Ports" / "Controllers - Communication Sequence"):
    // a digital pad on /JOY1, nothing on /JOY2, no memory card (0x81 gets no /ACK -> "no card").
    if (physical == 0x1F801040) {
        const uint8_t tx = static_cast<uint8_t>(value & 0xFF);
        uint8_t rx = 0xFF;
        bool ack = false;
        m_joy_ack_low = false;
        if ((m_joy_ctrl & 0x2002) == 0x0002) {  // /JOY1 selected
            switch (m_joy_step) {
            case 0: ack = tx == 0x01; m_joy_step = ack ? 1 : 5; break;  // pad address; others ignored
            case 1: ack = tx == 0x42; rx = ack ? 0x41 : 0xFF; m_joy_step = ack ? 2 : 5; break;  // read: ID lo
            case 2: rx = 0x5A; ack = true; m_joy_step = 3; break;                     // ID hi
            case 3: rx = static_cast<uint8_t>(~PadButtons()); ack = true; m_joy_step = 4; break;
            case 4: rx = static_cast<uint8_t>(~PadButtons() >> 8); m_joy_step = 5; break;  // last byte: no /ACK
            default: break;
            }
        }
        if (m_joy_rx_fifo.size() < 8) m_joy_rx_fifo.push_back(rx);
        // shortcut: RX is ready at once; /ACK follows one 8-bit transfer at the usual baud (0x88 * 8) plus a delay.
        m_joy_ack_timer = ack ? kJoyAckCycles : 0;
        return;
    }
    if (physical == 0x1F801048) {
        m_joy_mode = value & 0xFFFF;
        if (size == 4) WriteJoyCtrl(static_cast<uint16_t>(value >> 16));
        return;
    }
    if (physical == 0x1F80104A) { WriteJoyCtrl(static_cast<uint16_t>(value)); return; }
    if (physical == 0x1F80104E) { m_joy_baud = value & 0xFFFF; return; }

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

    if (physical >= hw::Dma::kBase && physical < hw::Dma::kBase + 0x80) {
        const uint32_t off = physical - hw::Dma::kBase;
        if (size == 1) m_dma->Write8(off, static_cast<uint8_t>(value));
        else if (size == 2) m_dma->Write16(off, static_cast<uint16_t>(value));
        else m_dma->Write32(off, value);
        return;
    }
    if (physical >= hw::Mdec::kBase && physical < hw::Mdec::kBase + 8) {
        m_mdec->Write32(physical - hw::Mdec::kBase, value);
        return;
    }
    if (physical >= hw::CdRom::kBase && physical < hw::CdRom::kBase + 4) {
        const uint32_t off = physical - hw::CdRom::kBase;
        if (size == 1) m_cdrom->Write8(off, static_cast<uint8_t>(value));
        else if (size == 2) m_cdrom->Write16(off, static_cast<uint16_t>(value));
        else m_cdrom->Write32(off, value);
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
        // shortcut: an 8-bit store writes the byte zero-extended into its halfword (no read-modify-write).
        const uint32_t reg = physical & ~1u;
        const uint16_t lo = static_cast<uint16_t>(size == 1 && (physical & 1) ? (value & 0xFF) << 8 : value);
        m_spu.Write16(reg, lo);
        if (size == 4) m_spu.Write16(reg + 2, static_cast<uint16_t>(value >> 16));
        if (m_kon_hook) {
            if (reg == 0x1F801D88 && lo) m_kon_hook(lo);
            if (reg == 0x1F801D8A && (lo & 0xFF)) m_kon_hook(static_cast<uint32_t>(lo & 0xFF) << 16);
            if (size == 4 && reg == 0x1F801D88 && (value >> 16)) m_kon_hook((value >> 16 & 0xFF) << 16);
        }
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
    m_boot_sp = 0x801FFFF0;
    {
        std::vector<uint8_t> cnf;
        if (m_disc_reader.ReadFile("SYSTEM.CNF", cnf))
            m_boot_sp = ParseSystemCnfStack(std::string(cnf.begin(), cnf.end()), m_boot_sp);
    }

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
        m_trace_disc = disc.recognized ? disc.product_id : std::string();
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
    if (m_config.enable_bios_hle) {
        m_bios.InstallKernel(m_ram.data(), m_ram.size());
        // The real BIOS boots from CD and leaves the controller's interrupt enable (HINTMSK) at 1Fh;
        // libcd CD_init never writes it and waits for INT3 forever otherwise (from memory, matches
        // other HLE implementations; verify with a real-BIOS trace).
        WriteBus(0x1F801800, 1, 1);
        WriteBus(0x1F801802, 0x1F, 1);
        WriteBus(0x1F801800, 0, 1);
    }

    if (!m_trace_path.empty()) {
        if (m_trace) m_trace->OnEnd("quit", static_cast<uint32_t>(m_frame_counter));
        SetTraceSink(nullptr);
        m_trace_writer.reset();
        m_trace_file = std::make_unique<std::ofstream>(m_trace_path, std::ios::binary | std::ios::trunc);
        if (*m_trace_file) {
            trace::TraceHeader header;
            header.src = "prm";
            header.disc = m_trace_disc;
            m_trace_writer = std::make_unique<trace::TraceWriter>(*m_trace_file, header);
            SetTraceSink(m_trace_writer.get());
        } else {
            PRM_LOG_ERROR("ENGINE", "Cannot open trace output " + m_trace_path);
        }
    }

    PsxExeInfo info{};
    if (!ExeLoader::LoadExe(exe_buffer, exe_size, m_cpu, m_ram, info)) {
        PRM_LOG_ERROR("ENGINE", "Failed to parse and load PS-X executable into RAM.");
        return false;
    }
    // BIOS Exec convention: header SP (base+offset) when nonzero, else the SYSTEM.CNF STACK value.
    if (info.initial_sp == 0) {
        m_cpu.SetGPR(29, m_boot_sp);
        m_cpu.SetGPR(30, m_boot_sp);
    }
    ActivateRecomp();

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
    uint64_t start_cycles = GetCycles();
    uint64_t target_cycles = start_cycles +
        static_cast<uint64_t>(PS1_CYCLES_PER_FRAME_NTSC * effective_multiplier);
    m_frame_target = target_cycles;

    while (GetCycles() < target_cycles && m_state == EngineState::Running) {
        // HLE: intercept the exception vector, the A0/B0/C0 call vectors and the kernel stubs before the
        // CPU fetches them. One HLE step can land on another HLE address (a stub returning to the
        // kernel trampoline), so loop until guest code is next.
        if (m_config.enable_bios_hle) {
            RunHle();
            if (m_state != EngineState::Running) break;
        }

        if (m_step_hook && !m_step_hook(m_cpu)) {
            m_state = EngineState::Stopped;
            break;
        }
        if (m_recomp_on) {
            if (RunNative()) {
                if (m_due >= m_next_event) SyncDevices(m_due);
                continue;
            }
            const uint32_t pc = m_cpu.GetPC(), p = pc & 0x1FFFFFFFu;
            if (p < 0x10000u || p >= 0x1FC00000u) ++m_recomp_stats.interp_kernel;
            else if (std::any_of(m_recomp_ranges.begin(), m_recomp_ranges.end(),
                                 [p](const auto& r) { return p >= r.first && p < r.second; }))
                ++m_recomp_stats.interp_resident;
            else
                ++m_recomp_stats.interp_overlay;
            if (pc - kNativeLo < m_native_map.size() * 4) ++m_recomp_stats.interp_by_pc[(pc - kNativeLo) / 4];
        }
        m_cpu.Step();
        m_due = GetCycles();
        if (m_due >= m_next_event) SyncDevices(m_due);
    }
    SyncDevices(m_due);  // frame end: devices where ticking after every instruction leaves them

    // VBlank IRQ0 once per frame. shortcut: raised at the end of the frame's cycle budget rather
    // than at scanline 240 of 263, and GPUSTAT bit 31 (interlace field) does not toggle;
    // upgrade with a scanline-based scheduler.
    RaiseIrq(0);
    m_frame_counter++;

    if (m_trace) {
        // TRACE_FORMAT.md section 3: `vsync`, then the checkpoint taken at it.
        const uint32_t frame = static_cast<uint32_t>(m_frame_counter);
        m_trace->OnVsync(frame, std::string(), 0);
        const CpuRegisters& regs = m_cpu.GetRegisters();
        const auto& vram = m_gpu.GetVram();
        m_trace->OnCheckpoint("vsync_" + std::to_string(frame), regs.pc, trace::HashGpr(regs),
                              trace::HashMaskedRam(m_ram.data(), m_ram.size(), trace::DefaultRamMasks()),
                              trace::HashBytes(m_scratchpad.data(), m_scratchpad.size()),
                              trace::HashBytes(reinterpret_cast<const uint8_t*>(vram.data()), vram.size() * 2));
    }
}

void Engine::RunHle() {
    for (int guard = 0; Ps1BiosHle::IsHleAddress(m_cpu.GetPC() & 0x1FFFFFFF); ++guard) {
        if (guard == 4096) {
            PRM_LOG_ERROR("ENGINE", "HLE kernel did not return to guest code");
            m_state = EngineState::Stopped;
            break;
        }
        if (m_trace && (m_cpu.GetPC() & 0x1FFFFFFF) == 0x80 &&
            ((m_cpu.GetRegisters().cp0_cause >> 2) & 0x1Fu) == 0) {
            const uint32_t pend = m_i_stat & m_i_mask;
            const char* src = (pend & 1u) ? "vblank" : (pend & 4u) ? "cd" : (pend & 8u) ? "dma"
                            : (pend & 0x70u) ? "timer" : "other";
            m_trace->OnIrq(src, m_cpu.GetRegisters().cp0_epc);
        }
        // A0:02 read / 2A memcpy / 2B memset write guest RAM straight through the pointer: they may
        // load or overwrite code. shortcut: other HLE writes are kernel tables, never code.
        const uint32_t fn = m_cpu.GetGPR(9);
        const bool copies = !m_watch.empty() && (m_cpu.GetPC() & 0x1FFFFFFF) == 0xA0 && (fn == 0x02 || fn == 0x2A || fn == 0x2B);
        const uint32_t dst = m_cpu.GetGPR(fn == 0x02 ? 5 : 4), len = m_cpu.GetGPR(6);
        if (!m_bios.HandleBiosHook(m_cpu, m_ram.data(), m_ram.size())) break;
        if (copies) OnRamRangeWritten(dst, len);
    }
}

void Engine::OnRamRangeWritten(uint32_t addr, uint32_t len) {
    const uint32_t p = addr & 0x1FFFFFFFu;
    if (m_watch.empty() || p >= PS1_MAIN_RAM_SIZE || len == 0) return;
    ++m_epoch;
    const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(PS1_MAIN_RAM_SIZE, uint64_t(p) + len));
    for (uint32_t o = p & ~3u; o < end; o += 4) OnCodeWrite(o);
}

void Engine::ActivateRecomp() {
    m_recomp_ranges.clear();
    m_recomp_stats = RecompStats{};
    m_mods.clear();
    m_cand.clear();
    m_cand_at.clear();
    m_watch.clear();
    m_native_map.clear();
    m_code_changed = false;
    m_recomp_on = !m_recomp_modules.empty();
    if (!m_recomp_on) return;
    // Flat maps over KSEG0 main RAM (the interpreter loop asks at every instruction). Only the exact
    // addresses the code was generated for: a mirror would link other $ra values.
    const size_t words = PS1_MAIN_RAM_SIZE / 4;
    m_native_map.assign(words, nullptr);
    m_watch.assign(words / 32, 0);
    m_cand_at.assign(words, 0);
    m_recomp_stats.interp_by_pc.assign(words, 0);
    m_recomp_stats.map_base = kNativeLo;
    std::map<uint32_t, std::vector<Candidate>> at;  // RAM word -> modules with an entry or resume point there
    for (const recomp::Module* m : m_recomp_modules) {
        const uint32_t lo = m->base - kNativeLo;
        if (m->base < kNativeLo || lo >= PS1_MAIN_RAM_SIZE || m->size > PS1_MAIN_RAM_SIZE - lo || !m->code_count) {
            m_recomp_stats.rejected.push_back(m->name);
            PRM_LOG_WARN("RECOMP", std::string("module ") + m->name + " is not a KSEG0 RAM image; interpreter only");
            continue;
        }
        const uint32_t index = static_cast<uint32_t>(m_mods.size());
        m_mods.push_back({m, lo, lo + m->size});
        std::vector<uint32_t> addrs;
        for (size_t i = 0; i < m->count; ++i) addrs.push_back(m->entries[i].address);
        for (size_t i = 0; i < m->resume_count; ++i) addrs.push_back(m->resume[i].address);
        std::sort(addrs.begin(), addrs.end());
        addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
        size_t r = 0, first = 0;  // code range, index in m->words of its first word
        for (uint32_t a : addrs) {
            while (r < m->code_count && a >= m->code[2 * r + 1]) first += (m->code[2 * r + 1] - m->code[2 * r]) / 4, ++r;
            if (r == m->code_count || a < m->code[2 * r]) continue;  // not a translated word: never entered
            at[(a - kNativeLo) / 4].push_back({index, m->words[first + (a - m->code[2 * r]) / 4]});
        }
    }
    for (const auto& w : at) {
        m_cand_at[w.first] = static_cast<uint32_t>(m_cand.size()) + 1;
        m_cand.insert(m_cand.end(), w.second.begin(), w.second.end());
        m_cand.push_back({~0u, 0});
    }
    // Modules whose code is in RAM at boot (the resident EXE) are live from the start; the others
    // (overlays) wait until their code is loaded.
    for (uint32_t i = 0; i < m_mods.size(); ++i) {
        const LiveModule& lm = m_mods[i];
        if (recomp::CodeHash(*lm.m, m_ram.data() + lm.lo) == lm.m->code_hash) {
            SetLive(i);
            m_recomp_ranges.push_back({lm.lo, lm.hi});
            m_recomp_stats.active.push_back(lm.m->name);
        } else {
            m_recomp_stats.rejected.push_back(lm.m->name);
        }
    }
    PRM_LOG_INFO("RECOMP", std::to_string(m_recomp_stats.active.size()) + " module(s) live at boot, " +
                           std::to_string(m_mods.size() - m_recomp_stats.active.size()) + " waiting for their code");
}

void Engine::SetLive(uint32_t index) {
    LiveModule& lm = m_mods[index];
    const recomp::Module& m = *lm.m;
    for (size_t r = 0; r < m.code_count; ++r)
        for (uint32_t o = m.code[2 * r] - kNativeLo; o < m.code[2 * r + 1] - kNativeLo; o += 4) {
            // A live module never shares a code word with another: that one's bytes are gone.
            if ((m_watch[o >> 7] >> ((o >> 2) & 31u)) & 1u) RetireAt(o);
            m_watch[o >> 7] |= 1u << ((o >> 2) & 31u);
        }
    for (size_t i = 0; i < m.resume_count; ++i) m_native_map[(m.resume[i].address - kNativeLo) / 4] = m.resume[i].fn;
    for (size_t i = 0; i < m.count; ++i) m_native_map[(m.entries[i].address - kNativeLo) / 4] = m.entries[i].fn;
    lm.live = true;
    lm.failed_epoch = ~0ull;
}

void Engine::Retire(uint32_t index) {
    LiveModule& lm = m_mods[index];
    const recomp::Module& m = *lm.m;
    for (size_t r = 0; r < m.code_count; ++r)
        for (uint32_t o = m.code[2 * r] - kNativeLo; o < m.code[2 * r + 1] - kNativeLo; o += 4)
            m_watch[o >> 7] &= ~(1u << ((o >> 2) & 31u));
    for (size_t i = 0; i < m.resume_count; ++i) m_native_map[(m.resume[i].address - kNativeLo) / 4] = nullptr;
    for (size_t i = 0; i < m.count; ++i) m_native_map[(m.entries[i].address - kNativeLo) / 4] = nullptr;
    lm.live = false;
}

void Engine::RetireAt(uint32_t ram_offset) {
    for (uint32_t i = 0; i < m_mods.size(); ++i) {
        if (!m_mods[i].live || ram_offset < m_mods[i].lo || ram_offset >= m_mods[i].hi) continue;
        Retire(i);
        ++m_recomp_stats.invalidations;
    }
    ++m_epoch;
    // Native code of the retired module may be on the native call stack: hand back at the next
    // instruction boundary (exact: the interpreter takes over with the state native code had).
    m_code_changed = true;
    if (m_in_native) m_native.deadline = 0;
}

bool Engine::TryValidate(uint32_t pc) {
    const uint32_t off = pc - kNativeLo;
    if ((off & 3u) || off >= PS1_MAIN_RAM_SIZE || !m_cand_at[off / 4]) return false;
    const uint32_t word = recomp::LoadLe(m_ram.data() + off, 4);
    for (const Candidate* c = &m_cand[m_cand_at[off / 4] - 1]; c->module != ~0u; ++c) {
        LiveModule& lm = m_mods[c->module];
        // Cheap filter first (the word at pc), then the code hash; a mismatch is not rehashed until
        // RAM may hold new code (shortcut: code copied by CPU stores alone does not move m_epoch).
        if (lm.live || c->word != word || lm.failed_epoch == m_epoch) continue;
        if (recomp::CodeHash(*lm.m, m_ram.data() + lm.lo) != lm.m->code_hash) {
            lm.failed_epoch = m_epoch;
            ++m_recomp_stats.hash_mismatches;
            continue;
        }
        SetLive(c->module);
        ++m_recomp_stats.validations;
        ++m_recomp_stats.loads[lm.m->name];
        return m_native_map[off / 4] != nullptr;
    }
    return false;
}

bool Engine::RunNative() {
    const uint32_t pc = m_cpu.GetPC();
    const uint32_t off = pc - kNativeLo;
    if ((off & 3u) || off >= PS1_MAIN_RAM_SIZE) return false;
    recomp::RecompFn fn = m_native_map[off / 4];
    if (!fn) {
        if (!m_cand_at[off / 4] || !TryValidate(pc)) return false;
        fn = m_native_map[off / 4];
    }
    const CpuRegisters& r = m_cpu.GetRegisters();
    // An IRQ due at this boundary is the interpreter's to take; a load in flight would land after
    // the first instruction there but at once in generated code.
    if (!m_cpu.AtCleanBoundary() || ((r.cp0_status & 1u) && (r.cp0_status & r.cp0_cause & 0xFF00u))) {
        ++m_recomp_stats.blocked;
        return false;
    }
    recomp::CpuState& s = m_native;
    CpuToNative();
    s.gte = &m_cpu.GetGTE();
    s.mem = this; s.host = this;
    s.read = NativeRead; s.write = NativeWrite; s.service = NativeService; s.external = NativeExternal;
    s.ram = m_ram.data(); s.scratch = m_scratchpad.data(); s.ram_stall = kRamLoadStallCycles;
    s.dispatcher = nullptr;
    s.map = m_native_map.data(); s.map_lo = kNativeLo; s.map_words = static_cast<uint32_t>(m_native_map.size());
    s.watch = m_watch.data(); s.code_write = NativeCodeWrite;
    s.fallbacks = 0; s.stalls = 0; s.stopped = false; s.next_pc = 0; s.load_reg = 0;
    const uint64_t start = GetCycles();
    s.cycles = start;
    m_lag_at = start;
    m_code_changed = false;
    s.deadline = std::min(m_frame_target, m_recomp_quantum > 1 ? start + m_recomp_quantum : m_next_event);

    m_in_native = true;
    fn(s);
    m_due = DueTime();
    m_in_native = false;

    NativeToCpu();
    if (s.next_pc) m_cpu.SetPCInDelaySlot(s.pc, s.next_pc);
    if (s.load_reg) m_cpu.SetPendingLoad(s.load_reg, s.load_val);
    // Same split as the interpreter: instructions in the CPU counter, RAM load stalls in the engine's.
    m_cpu.AddCycles(s.cycles - start - s.stalls);
    m_stall_cycles += s.stalls;

    ++m_recomp_stats.entries;
    m_recomp_stats.native_instructions += s.cycles - start - s.stalls;
    m_recomp_stats.dispatch_misses += s.fallbacks;
    if (s.stopped) ++m_recomp_stats.stops;
    else ++m_recomp_stats.exits[s.pc];
    return true;
}

void Engine::CpuToNative() {
    const CpuRegisters& r = m_cpu.GetRegisters();
    recomp::CpuState& s = m_native;
    std::copy(r.gpr.begin(), r.gpr.end(), s.gpr);
    s.hi = r.hi; s.lo = r.lo; s.pc = r.pc;
    s.sr = r.cp0_status; s.cause = r.cp0_cause; s.epc = r.cp0_epc; s.badvaddr = m_cpu.GetBadVaddr();
}

void Engine::NativeToCpu() {
    const recomp::CpuState& s = m_native;
    CpuRegisters out = m_cpu.GetRegisters();
    std::copy(s.gpr, s.gpr + 32, out.gpr.begin());
    out.gpr[0] = 0;
    out.hi = s.hi; out.lo = s.lo; out.pc = s.pc;
    out.cp0_status = s.sr; out.cp0_cause = s.cause; out.cp0_epc = s.epc;
    m_cpu.SetRegisters(out);
    m_cpu.SetBadVaddr(s.badvaddr);
}

// A native jump nobody compiled at that address in the live map. An HLE vector (A0/B0/C0 call,
// kernel stub) runs the same HLE the interpreter loop runs between two instructions (0 cycles),
// then native code continues where it returns. Code of a module that just arrived in RAM (an
// overlay called right after its load) becomes live and runs at once.
bool Engine::NativeExternal(recomp::CpuState& s) {
    Engine& e = *static_cast<Engine*>(s.host);
    if (!e.m_config.enable_bios_hle || !Ps1BiosHle::IsHleAddress(s.pc & 0x1FFFFFFF)) {
        if (!e.TryValidate(s.pc)) return false;
        e.m_native_map[(s.pc - kNativeLo) / 4](s);
        return true;
    }
    e.m_due = e.DueTime();
    e.m_in_native = false;
    e.NativeToCpu();
    const uint64_t stall_before = e.m_stall_cycles;
    e.RunHle();
    // HLE reads of RAM through the bus stall like guest loads: fold them into native time, which
    // RunNative adds to the engine counters on exit. They end no instruction: devices do not see
    // them until the next one ends (m_lag_at), as in the interpreter loop.
    const uint64_t hle_stall = e.m_stall_cycles - stall_before;
    e.m_stall_cycles = stall_before;
    s.cycles += hle_stall;
    s.stalls += hle_stall;
    e.m_lag_at = s.cycles;
    e.CpuToNative();
    e.m_in_native = true;
    s.deadline = std::min(s.deadline, e.m_next_event);  // HLE I/O may have planned a device event
    ++e.m_recomp_stats.hle_calls;
    // The interpreter would check for an IRQ before the next instruction: hand back to it.
    if (e.m_state != EngineState::Running || e.m_code_changed || ((s.sr & 1u) && (s.sr & s.cause & 0xFF00u)))
        s.stopped = true;
    return true;
}

// Called at the end of an instruction once s.cycles reaches the deadline: the next device event,
// the frame end, an IRQ unmasked (deadline 0), code retired, or every `quantum` cycles.
bool Engine::NativeService(recomp::CpuState& s) {
    Engine& e = *static_cast<Engine*>(s.host);
    e.SyncDevices(s.cycles);
    if (e.m_code_changed || s.cycles >= e.m_frame_target) return true;
    if ((s.sr & 1u) && (s.sr & s.cause & 0xFF00u)) return true;  // the interpreter delivers it
    s.deadline = std::min(e.m_frame_target, e.m_recomp_quantum > 1 ? s.cycles + e.m_recomp_quantum : e.m_next_event);
    return false;
}

// Generated code reaches here only for addresses outside RAM and the scratchpad (its fast paths);
// the bus brings the devices to the end of the last instruction first (CatchUp).
uint32_t Engine::NativeRead(void* engine, uint32_t address, uint32_t size) {
    return static_cast<Engine*>(engine)->ReadBus(address, size);
}

void Engine::NativeWrite(void* engine, uint32_t address, uint32_t value, uint32_t size) {
    static_cast<Engine*>(engine)->WriteBus(address, value, size);
}

void Engine::NativeCodeWrite(recomp::CpuState& s, uint32_t ram_offset) {
    static_cast<Engine*>(s.host)->RetireAt(ram_offset);
}

uint64_t Engine::NextDeviceEvent() const {
    uint64_t e = UINT64_MAX;
    for (uint32_t i = 0; i < 3; ++i) {
        const RootCounter& t = m_timers[i];
        if (!(t.mode & 0x30u)) continue;  // no IRQ at target or at FFFFh: nothing to deliver
        uint32_t div = 1;
        if (i == 1 && (t.mode & 0x100u)) div = kCyclesPerHblank;
        if (i == 2 && (t.mode & 0x200u)) div = 8;
        // Increments until the counter next equals the target or FFFFh (checked after each one).
        uint32_t dt = (t.target - t.value) & 0xFFFFu, df = (0xFFFFu - t.value) & 0xFFFFu;
        const uint64_t ticks = std::min(dt ? dt : 0x10000u, df ? df : 0x10000u);
        e = std::min(e, ticks * div - t.prescale);
    }
    if (m_joy_ack_timer) e = std::min<uint64_t>(e, m_joy_ack_timer);
    e = std::min(e, m_cdrom->CyclesToNextEvent());
    e = std::min(e, m_dma->CyclesToNextEvent());
    // SPU (no IRQ modelled) and MDEC (no IRQ; its DRQs only matter to a paced DMA, which reports
    // 1) change nothing the CPU sees except through their registers: they wait for CatchUp.
    return e ? e : 1;
}

void Engine::ScheduleDevices() {
    m_next_event = m_eager_devices ? m_timer_cycles + 1 : m_timer_cycles + std::min<uint64_t>(NextDeviceEvent(), 1ull << 40);
    if (m_in_native) m_native.deadline = std::min(m_native.deadline, m_next_event);
}

void Engine::TickTimers(uint32_t cycles) {
    for (uint32_t i = 0; i < 3; ++i) {
        RootCounter& t = m_timers[i];
        // Clock source (psx-spx "Timers", mode bits 8-9): Timer1 1/3 = Hblank, Timer2 2/3 = sysclk/8.
        // libetc VSync() spins until two reads of Timer1 agree, so a sysclk Timer1 hangs it.
        uint32_t div = 1;
        if (i == 1 && (t.mode & 0x100u)) div = kCyclesPerHblank;
        if (i == 2 && (t.mode & 0x200u)) div = 8;
        uint32_t ticks = cycles;
        if (div > 1) {
            t.prescale += cycles;
            ticks = t.prescale / div;
            t.prescale %= div;
        }
        // shortcut: Timer0 dotclock source (mode bit 8) and sync modes (bits 0-2) count as plain
        // sysclk; upgrade when the GPU exposes a dot clock.
        while (ticks) {
            // Increments that reach neither the target nor FFFFh change nothing else: take them at once
            // (lazy device time hands over long batches).
            const uint32_t dt = (t.target - t.value) & 0xFFFFu, df = (0xFFFFu - t.value) & 0xFFFFu;
            const uint32_t d = std::min(dt ? dt : 0x10000u, df ? df : 0x10000u);
            if (ticks < d) { t.value = (t.value + ticks) & 0xFFFFu; break; }
            ticks -= d;
            t.value = (t.value + d) & 0xFFFFu;
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

bool Engine::ParsePadPress(const std::string& text, PadPress& out) {
    static const char* const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                           "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    const size_t at = text.find('@');
    if (at == std::string::npos) return false;
    const std::string name = text.substr(0, at);
    const auto it = std::find(std::begin(kNames), std::end(kNames), name);
    if (it == std::end(kNames)) return false;
    char* end = nullptr;
    const char* p = text.c_str() + at + 1;
    PadPress r;
    r.mask = static_cast<uint16_t>(1u << (it - std::begin(kNames)));
    r.frame = std::strtoull(p, &end, 10);
    if (end == p) return false;
    if (*end == '+') {
        p = end + 1;
        r.frames = std::strtoull(p, &end, 10);
        if (end == p) return false;
    }
    if (*end != 0) return false;
    out = r;
    return true;
}

bool Engine::ParsePadScript(std::istream& in, std::vector<PadPress>& out, size_t& bad_line) {
    std::string line;
    for (bad_line = 1; std::getline(in, line); ++bad_line) {
        line = line.substr(0, line.find('#'));
        std::istringstream ls(line);
        std::string frame, button, hold, extra;
        if (!(ls >> frame)) continue;  // blank or comment
        PadPress p{};
        if (!(ls >> button) || (ls >> hold && ls >> extra) ||
            !ParsePadPress(button + "@" + frame + (hold.empty() ? "" : "+" + hold), p))
            return false;
        out.push_back(p);
    }
    return true;
}

uint16_t Engine::PadButtons() const {
    uint16_t held = ScriptedButtons() | m_pad_buttons;
    static const std::pair<VirtualAction, uint16_t> kMap[] = {
        {VirtualAction::MoveUp, 1u << 4}, {VirtualAction::MoveRight, 1u << 5}, {VirtualAction::MoveDown, 1u << 6},
        {VirtualAction::MoveLeft, 1u << 7}, {VirtualAction::CameraRotateLeft, 1u << 10},
        {VirtualAction::CameraRotateRight, 1u << 11}, {VirtualAction::Menu, 1u << 12},
        {VirtualAction::Cancel, 1u << 13}, {VirtualAction::Confirm, 1u << 14}, {VirtualAction::Map, 1u << 15}};
    for (const auto& m : kMap)
        if (m_input.IsActionPressed(m.first)) held |= m.second;
    return held;
}

// JOY_CTRL: bit 1 /JOYn select, 4 acknowledge (clears IRQ), 6 reset, 12 /ACK IRQ enable, 13 slot.
void Engine::WriteJoyCtrl(uint16_t v) {
    if (v & 0x40) {
        m_joy_rx_fifo.clear();
        m_joy_mode = m_joy_baud = 0;
        m_joy_ack_low = m_joy_irq = false;
        m_joy_ack_timer = 0;
    }
    if (v & 0x10) m_joy_irq = false;
    if (!(v & 0x2) || ((v ^ m_joy_ctrl) & 0x2000)) m_joy_step = 0;  // deselect / slot change ends the exchange
    m_joy_ctrl = v & ~0x50u;
}

void Engine::TickJoy(uint32_t cycles) {
    if (!m_joy_ack_timer) return;
    if (m_joy_ack_timer > cycles) { m_joy_ack_timer -= cycles; return; }
    m_joy_ack_timer = 0;
    m_joy_ack_low = true;
    if ((m_joy_ctrl & 0x1000) && !m_joy_irq) {
        m_joy_irq = true;
        RaiseIrq(7);
    }
}

uint16_t Engine::ScriptedButtons() const {
    uint16_t held = 0;
    for (const PadPress& p : m_pad_script)
        if (m_frame_counter >= p.frame && m_frame_counter < p.frame + p.frames) held |= p.mask;
    return held;
}

void Engine::GetCurrentFrame(std::vector<uint32_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) {
    m_gpu.OutputFrameToRGBA(out_rgba, out_w, out_h);
}

} // namespace personarm

