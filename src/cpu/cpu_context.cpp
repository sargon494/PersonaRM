#include "cpu_context.h"
#include <iostream>

namespace personarm {

CpuContext::CpuContext() {
    Reset();
}

void CpuContext::Reset() {
    m_regs.gpr.fill(0);
    m_regs.pc = 0x80010000; // Standard entry point
    m_regs.hi = 0;
    m_regs.lo = 0;
    m_regs.cp0_status = 0;
    m_regs.cp0_cause = 0;
    m_regs.cp0_epc = 0;
    m_cycles = 0;
}

uint32_t CpuContext::GetGPR(uint32_t index) const {
    if (index >= 32) return 0;
    return m_regs.gpr[index];
}

void CpuContext::SetGPR(uint32_t index, uint32_t value) {
    if (index == 0 || index >= 32) return; // r0 is hardwired to 0
    m_regs.gpr[index] = value;
}

void CpuContext::SetMemoryHooks(ReadMemoryFunc read_fn, WriteMemoryFunc write_fn) {
    m_read_memory = std::move(read_fn);
    m_write_memory = std::move(write_fn);
}

void CpuContext::Step() {
    if (!m_read_memory) {
        m_regs.pc += 4;
        m_cycles++;
        return;
    }

    uint32_t opcode = m_read_memory(m_regs.pc, 4);
    ExecuteInstruction(opcode);
    m_regs.pc += 4;
    m_cycles++;
}

void CpuContext::ExecuteInstruction(uint32_t opcode) {
    uint32_t op = (opcode >> 26) & 0x3F;
    uint32_t rs = (opcode >> 21) & 0x1F;
    uint32_t rt = (opcode >> 16) & 0x1F;
    uint32_t rd = (opcode >> 11) & 0x1F;
    uint32_t funct = opcode & 0x3F;
    uint16_t imm = opcode & 0xFFFF;
    int32_t simm = static_cast<int16_t>(imm);

    switch (op) {
        case 0x00: // SPECIAL
            switch (funct) {
                case 0x20: // ADD
                case 0x21: // ADDU
                    SetGPR(rd, GetGPR(rs) + GetGPR(rt));
                    break;
                case 0x22: // SUB
                case 0x23: // SUBU
                    SetGPR(rd, GetGPR(rs) - GetGPR(rt));
                    break;
                case 0x24: // AND
                    SetGPR(rd, GetGPR(rs) & GetGPR(rt));
                    break;
                case 0x25: // OR
                    SetGPR(rd, GetGPR(rs) | GetGPR(rt));
                    break;
                case 0x26: // XOR
                    SetGPR(rd, GetGPR(rs) ^ GetGPR(rt));
                    break;
                case 0x27: // NOR
                    SetGPR(rd, ~(GetGPR(rs) | GetGPR(rt)));
                    break;
                default:
                    break;
            }
            break;

        case 0x08: // ADDI
        case 0x09: // ADDIU
            SetGPR(rt, GetGPR(rs) + simm);
            break;

        case 0x0C: // ANDI
            SetGPR(rt, GetGPR(rs) & imm);
            break;

        case 0x0D: // ORI
            SetGPR(rt, GetGPR(rs) | imm);
            break;

        case 0x0E: // XORI
            SetGPR(rt, GetGPR(rs) ^ imm);
            break;

        case 0x0F: // LUI
            SetGPR(rt, static_cast<uint32_t>(imm) << 16);
            break;

        default:
            break;
    }
}

} // namespace personarm
