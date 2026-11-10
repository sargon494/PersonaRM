#include "cpu_context.h"
#include "core/logger.h"

namespace personarm {

CpuContext::CpuContext() {
    Reset();
}

void CpuContext::Reset() {
    m_regs.gpr.fill(0);
    m_regs.pc = 0x80010000; // Standard PS-X EXE entry point
    m_regs.hi = 0;
    m_regs.lo = 0;
    m_regs.cp0_status = 0;
    m_regs.cp0_cause = 0;
    m_regs.cp0_epc = 0;
    m_cycles = 0;

    m_next_pc = m_regs.pc + 4;
    m_branch_target.reset();
    m_in_delay_slot = false;
    m_load_delay_reg = 0;
    m_load_delay_val = 0;
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

void CpuContext::ScheduleBranch(uint32_t target_address) {
    m_branch_target = target_address;
}

void CpuContext::Step() {
    if (!m_read_memory) {
        m_regs.pc += 4;
        m_cycles++;
        return;
    }

    // Apply delayed load if pending from previous cycle
    if (m_load_delay_reg != 0) {
        SetGPR(m_load_delay_reg, m_load_delay_val);
        m_load_delay_reg = 0;
    }

    uint32_t current_pc = m_regs.pc;
    uint32_t opcode = m_read_memory(current_pc, 4);

    // Update PC pipeline: advance to next_pc or branch destination
    if (m_in_delay_slot && m_branch_target.has_value()) {
        m_regs.pc = *m_branch_target;
        m_next_pc = m_regs.pc + 4;
        m_branch_target.reset();
        m_in_delay_slot = false;
    } else {
        m_regs.pc = m_next_pc;
        m_next_pc += 4;
        if (m_branch_target.has_value()) {
            m_in_delay_slot = true;
        }
    }

    ExecuteInstruction(opcode);
    m_cycles++;
}

void CpuContext::ExecuteInstruction(uint32_t opcode) {
    uint32_t op     = (opcode >> 26) & 0x3F;
    uint32_t rs     = (opcode >> 21) & 0x1F;
    uint32_t rt     = (opcode >> 16) & 0x1F;
    uint32_t rd     = (opcode >> 11) & 0x1F;
    uint32_t shamt  = (opcode >> 6)  & 0x1F;
    uint32_t funct  = opcode & 0x3F;
    uint16_t imm    = opcode & 0xFFFF;
    int32_t  simm   = static_cast<int16_t>(imm);
    uint32_t target = opcode & 0x03FFFFFF;

    uint32_t rs_val = GetGPR(rs);
    uint32_t rt_val = GetGPR(rt);

    switch (op) {
        case 0x00: // SPECIAL (R-type instructions)
            switch (funct) {
                case 0x00: // SLL
                    SetGPR(rd, rt_val << shamt);
                    break;
                case 0x02: // SRL
                    SetGPR(rd, rt_val >> shamt);
                    break;
                case 0x03: // SRA
                    SetGPR(rd, static_cast<uint32_t>(static_cast<int32_t>(rt_val) >> shamt));
                    break;
                case 0x04: // SLLV
                    SetGPR(rd, rt_val << (rs_val & 0x1F));
                    break;
                case 0x06: // SRLV
                    SetGPR(rd, rt_val >> (rs_val & 0x1F));
                    break;
                case 0x07: // SRAV
                    SetGPR(rd, static_cast<uint32_t>(static_cast<int32_t>(rt_val) >> (rs_val & 0x1F)));
                    break;
                case 0x08: // JR (Jump Register)
                    ScheduleBranch(rs_val);
                    break;
                case 0x09: // JALR (Jump and Link Register)
                    SetGPR(rd != 0 ? rd : 31, m_regs.pc + 4);
                    ScheduleBranch(rs_val);
                    break;
                case 0x0C: // SYSCALL
                    m_regs.cp0_cause = (m_regs.cp0_cause & ~0x7C) | (0x08 << 2);
                    m_regs.cp0_epc = m_regs.pc - 4;
                    PRM_LOG_DEBUG("CPU", "SYSCALL encountered at PC: " + std::to_string(m_regs.cp0_epc));
                    break;
                case 0x0D: // BREAK
                    PRM_LOG_WARN("CPU", "BREAK breakpoint at PC: " + std::to_string(m_regs.pc - 4));
                    break;
                case 0x10: // MFHI
                    SetGPR(rd, m_regs.hi);
                    break;
                case 0x11: // MTHI
                    m_regs.hi = rs_val;
                    break;
                case 0x12: // MFLO
                    SetGPR(rd, m_regs.lo);
                    break;
                case 0x13: // MTLO
                    m_regs.lo = rs_val;
                    break;
                case 0x18: { // MULT (signed)
                    int64_t res = static_cast<int64_t>(static_cast<int32_t>(rs_val)) *
                                  static_cast<int64_t>(static_cast<int32_t>(rt_val));
                    m_regs.lo = static_cast<uint32_t>(res & 0xFFFFFFFF);
                    m_regs.hi = static_cast<uint32_t>((res >> 32) & 0xFFFFFFFF);
                    break;
                }
                case 0x19: { // MULTU (unsigned)
                    uint64_t res = static_cast<uint64_t>(rs_val) * static_cast<uint64_t>(rt_val);
                    m_regs.lo = static_cast<uint32_t>(res & 0xFFFFFFFF);
                    m_regs.hi = static_cast<uint32_t>((res >> 32) & 0xFFFFFFFF);
                    break;
                }
                case 0x1A: // DIV (signed)
                    if (rt_val != 0) {
                        m_regs.lo = static_cast<uint32_t>(static_cast<int32_t>(rs_val) / static_cast<int32_t>(rt_val));
                        m_regs.hi = static_cast<uint32_t>(static_cast<int32_t>(rs_val) % static_cast<int32_t>(rt_val));
                    }
                    break;
                case 0x1B: // DIVU (unsigned)
                    if (rt_val != 0) {
                        m_regs.lo = rs_val / rt_val;
                        m_regs.hi = rs_val % rt_val;
                    }
                    break;
                case 0x20: // ADD
                case 0x21: // ADDU
                    SetGPR(rd, rs_val + rt_val);
                    break;
                case 0x22: // SUB
                case 0x23: // SUBU
                    SetGPR(rd, rs_val - rt_val);
                    break;
                case 0x24: // AND
                    SetGPR(rd, rs_val & rt_val);
                    break;
                case 0x25: // OR
                    SetGPR(rd, rs_val | rt_val);
                    break;
                case 0x26: // XOR
                    SetGPR(rd, rs_val ^ rt_val);
                    break;
                case 0x27: // NOR
                    SetGPR(rd, ~(rs_val | rt_val));
                    break;
                case 0x2A: // SLT (Set on Less Than, signed)
                    SetGPR(rd, (static_cast<int32_t>(rs_val) < static_cast<int32_t>(rt_val)) ? 1 : 0);
                    break;
                case 0x2B: // SLTU (Set on Less Than, unsigned)
                    SetGPR(rd, (rs_val < rt_val) ? 1 : 0);
                    break;
                default:
                    PRM_LOG_WARN("CPU", "Unknown SPECIAL funct: 0x" + std::to_string(funct));
                    break;
            }
            break;

        case 0x01: { // REGIMM (Branch condition sub-opcodes)
            bool condition = false;
            int32_t sval = static_cast<int32_t>(rs_val);
            if (rt == 0x00) condition = (sval < 0);            // BLTZ
            else if (rt == 0x01) condition = (sval >= 0);     // BGEZ
            else if (rt == 0x10) {                            // BLTZAL
                SetGPR(31, m_regs.pc + 4);
                condition = (sval < 0);
            }
            else if (rt == 0x11) {                            // BGEZAL
                SetGPR(31, m_regs.pc + 4);
                condition = (sval >= 0);
            }

            if (condition) {
                ScheduleBranch(m_regs.pc + (simm << 2));
            }
            break;
        }

        case 0x02: // J (Jump)
            ScheduleBranch((m_regs.pc & 0xF0000000) | (target << 2));
            break;

        case 0x03: // JAL (Jump and Link)
            SetGPR(31, m_regs.pc + 4);
            ScheduleBranch((m_regs.pc & 0xF0000000) | (target << 2));
            break;

        case 0x04: // BEQ (Branch on Equal)
            if (rs_val == rt_val) {
                ScheduleBranch(m_regs.pc + (simm << 2));
            }
            break;

        case 0x05: // BNE (Branch on Not Equal)
            if (rs_val != rt_val) {
                ScheduleBranch(m_regs.pc + (simm << 2));
            }
            break;

        case 0x06: // BLEZ (Branch on Less Than or Equal to Zero)
            if (static_cast<int32_t>(rs_val) <= 0) {
                ScheduleBranch(m_regs.pc + (simm << 2));
            }
            break;

        case 0x07: // BGTZ (Branch on Greater Than Zero)
            if (static_cast<int32_t>(rs_val) > 0) {
                ScheduleBranch(m_regs.pc + (simm << 2));
            }
            break;

        case 0x08: // ADDI
        case 0x09: // ADDIU
            SetGPR(rt, rs_val + simm);
            break;

        case 0x0A: // SLTI (signed)
            SetGPR(rt, (static_cast<int32_t>(rs_val) < simm) ? 1 : 0);
            break;

        case 0x0B: // SLTIU (unsigned)
            SetGPR(rt, (rs_val < static_cast<uint32_t>(simm)) ? 1 : 0);
            break;

        case 0x0C: // ANDI
            SetGPR(rt, rs_val & imm);
            break;

        case 0x0D: // ORI
            SetGPR(rt, rs_val | imm);
            break;

        case 0x0E: // XORI
            SetGPR(rt, rs_val ^ imm);
            break;

        case 0x0F: // LUI
            SetGPR(rt, static_cast<uint32_t>(imm) << 16);
            break;

        case 0x10: // COP0 (Coprocessor 0 - System Control)
            if (rs == 0x00) { // MFC0 rt, rd
                if (rd == 12) SetGPR(rt, m_regs.cp0_status);
                else if (rd == 13) SetGPR(rt, m_regs.cp0_cause);
                else if (rd == 14) SetGPR(rt, m_regs.cp0_epc);
            } else if (rs == 0x04) { // MTC0 rt, rd
                if (rd == 12) m_regs.cp0_status = rt_val;
                else if (rd == 13) m_regs.cp0_cause = rt_val;
                else if (rd == 14) m_regs.cp0_epc = rt_val;
            }
            break;

        // Memory Loads (with Load Delay Slot simulation)
        case 0x20: { // LB (Load Byte signed)
            if (m_read_memory) {
                uint32_t addr = rs_val + simm;
                int8_t val = static_cast<int8_t>(m_read_memory(addr, 1));
                m_load_delay_reg = rt;
                m_load_delay_val = static_cast<int32_t>(val);
            }
            break;
        }
        case 0x21: { // LH (Load Halfword signed)
            if (m_read_memory) {
                uint32_t addr = rs_val + simm;
                int16_t val = static_cast<int16_t>(m_read_memory(addr, 2));
                m_load_delay_reg = rt;
                m_load_delay_val = static_cast<int32_t>(val);
            }
            break;
        }
        case 0x23: { // LW (Load Word)
            if (m_read_memory) {
                uint32_t addr = rs_val + simm;
                uint32_t val = m_read_memory(addr, 4);
                m_load_delay_reg = rt;
                m_load_delay_val = val;
            }
            break;
        }
        case 0x24: { // LBU (Load Byte unsigned)
            if (m_read_memory) {
                uint32_t addr = rs_val + simm;
                uint8_t val = static_cast<uint8_t>(m_read_memory(addr, 1));
                m_load_delay_reg = rt;
                m_load_delay_val = val;
            }
            break;
        }
        case 0x25: { // LHU (Load Halfword unsigned)
            if (m_read_memory) {
                uint32_t addr = rs_val + simm;
                uint16_t val = static_cast<uint16_t>(m_read_memory(addr, 2));
                m_load_delay_reg = rt;
                m_load_delay_val = val;
            }
            break;
        }

        // Memory Stores
        case 0x28: // SB (Store Byte)
            if (m_write_memory) {
                m_write_memory(rs_val + simm, rt_val & 0xFF, 1);
            }
            break;

        case 0x29: // SH (Store Halfword)
            if (m_write_memory) {
                m_write_memory(rs_val + simm, rt_val & 0xFFFF, 2);
            }
            break;

        case 0x2B: // SW (Store Word)
            if (m_write_memory) {
                m_write_memory(rs_val + simm, rt_val, 4);
            }
            break;

        default:
            PRM_LOG_WARN("CPU", "Unimplemented opcode: 0x" + std::to_string(op));
            break;
    }
}

} // namespace personarm
