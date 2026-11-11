#include "mips_decode.h"

#include <cstdio>

namespace personarm::recomp {

namespace {

bool ValidSpecialFunct(uint32_t f) {
    switch (f) {
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:   // shifts
        case 0x10: case 0x11: case 0x12: case 0x13:                          // hi/lo moves
        case 0x18: case 0x19: case 0x1A: case 0x1B:                          // mul/div
        case 0x20: case 0x21: case 0x22: case 0x23:                          // add/sub
        case 0x24: case 0x25: case 0x26: case 0x27:                          // logic
        case 0x2A: case 0x2B:                                                // slt
            return true;
        default:
            return false;
    }
}

} // namespace

Insn Decode(uint32_t w, uint32_t addr) {
    Insn in;
    in.word = w;
    in.addr = addr;
    const uint32_t op = w >> 26;
    in.rs = static_cast<uint8_t>((w >> 21) & 31);
    in.rt = static_cast<uint8_t>((w >> 16) & 31);
    in.rd = static_cast<uint8_t>((w >> 11) & 31);
    const uint32_t funct = w & 63;
    const int32_t simm = static_cast<int16_t>(w & 0xFFFF);
    const uint32_t branch_target = addr + 4 + (static_cast<uint32_t>(simm) << 2);
    const uint32_t jump_target = ((addr + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);

    switch (op) {
        case 0x00:  // SPECIAL
            switch (funct) {
                case 0x08: in.kind = (in.rs == 31) ? InsnKind::Return : InsnKind::IndirectJump; in.has_delay_slot = true; break;
                case 0x09: in.kind = InsnKind::IndirectCall; in.has_delay_slot = true; break;
                case 0x0C: case 0x0D: in.kind = InsnKind::Syscall; break;
                default:
                    if (ValidSpecialFunct(funct)) in.kind = InsnKind::Alu;
                    break;
            }
            break;
        case 0x01:  // REGIMM
            switch (in.rt) {
                case 0x00: case 0x01:
                    in.kind = InsnKind::Branch; in.has_delay_slot = true; in.has_target = true; in.target = branch_target; break;
                case 0x10: case 0x11:
                    in.kind = InsnKind::Call; in.has_delay_slot = true; in.has_target = true; in.target = branch_target; break;
                default: break;
            }
            break;
        case 0x02:
            in.kind = InsnKind::Jump; in.has_delay_slot = true; in.has_target = true; in.target = jump_target; break;
        case 0x03:
            in.kind = InsnKind::Call; in.has_delay_slot = true; in.has_target = true; in.target = jump_target; break;
        case 0x04: case 0x05: case 0x06: case 0x07:
            in.kind = InsnKind::Branch; in.has_delay_slot = true; in.has_target = true; in.target = branch_target; break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            in.kind = InsnKind::Alu; break;
        case 0x10: case 0x12:                                  // COP0, COP2
            in.kind = InsnKind::Cop; break;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
            in.kind = InsnKind::Load; break;
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
            in.kind = InsnKind::Store; break;
        case 0x30: case 0x31: case 0x32: case 0x33:            // LWCn (n = 0..3)
        case 0x38: case 0x39: case 0x3A: case 0x3B:            // SWCn
            in.kind = InsnKind::Cop; break;
        default:
            break;                                              // reserved opcode
    }
    return in;
}


std::string DescribeInsn(uint32_t w) {
    static const char* const kSpecial[64] = {
        "sll", nullptr, "srl", "sra", "sllv", nullptr, "srlv", "srav",
        "jr", "jalr", nullptr, nullptr, "syscall", "break", nullptr, nullptr,
        "mfhi", "mthi", "mflo", "mtlo", nullptr, nullptr, nullptr, nullptr,
        "mult", "multu", "div", "divu", nullptr, nullptr, nullptr, nullptr,
        "add", "addu", "sub", "subu", "and", "or", "xor", "nor",
        nullptr, nullptr, "slt", "sltu", nullptr, nullptr, nullptr, nullptr,
    };
    static const char* const kImm[64] = {
        nullptr, nullptr, "j", "jal", "beq", "bne", "blez", "bgtz",
        "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
        "cop0", nullptr, "cop2", nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", nullptr,
        "sb", "sh", "swl", "sw", nullptr, nullptr, "swr", nullptr,
        "lwc0", "lwc1", "lwc2", "lwc3", nullptr, nullptr, nullptr, nullptr,
        "swc0", "swc1", "swc2", "swc3", nullptr, nullptr, nullptr, nullptr,
    };
    const uint32_t op = w >> 26;
    const uint32_t rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    const int16_t imm = static_cast<int16_t>(w & 0xFFFF);
    char buf[96];
    if (op == 0) {
        const char* name = kSpecial[w & 63];
        if (!name) { std::snprintf(buf, sizeof buf, ".word 0x%08X", w); return buf; }
        switch (w & 63) {
            case 0x00: case 0x02: case 0x03:
                std::snprintf(buf, sizeof buf, "%s $%u,$%u,%u", name, rd, rt, sa); break;
            case 0x08: std::snprintf(buf, sizeof buf, "jr $%u", rs); break;
            case 0x09: std::snprintf(buf, sizeof buf, "jalr $%u,$%u", rd, rs); break;
            case 0x0C: case 0x0D: std::snprintf(buf, sizeof buf, "%s", name); break;
            case 0x10: case 0x12: std::snprintf(buf, sizeof buf, "%s $%u", name, rd); break;
            case 0x11: case 0x13: std::snprintf(buf, sizeof buf, "%s $%u", name, rs); break;
            default: std::snprintf(buf, sizeof buf, "%s $%u,$%u,$%u", name, rd, rs, rt); break;
        }
        return buf;
    }
    if (op == 1) {
        std::snprintf(buf, sizeof buf, "%s $%u,%d", rt == 0 ? "bltz" : rt == 1 ? "bgez" : rt == 16 ? "bltzal" : rt == 17 ? "bgezal" : "regimm", rs, imm);
        return buf;
    }
    const char* name = kImm[op];
    if (!name) { std::snprintf(buf, sizeof buf, ".word 0x%08X", w); return buf; }
    if (op == 2 || op == 3) { std::snprintf(buf, sizeof buf, "%s 0x%07X", name, (w & 0x03FFFFFFu) << 2); return buf; }
    if (op >= 4 && op <= 7) { std::snprintf(buf, sizeof buf, "%s $%u,$%u,%d", name, rs, rt, imm); return buf; }
    if (op == 0x0F) { std::snprintf(buf, sizeof buf, "lui $%u,0x%04X", rt, w & 0xFFFF); return buf; }
    if (op >= 0x20 && op <= 0x2E) { std::snprintf(buf, sizeof buf, "%s $%u,%d($%u)", name, rt, imm, rs); return buf; }
    if (op >= 0x08 && op <= 0x0E) { std::snprintf(buf, sizeof buf, "%s $%u,$%u,%d", name, rt, rs, imm); return buf; }
    std::snprintf(buf, sizeof buf, "%s $%u,%d($%u)", name, rt, imm, rs);
    return buf;
}

} // namespace personarm::recomp
