#pragma once

// MIPS-I (PlayStation R3000A) instruction classifier used by the offline recompilation
// analysis (WP-14). It classifies one 32-bit word; it does not execute anything.

#include <cstdint>
#include <string>

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

/// Raw instruction field accessors: the one definition of the MIPS-I encoding, used by the
/// interpreter (cpu_context.cpp) and by Decode(). Individual inline accessors (not a struct) so
/// the interpreter hot path only computes the fields it uses.
inline uint32_t OpOf(uint32_t w)     { return w >> 26; }
inline uint32_t RsOf(uint32_t w)     { return (w >> 21) & 0x1F; }
inline uint32_t RtOf(uint32_t w)     { return (w >> 16) & 0x1F; }
inline uint32_t RdOf(uint32_t w)     { return (w >> 11) & 0x1F; }
inline uint32_t ShamtOf(uint32_t w)  { return (w >> 6) & 0x1F; }
inline uint32_t FunctOf(uint32_t w)  { return w & 0x3F; }
inline uint32_t ImmOf(uint32_t w)    { return w & 0xFFFF; }
inline int32_t  SImmOf(uint32_t w)   { return static_cast<int16_t>(w & 0xFFFF); }
inline uint32_t TargetOf(uint32_t w) { return w & 0x03FFFFFF; }

/// True when the interpreter arms a branch delay slot for this word: every op 1..7 plus JR/JALR.
/// Matches Decode().has_delay_slot except reserved REGIMM rt values (Invalid there), which the
/// interpreter has always treated as branches; kept as-is so behaviour does not change.
inline bool OwnsDelaySlot(uint32_t w) {
    const uint32_t op = OpOf(w), fn = FunctOf(w);
    return (op >= 0x01 && op <= 0x07) || (op == 0x00 && (fn == 0x08 || fn == 0x09));
}

Insn Decode(uint32_t word, uint32_t addr);

/// Human-readable form of one word for local diagnostics, e.g. "lw $10,0($9)".
/// Unknown encodings render as ".word 0x........".
std::string DescribeInsn(uint32_t word);

} // namespace personarm::recomp
