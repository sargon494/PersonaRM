#include "mips_decode.h"

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

} // namespace personarm::recomp
