#include "loader/ps1_bios_hle.h"
#include "loader/disc_reader.h"
#include "core/logger.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace personarm {

Ps1BiosHle& Ps1BiosHle::Instance() {
    static Ps1BiosHle instance;
    return instance;
}

void Ps1BiosHle::Initialize() {
    Reset();
    m_initialized = true;
    PRM_LOG_INFO("BIOS", "PlayStation 1 HLE BIOS Services initialized (Vectors A, B, C)");
}

void Ps1BiosHle::Reset() {
    m_intercept_count = 0;
    m_open_files.clear();
    m_next_fd = 1;
}

bool Ps1BiosHle::HandleBiosHook(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    return HandleBiosHook(cpu, nullptr, 0);
}

bool Ps1BiosHle::HandleBiosHook(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    uint32_t pc = cpu.GetPC();
    uint32_t masked_pc = pc & 0x1FFFFFFF;

    if (masked_pc == 0xA0) {
        // Vector A: function code passed in register $t1 ($9)
        uint32_t func = cpu.GetGPR(9);
        return ExecuteCall(BiosVector::VectorA, func, cpu, ram, ram_size);
    } else if (masked_pc == 0xB0) {
        // Vector B: function code passed in register $t1 ($9)
        uint32_t func = cpu.GetGPR(9);
        return ExecuteCall(BiosVector::VectorB, func, cpu, ram, ram_size);
    } else if (masked_pc == 0xC0) {
        // Vector C: function code passed in register $t1 ($9)
        uint32_t func = cpu.GetGPR(9);
        return ExecuteCall(BiosVector::VectorC, func, cpu, ram, ram_size);
    } else if (masked_pc == 0x80) {
        // Exception vector with no BIOS in RAM: return past the faulting opcode and pop SR (rfe).
        // shortcut: SYSCALL 1/2 (Enter/ExitCriticalSection) do not touch SR.IEc and IRQs are not
        // dispatched; upgrade when interrupts are emulated (GAP-CPU-06).
        CpuRegisters regs = cpu.GetRegisters();
        // Only SYSCALL (8) and BREAK (9) are expected here; a fault (Ov, AdEL, AdES...) means a
        // real bug, so make it visible instead of silently resuming.
        const uint32_t exc = (regs.cp0_cause >> 2) & 0x1Fu;
        if (exc != 8 && exc != 9) {
            std::ostringstream m;
            m << "unhandled CPU exception code " << exc << " at EPC 0x" << std::hex << regs.cp0_epc;
            PRM_LOG_ERROR("HLE", m.str());
        }
        regs.pc = regs.cp0_epc + 4;
        regs.cp0_status = (regs.cp0_status & ~0x0Fu) | ((regs.cp0_status >> 2) & 0x0Fu);
        cpu.SetRegisters(regs);
        return true;
    }

    return false;
}

bool Ps1BiosHle::ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    return ExecuteCall(vec, func_code, cpu, nullptr, 0);
}

bool Ps1BiosHle::ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    m_intercept_count++;

    if (vec == BiosVector::VectorA) {
        switch (func_code) {
            case 0x00: // FileOpen (a0=filename, a1=access_mode) -> return fd in $v0
                HandleFileOpen(cpu, ram, ram_size);
                break;
            case 0x02: // FileRead (a0=fd, a1=dst_buf, a2=count) -> return bytes read in $v0
                HandleFileRead(cpu, ram, ram_size);
                break;
            case 0x04: // FileClose (a0=fd) -> return 0
                HandleFileClose(cpu);
                break;
            case 0x2A: // memcpy (a0=dst, a1=src, a2=count)
                HandleMemCopy(cpu, ram, ram_size);
                break;
            case 0x2B: // memset (a0=dst, a1=val, a2=count)
                HandleMemSet(cpu, ram, ram_size);
                break;
            default:
                PRM_LOG_DEBUG("BIOS", "Unhandled Vector A call: 0x" + std::to_string(func_code));
                cpu.SetGPR(2, 0); // $v0 = 0
                break;
        }
    } else if (vec == BiosVector::VectorB) {
        switch (func_code) {
            case 0x3F: // puts (a0=str_ptr)
                HandleStdPuts(cpu, ram, ram_size);
                break;
            case 0x12: // InitHeap (a0=heap_addr, a1=size)
                HandleHeapInit(cpu);
                break;
            default:
                PRM_LOG_DEBUG("BIOS", "Unhandled Vector B call: 0x" + std::to_string(func_code));
                cpu.SetGPR(2, 0);
                break;
        }
    } else if (vec == BiosVector::VectorC) {
        switch (func_code) {
            case 0x12: // InitPAD (a0=buf1, a1=len1, a2=buf2, a3=len2)
                HandlePadInit(cpu);
                break;
            default:
                PRM_LOG_DEBUG("BIOS", "Unhandled Vector C call: 0x" + std::to_string(func_code));
                cpu.SetGPR(2, 0);
                break;
        }
    }

    // Emulate standard MIPS function return: JR $ra ($31)
    uint32_t return_address = cpu.GetGPR(31);
    if (return_address != 0) {
        cpu.SetPC(return_address);
    }
    return true;
}

