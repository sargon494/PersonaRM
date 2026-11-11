#pragma once

// MIPS-I (PlayStation R3000A) instruction classifier used by the offline recompilation
// analysis (WP-14). It classifies one 32-bit word; it does not execute anything.

#include <cstdint>

namespace personarm::recomp {

enum class InsnKind : uint8_t {
    Invalid,        // reserved encoding: data or a decoding error
    Alu,            // register or immediate arithmetic/logic, shifts, mul/div, lui
    Load,           // lb..lwr
    Store,          // sb..swr
    Branch,         // conditional PC-relative branch (beq, bne, blez, bgtz, bltz, bgez)
    Jump,           // j (direct)
    Call,           // jal, bltzal, bgezal (direct)
    IndirectJump,   // jr <reg> other than $ra
    IndirectCall,   // jalr
    Return,         // jr $ra
    Syscall,        // syscall, break (no delay slot)
    Cop             // COP0/COP2 transfers and unusable coprocessors
};

struct Insn {
    uint32_t word = 0;
    uint32_t addr = 0;
    InsnKind kind = InsnKind::Invalid;
    bool has_delay_slot = false;
    bool has_target = false;    // direct targets only
    uint32_t target = 0;
    uint8_t rs = 0, rt = 0, rd = 0;
};

Insn Decode(uint32_t word, uint32_t addr);

} // namespace personarm::recomp
