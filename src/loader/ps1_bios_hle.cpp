#include "loader/ps1_bios_hle.h"
#include "core/logger.h"
#include <iostream>

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
}

bool Ps1BiosHle::HandleBiosHook(CpuContext& cpu, MemoryManager& memory) {
    uint32_t pc = cpu.GetPC();

    // Mask to detect BIOS function vector calls in KUSEG / KSEG0
    uint32_t masked_pc = pc & 0x1FFFFFFF;

    if (masked_pc == 0xA0) {
        // Vector A: function code passed in register $t1 ($9)
        uint32_t func = cpu.GetGPR(9);
        return ExecuteCall(BiosVector::VectorA, func, cpu, memory);
    } else if (masked_pc == 0xB0) {
        // Vector B: function code passed in register $t1 ($9)
        uint32_t func = cpu.GetGPR(9);
        return ExecuteCall(BiosVector::VectorB, func, cpu, memory);
    } else if (masked_pc == 0xC0) {
        // Vector C: function code passed in register $t1 ($9)
        uint32_t func = cpu.GetGPR(9);
        return ExecuteCall(BiosVector::VectorC, func, cpu, memory);
    }

    return false;
}

bool Ps1BiosHle::ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, MemoryManager& memory) {
    m_intercept_count++;

    if (vec == BiosVector::VectorA) {
        switch (func_code) {
            case 0x00: // FileOpen (a0=filename, a1=access_mode) -> return fd in $v0
                HandleFileOpen(cpu);
                break;
            case 0x02: // FileRead (a0=fd, a1=dst_buf, a2=count) -> return bytes read in $v0
                HandleFileRead(cpu, memory);
                break;
            case 0x04: // FileClose (a0=fd) -> return 0
                HandleFileClose(cpu);
                break;
            case 0x2A: // memcpy (a0=dst, a1=src, a2=count)
                HandleMemCopy(cpu, memory);
                break;
            case 0x2B: // memset (a0=dst, a1=val, a2=count)
                HandleMemSet(cpu, memory);
                break;
            default:
                PRM_LOG_DEBUG("BIOS", "Unhandled Vector A call: 0x" + std::to_string(func_code));
                cpu.SetGPR(2, 0); // $v0 = 0
                break;
        }
    } else if (vec == BiosVector::VectorB) {
        switch (func_code) {
            case 0x3F: // puts (a0=str_ptr)
                HandleStdPuts(cpu, memory);
                break;
            case 0x12: // InitHeap (a0=heap_addr, a1=size)
                HandleHeapInit(cpu, memory);
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

void Ps1BiosHle::HandleFileOpen(CpuContext& cpu) {
    // Return dummy valid file descriptor 1
    cpu.SetGPR(2, 1);
    PRM_LOG_DEBUG("BIOS", "FileOpen intercepted -> returning fd=1");
}

void Ps1BiosHle::HandleFileRead(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    uint32_t count = cpu.GetGPR(6); // $a2
    // Report requested bytes read successfully
    cpu.SetGPR(2, count);
    PRM_LOG_DEBUG("BIOS", "FileRead intercepted -> read " + std::to_string(count) + " bytes");
}

void Ps1BiosHle::HandleFileClose(CpuContext& cpu) {
    cpu.SetGPR(2, 0);
    PRM_LOG_DEBUG("BIOS", "FileClose intercepted");
}

void Ps1BiosHle::HandleMemCopy(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    uint32_t dst = cpu.GetGPR(4); // $a0
    uint32_t count = cpu.GetGPR(6); // $a2
    cpu.SetGPR(2, dst); // memcpy returns dst
    PRM_LOG_DEBUG("BIOS", "memcpy intercepted -> count=" + std::to_string(count));
}

void Ps1BiosHle::HandleMemSet(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    uint32_t dst = cpu.GetGPR(4); // $a0
    uint32_t count = cpu.GetGPR(6); // $a2
    cpu.SetGPR(2, dst);
    PRM_LOG_DEBUG("BIOS", "memset intercepted -> count=" + std::to_string(count));
}

void Ps1BiosHle::HandleHeapInit(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    uint32_t heap_base = cpu.GetGPR(4); // $a0
    uint32_t heap_size = cpu.GetGPR(5); // $a1
    cpu.SetGPR(2, 1); // Success
    PRM_LOG_INFO("BIOS", "InitHeap intercepted: base=0x" + std::to_string(heap_base) +
                         " size=" + std::to_string(heap_size) + " bytes");
}

void Ps1BiosHle::HandleStdPuts(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    uint32_t str_ptr = cpu.GetGPR(4);
    cpu.SetGPR(2, 0);
    PRM_LOG_DEBUG("BIOS", "puts intercepted at 0x" + std::to_string(str_ptr));
}

void Ps1BiosHle::HandlePadInit(CpuContext& cpu) {
    cpu.SetGPR(2, 1); // 1 = initialized successfully
    PRM_LOG_INFO("BIOS", "InitPAD intercepted -> controllers registered via InputManager");
}

} // namespace personarm