void Ps1BiosHle::HandleFileOpen(CpuContext& cpu, const uint8_t* ram, size_t ram_size) {
    uint32_t name_ptr = cpu.GetGPR(4); // $a0
    std::string filename;

    if (ram && name_ptr != 0) {
        uint32_t phys = name_ptr & 0x1FFFFFFF;
        while (phys < ram_size && ram[phys] != 0 && filename.size() < 128) {
            filename += static_cast<char>(ram[phys++]);
        }
    }

    int fd = m_next_fd++;
    PRM_LOG_INFO("BIOS", "FileOpen intercepted: '" + filename + "' -> returning fd=" + std::to_string(fd));

    if (m_disc_reader && !filename.empty()) {
        std::vector<uint8_t> file_bytes;
        // Strip cdrom: prefix if present
        std::string lookup_name = filename;
        if (lookup_name.rfind("cdrom:", 0) == 0) {
            lookup_name = lookup_name.substr(6);
        }
        if (lookup_name.rfind("cdrom\\", 0) == 0) {
            lookup_name = lookup_name.substr(6);
        }
        // Remove leading slashes
        while (!lookup_name.empty() && (lookup_name.front() == '/' || lookup_name.front() == '\\')) {
            lookup_name.erase(0, 1);
        }

        if (m_disc_reader->ReadFile(lookup_name, file_bytes)) {
            m_open_files[fd] = std::move(file_bytes);
        }
    }

    cpu.SetGPR(2, static_cast<uint32_t>(fd));
}

void Ps1BiosHle::HandleFileRead(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    int fd = static_cast<int>(cpu.GetGPR(4));     // $a0 = fd
    uint32_t dst_ptr = cpu.GetGPR(5);           // $a1 = dst_buf
    uint32_t count = cpu.GetGPR(6);             // $a2 = count

    uint32_t bytes_read = 0;
    auto it = m_open_files.find(fd);
    if (it != m_open_files.end() && ram && count > 0) {
        uint32_t phys = dst_ptr & 0x1FFFFFFF;
        size_t available = it->second.size();
        size_t to_copy = std::min(static_cast<size_t>(count), available);
        if (phys + to_copy <= ram_size) {
            std::memcpy(&ram[phys], it->second.data(), to_copy);
            bytes_read = static_cast<uint32_t>(to_copy);
        }
    } else {
        // Fallback: report requested count read
        bytes_read = count;
    }

    cpu.SetGPR(2, bytes_read); // $v0 = bytes read
    PRM_LOG_DEBUG("BIOS", "FileRead intercepted: fd=" + std::to_string(fd) +
                          ", bytes=" + std::to_string(bytes_read));
}

void Ps1BiosHle::HandleFileClose(CpuContext& cpu) {
    int fd = static_cast<int>(cpu.GetGPR(4));
    m_open_files.erase(fd);
    cpu.SetGPR(2, 0); // 0 = success
    PRM_LOG_DEBUG("BIOS", "FileClose intercepted for fd=" + std::to_string(fd));
}

void Ps1BiosHle::HandleMemCopy(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    uint32_t dst = cpu.GetGPR(4);   // $a0
    uint32_t src = cpu.GetGPR(5);   // $a1
    uint32_t count = cpu.GetGPR(6); // $a2

    if (ram && count > 0) {
        uint32_t dst_phys = dst & 0x1FFFFFFF;
        uint32_t src_phys = src & 0x1FFFFFFF;
        if (dst_phys < ram_size && src_phys < ram_size) {
            size_t copy_len = std::min(static_cast<size_t>(count),
                                       std::min(ram_size - dst_phys, ram_size - src_phys));
            std::memmove(&ram[dst_phys], &ram[src_phys], copy_len);
        }
    }

    cpu.SetGPR(2, dst); // memcpy returns dst pointer in $v0
    PRM_LOG_DEBUG("BIOS", "memcpy intercepted: dst=0x" + std::to_string(dst) +
                          " src=0x" + std::to_string(src) + " count=" + std::to_string(count));
}

void Ps1BiosHle::HandleMemSet(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    uint32_t dst = cpu.GetGPR(4);   // $a0
    uint32_t val = cpu.GetGPR(5);   // $a1
    uint32_t count = cpu.GetGPR(6); // $a2

    if (ram && count > 0) {
        uint32_t dst_phys = dst & 0x1FFFFFFF;
        if (dst_phys < ram_size) {
            size_t set_len = std::min(static_cast<size_t>(count), ram_size - dst_phys);
            std::memset(&ram[dst_phys], static_cast<int>(val & 0xFF), set_len);
        }
    }

    cpu.SetGPR(2, dst); // memset returns dst pointer in $v0
    PRM_LOG_DEBUG("BIOS", "memset intercepted: dst=0x" + std::to_string(dst) +
                          " val=" + std::to_string(val & 0xFF) + " count=" + std::to_string(count));
}

void Ps1BiosHle::HandleHeapInit(CpuContext& cpu) {
    uint32_t heap_base = cpu.GetGPR(4); // $a0
    uint32_t heap_size = cpu.GetGPR(5); // $a1
    cpu.SetGPR(2, 1); // 1 = success
    PRM_LOG_INFO("BIOS", "InitHeap intercepted: base=0x" + std::to_string(heap_base) +
                         " size=" + std::to_string(heap_size) + " bytes");
}

void Ps1BiosHle::HandleStdPuts(CpuContext& cpu, const uint8_t* ram, size_t ram_size) {
    uint32_t str_ptr = cpu.GetGPR(4);
    std::string msg;

    if (ram && str_ptr != 0) {
        uint32_t phys = str_ptr & 0x1FFFFFFF;
        while (phys < ram_size && ram[phys] != 0 && msg.size() < 1024) {
            msg += static_cast<char>(ram[phys++]);
        }
    }

    cpu.SetGPR(2, 0);
    PRM_LOG_INFO("BIOS_TTY", msg.empty() ? "(empty)" : msg);
}

void Ps1BiosHle::HandlePadInit(CpuContext& cpu) {
    cpu.SetGPR(2, 1); // 1 = initialized successfully
    PRM_LOG_INFO("BIOS", "InitPAD intercepted -> controllers registered via InputManager");
}

} // namespace personarm
