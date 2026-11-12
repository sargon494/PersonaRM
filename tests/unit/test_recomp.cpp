// Tests for the recompilation analysis (WP-14): MIPS classification and control-flow reachability.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "recomp/cfg.h"
#include "recomp/mips_decode.h"

using namespace personarm::recomp;

static int g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static constexpr uint32_t kBase = 0x80010000u;
static constexpr uint32_t kNop = 0;
static constexpr uint32_t kJrRa = 0x03E00008u;
static constexpr uint32_t kJrT0 = 0x01000008u;
static constexpr uint32_t kInvalid = 0xFC000000u;    // opcode 0x3F: reserved on the R3000A
static constexpr uint32_t kLw = 0x8C000000u;
static constexpr uint32_t kAddiuV0One = 0x24020001u;

static uint32_t Jal(uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu); }
static uint32_t BeqZero(int32_t words_after_slot) { return 0x10000000u | (static_cast<uint32_t>(words_after_slot) & 0xFFFFu); }

static std::vector<uint8_t> Bytes(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out;
    for (uint32_t w : words) for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(w >> (8 * i)));
    return out;
}

static void TestDecode() {
    CHECK(Decode(kJrRa, 0).kind == InsnKind::Return);
    CHECK(Decode(kJrT0, 0).kind == InsnKind::IndirectJump && Decode(kJrT0, 0).has_delay_slot);
    CHECK(Decode(0x0000000Cu, 0).kind == InsnKind::Syscall && !Decode(0x0000000Cu, 0).has_delay_slot);
    CHECK(Decode(0x0000000Du, 0).kind == InsnKind::Syscall);
    CHECK(Decode(0x01000009u, 0).kind == InsnKind::IndirectCall);                     // jalr t0
    CHECK(Decode(kInvalid, 0).kind == InsnKind::Invalid);
    CHECK(Decode(0x0000003Fu, 0).kind == InsnKind::Invalid);                         // funct 0x3F
    CHECK(Decode(kNop, 0).kind == InsnKind::Alu);
    CHECK(Decode(kAddiuV0One, 0).kind == InsnKind::Alu);
    CHECK(Decode(0x3C041F80u, 0).kind == InsnKind::Alu);                             // lui
    CHECK(Decode(kLw, 0).kind == InsnKind::Load);
    CHECK(Decode(0xAC820000u, 0).kind == InsnKind::Store);                           // sw
    CHECK(Decode(0x40086000u, 0).kind == InsnKind::Cop);                             // mfc0

    Insn j = Decode(Jal(0x80012340u), 0x80010000u);
    CHECK(j.kind == InsnKind::Call && j.has_target && j.target == 0x80012340u);
    Insn b = Decode(BeqZero(3), 0x80010010u);                                        // target = pc+4+3*4
    CHECK(b.kind == InsnKind::Branch && b.target == 0x80010020u);
    Insn back = Decode(BeqZero(-2), 0x80010010u);                                    // backwards
    CHECK(back.target == 0x8001000Cu);
    Insn bltzal = Decode(0x04110004u, 0x80010000u);                                  // bgezal zero,+4
    CHECK(bltzal.kind == InsnKind::Call && bltzal.target == 0x80010014u);
    CHECK(Decode(0x04000000u, 0).kind == InsnKind::Branch);                          // bltz zero
    CHECK(Decode(0x04020000u, 0).kind == InsnKind::Invalid);                         // REGIMM rt=2 reserved
    CHECK(Decode(0x08000000u, 0x8001FFFCu).target == 0x80000000u);                   // j keeps the 256 MB region
}

static void TestCfgCallsAndDelaySlots() {
    // 0: jal F1 ; 1: nop ; 2: beq zero,zero,+1 ; 3: nop ; 4: jr ra ; 5: nop
    // F1 (6): addiu v0 ; 7: jal F2 ; 8: nop ; 9: jr ra ; 10: nop
    // F2 (11): lw ; 12: jr t0 (unresolved) ; 13: nop
    auto code = Bytes({Jal(kBase + 6 * 4), kNop, BeqZero(1), kNop, kJrRa, kNop,
                       kAddiuV0One, Jal(kBase + 11 * 4), kNop, kJrRa, kNop,
                       kLw, kJrT0, kNop});
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.text_words == 14);
    CHECK(r.reachable_words == 14 && r.coverage_permille == 1000);
    CHECK(r.functions == 3);
    CHECK(r.function_entries.size() == 3 && r.function_entries[1] == kBase + 6 * 4);
    CHECK(r.direct_calls == 2);
    CHECK(r.conditional_branches == 1);
    CHECK(r.returns == 2);
    CHECK(r.indirect_jumps == 1 && r.indirect_sites.size() == 1 && r.indirect_sites[0] == kBase + 12 * 4);
    CHECK(r.invalid_reached == 0 && r.out_of_range_targets == 0);
}

static void TestCfgInvalidReached() {
    // beq jumps over the nop straight onto a reserved word
    auto code = Bytes({BeqZero(1), kNop, kInvalid});
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.invalid_reached == 1 && r.invalid_sites.size() == 1 && r.invalid_sites[0] == kBase + 8);
    CHECK(r.reachable_words == 3);
}

static void TestCfgOutOfRange() {
    // jal to an address outside the image: counted, not followed
    auto code = Bytes({Jal(0x80100000u), kNop, kJrRa, kNop});
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.direct_calls == 1 && r.out_of_range_targets == 1 && r.out_of_range_sites[0] == kBase);
    CHECK(r.functions == 1 && r.coverage_permille == 1000);

    // entry point outside the image
    CfgReport e = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase + 0x1000);
    CHECK(e.out_of_range_targets == 1 && e.reachable_words == 0 && e.functions == 0);

    // code that runs off the end of the image
    auto open = Bytes({kAddiuV0One, kAddiuV0One});
    CfgReport o = AnalyzeControlFlow(open.data(), open.size(), kBase, kBase);
    CHECK(o.out_of_range_targets == 1 && o.reachable_words == 2);
}

static void TestCfgCoverage() {
    // Four words, but only the first two are reachable from the entry point.
    auto code = Bytes({kJrRa, kNop, kAddiuV0One, kAddiuV0One});
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.reachable_words == 2 && r.coverage_permille == 500);
    CHECK(r.returns == 1);
}


// Switch-table dispatch as the compiler emits it:
//   sltiu t0,a0,4 ; beq t0,zero,default ; nop          -- bound check (index < 4)
//   sll t1,a0,2 ; lui at,hi ; addiu at,at,lo            -- table base
//   addu t1,t1,at ; lw t2,0(t1) ; nop ; jr t2 ; nop     -- load target, jump
// The four table entries sit in the image after the code and point at four small cases.
static void TestCfgJumpTable() {
    const uint32_t table = kBase + 16 * 4;     // word 16 holds the table
    const uint32_t hi = table >> 16;
    const uint32_t lo = table & 0xFFFF;
    const uint32_t lui = 0x3C010000u | hi;     // lui at,hi
    const uint32_t addiu = 0x24210000u | lo;   // addiu at,at,lo
    // 0 sltiu t0,a0,4 ; 1 beq t0,zero,+? ; 2 nop ; 3 sll t1,a0,2 ; 4 lui at ; 5 addiu at
    // 6 addu t1,t1,at ; 7 lw t2,0(t1) ; 8 nop ; 9 jr t2 ; 10 nop
    // 11 default: jr ra ; 12 nop
    // 13..15 padding (unreachable filler, as in the real image)
    // 16..19 table entries pointing at cases 20, 21, 22, 23
    // 20..23 cases: each is addiu v0,zero,n ; jr ra ; nop (three words each, padded to 4)
    auto code = Bytes({
        0x2C880004u,                 // 0  sltiu t0,a0,4
        0x11000000u | 9u,            // 1  beq t0,zero,+9 (to default at word 11)
        kNop,                        // 2
        0x00044880u,                 // 3  sll t1,a0,2
        lui,                         // 4  lui at,hi
        addiu,                       // 5  addiu at,at,lo
        0x00294821u,                 // 6  addu t1,t1,at
        0x8D2A0000u,                 // 7  lw t2,0(t1)
        kNop,                        // 8
        0x01400008u,                 // 9  jr t2
        kNop,                        // 10
        kJrRa,                       // 11 default: jr ra
        kNop,                        // 12
        kNop, kNop, kNop,            // 13-15
        0, 0, 0, 0,                  // 16-19 table entries, patched below
        0, 0, 0, 0, 0, 0, 0, 0,      // 20-27 case blocks, filled in below
        0, 0, 0, 0, 0, 0, 0, 0,
    });
    // Patch the four table entries and the four cases.
    // Each case block is four words long, so case i starts at word 20 + 4*i.
    const uint32_t cases[4] = {kBase + (20 + 4 * 0) * 4, kBase + (20 + 4 * 1) * 4, kBase + (20 + 4 * 2) * 4, kBase + (20 + 4 * 3) * 4};
    for (int i = 0; i < 4; ++i) {
        uint32_t w = cases[i];
        for (int b = 0; b < 4; ++b) code[(16 + i) * 4 + b] = static_cast<uint8_t>(w >> (8 * b));
    }
    // cases at 20..23: addiu v0,zero,i ; jr ra ; nop ; nop
    for (int i = 0; i < 4; ++i) {
        uint32_t words[4] = {0x24020000u | static_cast<uint32_t>(i), kJrRa, kNop, kNop};
        for (int k = 0; k < 4; ++k) {
            uint32_t w = words[k];
            for (int b = 0; b < 4; ++b) code[(20 + 4 * i + k) * 4 + b] = static_cast<uint8_t>(w >> (8 * b));
        }
    }

    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    // Every case must be reached through the table, so the indirect jump is resolved.
    CHECK(r.indirect_jumps == 1);
    CHECK(r.resolved_jump_tables == 1);
    CHECK(r.resolved_table_entries == 4);
    CHECK(r.indirect_sites.empty());
    // Reached: words 0-12 (bound check, dispatch, default with its delay slot) and, through the table,
    // the first three words of each of the four case blocks (20+4i .. 22+4i). The fourth word of each
    // case block is never reached as code, and padding 13-15 and table words 16-19 are data.
    CHECK(r.reachable_words == 25);
    CHECK(r.functions == 5);            // entry point plus one function per table case
    CHECK(r.invalid_reached == 0 && r.out_of_range_targets == 0);
}

// A bound-checked table whose first entry points outside the image: that entry must not be
// followed, must be counted as an out-of-range target, and the jump site must be reported.
static void TestCfgJumpTableOutOfRange() {
    const uint32_t table = kBase + 16 * 4;
    const uint32_t lui = 0x3C010000u | (table >> 16);
    const uint32_t addiu = 0x24210000u | (table & 0xFFFF);
    auto code = Bytes({
        0x2C880004u, 0x11000009u, kNop, 0x00044880u, lui, addiu,
        0x00294821u, 0x8D2A0000u, kNop, 0x01400008u, kNop,
        kJrRa, kNop, kNop, kNop, kNop,
        0x80100000u, kBase + 20 * 4, kBase + 24 * 4, kBase + 28 * 4,   // entry 0 is outside the image
        kJrRa, kNop, kNop, kNop, kJrRa, kNop, kNop, kNop, kJrRa, kNop, kNop, kNop, kJrRa, kNop, kNop, kNop,
    });
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.resolved_jump_tables == 0);               // no entry was read, so the table is not resolved
    CHECK(r.resolved_table_entries == 0);             // the first entry stops the table walk
    CHECK(r.out_of_range_targets == 1);
    CHECK(r.out_of_range_sites.size() == 1 && r.out_of_range_sites[0] == kBase + 9 * 4);
    CHECK(r.indirect_sites.size() == 1 && r.indirect_sites[0] == kBase + 9 * 4);  // stays unresolved
    CHECK(r.unresolved_contexts.size() == 1 && r.unresolved_contexts[0].site == kBase + 9 * 4);
    bool marked = false;
    for (const auto& line : r.unresolved_contexts[0].window) marked |= line.find("sin resolver") != std::string::npos;
    CHECK(marked);
}

static void TestDescribeInsn() {
    CHECK(DescribeInsn(0x8D2A0000u) == "lw $10,0($9)");
    CHECK(DescribeInsn(0x00294821u) == "addu $9,$1,$9");
    CHECK(DescribeInsn(0x2C880004u) == "sltiu $8,$4,4");
    CHECK(DescribeInsn(0x01400008u) == "jr $10");
    CHECK(DescribeInsn(0x3C018001u) == "lui $1,0x8001");
    CHECK(DescribeInsn(0xFC000000u) == ".word 0xFC000000");
    CHECK(DescribeInsn(0x03E00008u) == "jr $31");
}

static void TestCfgUnresolvedContextLimit() {
    // Five jalr t0 sites in a row, each with a nop delay slot, then jr ra: only four get a context window.
    constexpr uint32_t kJalrT0 = 0x0100F809u;
    auto code = Bytes({kJalrT0, kNop, kJalrT0, kNop, kJalrT0, kNop, kJalrT0, kNop, kJalrT0, kNop, kJrRa, kNop});
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.indirect_calls == 5 && r.indirect_sites.size() == 5);
    CHECK(r.unresolved_contexts.size() == 4);
}

// Switch with a load offset and no addiu, as the compiler emits for large case counts:
//   sltiu $2,$3,4 ; beq $2,$0,default ; nop ; sll $2,$3,2 ; lui $1,hi ; addu $1,$1,$2
//   lw $2,lo($1) ; nop ; jr $2 ; nop
static void TestCfgSwitchOffsetLoad() {
    const uint32_t table = kBase + 16 * 4;                   // 0x80010040: hi = 0x8001, lo = 0x40
    auto code = Bytes({
        0x2C620004u,                                          // 0 sltiu $2,$3,4
        0x10400009u,                                          // 1 beq $2,$0,+9 (to word 11)
        kNop,                                                 // 2
        0x00031080u,                                          // 3 sll $2,$3,2
        0x3C010000u | (table >> 16),                          // 4 lui $1,0x8001
        0x00220821u,                                          // 5 addu $1,$1,$2
        0x8C220000u | (table & 0xFFFF),                       // 6 lw $2,0x40($1)
        kNop,                                                 // 7
        0x00400008u,                                          // 8 jr $2
        kNop,                                                 // 9
        kNop,                                                 // 10
        kJrRa, kNop,                                          // 11-12 default
        kNop, kNop, kNop,                                     // 13-15
        0, 0, 0, 0,                                           // 16-19 table entries (filled below)
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,        // 20-35 case blocks (filled below)
    });
    const uint32_t cases[4] = {kBase + 20 * 4, kBase + 24 * 4, kBase + 28 * 4, kBase + 32 * 4};
    for (int i = 0; i < 4; ++i) {
        const uint32_t w = cases[i];
        for (int b = 0; b < 4; ++b) code[(16 + i) * 4 + b] = static_cast<uint8_t>(w >> (8 * b));
    }
    for (int i = 0; i < 4; ++i) {
        const uint32_t words[4] = {0x24020000u | static_cast<uint32_t>(i), kJrRa, kNop, kNop};
        for (int k = 0; k < 4; ++k)
            for (int b = 0; b < 4; ++b) code[(20 + 4 * i + k) * 4 + b] = static_cast<uint8_t>(words[k] >> (8 * b));
    }
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.resolved_jump_tables == 1);
    CHECK(r.resolved_table_entries == 4);
    CHECK(r.indirect_sites.empty());
    CHECK(r.out_of_range_targets == 0);
}

// `li $10,0xB0 ; jr $10 ; li $9,fn` is a BIOS call, not an indirect jump; execution resumes after it.
static void TestCfgBiosCall() {
    auto code = Bytes({0x240A00B0u, 0x01400008u, 0x24090033u, kJrRa, kNop});
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.bios_calls == 1);
    CHECK(r.indirect_jumps == 0 && r.indirect_sites.empty());
    CHECK(r.returns == 1);
    CHECK(r.reachable_words == 5);
}

// `lui $2,0x8001 ; lw $2,0x100($2) ; jalr $2`: a global function pointer. The value at 0x80010100
// is read from the image, so each status can be checked on its own.
static std::vector<uint8_t> GlobalPointerImage(uint32_t value_at_slot, uint32_t hi, uint32_t off) {
    std::vector<uint8_t> code(0x200, 0);
    auto put = [&](uint32_t off_bytes, uint32_t w) { for (int b = 0; b < 4; ++b) code[off_bytes + b] = static_cast<uint8_t>(w >> (8 * b)); };
    put(0x00, 0x3C020000u | hi);                    // lui $2,hi
    put(0x04, 0x8C420000u | off);                   // lw $2,off($2)
    put(0x08, 0x0040F809u);                         // jalr $31,$2
    put(0x0C, kNop);
    put(0x100, value_at_slot);                      // the slot the pointer is loaded from (image offset 0x100)
    return code;
}

static void TestCfgGlobalPointers() {
    {   // slot holds zero: assigned at run time
        auto code = GlobalPointerImage(0, 0x8001, 0x100);
        CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
        CHECK(r.indirect_calls == 1 && r.pointers.null_value == 1);
        CHECK(r.pointer_samples.size() == 1 && r.pointer_samples[0].address == kBase + 0x100
              && r.pointer_samples[0].status == PointerStatus::Null);
    }
    {   // slot points inside the image
        auto code = GlobalPointerImage(kBase + 0x180, 0x8001, 0x100);
        CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
        CHECK(r.pointers.in_image_value == 1 && r.pointer_samples[0].value == kBase + 0x180);
    }
    {   // slot points outside the image
        auto code = GlobalPointerImage(0x80100000u, 0x8001, 0x100);
        CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
        CHECK(r.pointers.value_outside == 1);
    }
    {   // the base address itself is outside the image
        auto code = GlobalPointerImage(0, 0x8100, 0x100);
        CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
        CHECK(r.pointers.address_outside == 1);
    }
    {   // the pointer comes from a value that is not a constant address
        auto code = Bytes({0x8FA80000u, 0x0100F809u, kNop});   // lw $8,0($29) ; jalr $31,$8
        CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
        CHECK(r.pointers.unrecognised == 1);
    }
}

static void TestCfgUnresolvedJumpCounter() {
    // jr to an unknown register, not a table and not a BIOS vector: counted as unresolved.
    auto code = Bytes({0x01400008u, kNop});                 // jr $10 with $10 unknown
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.indirect_jumps == 1 && r.unresolved_jumps == 1);
}

// The slot at 0x80010100 starts at zero, but the code stores the address of the function at 0x80010040
// into it before calling through it. The store makes that address a candidate.
static void TestCfgGlobalPointerStored() {
    std::vector<uint8_t> code(0x200, 0);
    const uint32_t words[] = {
        0x3C028001u,   // 0 lui $2,0x8001
        0x24420040u,   // 1 addiu $2,$2,0x40          -> 0x80010040
        0x3C038001u,   // 2 lui $3,0x8001
        0xAC620100u,   // 3 sw $2,0x100($3)           -> store to 0x80010100
        0x3C048001u,   // 4 lui $4,0x8001
        0x8C850100u,   // 5 lw $5,0x100($4)           -> load from 0x80010100
        0x00A0F809u,   // 6 jalr $31,$5
        kNop,          // 7 delay slot
        kJrRa, kNop,   // 8-9
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        for (int b = 0; b < 4; ++b) code[i * 4 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.pointers.assigned_in_image == 1 && r.pointers.null_value == 0);
    CHECK(r.pointer_samples.size() == 1 && r.pointer_samples[0].status == PointerStatus::AssignedInImage);
    CHECK(r.pointer_samples[0].candidates.size() == 1 && r.pointer_samples[0].candidates[0] == kBase + 0x40);
    // The call has a candidate but is not followed, so it is counted as assigned, not unresolved.
    CHECK(r.indirect_calls == 1);
    CHECK(r.indirect_calls_assigned == 1 && r.indirect_calls_unresolved == 0);
}

static void TestCfgCallSplitCounters() {
    // One jalr with a constant store (assigned) and one whose base is not constant (unresolved).
    std::vector<uint8_t> code(0x200, 0);
    const uint32_t words[] = {
        0x3C028001u,   // 0 lui $2,0x8001
        0x24420040u,   // 1 addiu $2,$2,0x40
        0x3C038001u,   // 2 lui $3,0x8001
        0xAC620100u,   // 3 sw $2,0x100($3)
        0x3C048001u,   // 4 lui $4,0x8001
        0x8C850100u,   // 5 lw $5,0x100($4)
        0x00A0F809u,   // 6 jalr $31,$5               assigned
        kNop,          // 7
        0x8CA60000u,   // 8 lw $6,0($5)              base $5 comes from memory: not constant
        0x00C0F809u,   // 9 jalr $31,$6               unresolved
        kNop,          // 10
        kJrRa, kNop,   // 11-12
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        for (int b = 0; b < 4; ++b) code[i * 4 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
    CfgReport r = AnalyzeControlFlow(code.data(), code.size(), kBase, kBase);
    CHECK(r.indirect_calls == 2);
    CHECK(r.indirect_calls_assigned == 1);
    CHECK(r.indirect_calls_unresolved == 1);
    CHECK(r.indirect_calls_assigned + r.indirect_calls_unresolved == r.indirect_calls);
}

static std::vector<uint8_t> MakeSlotImage(bool second_value) {
    std::vector<uint8_t> code(0x200, 0);
    std::vector<uint32_t> w = {
        0x3C028001u,   // 0 lui $2,0x8001
        0x24420040u,   // 1 addiu $2,$2,0x40          -> 0x80010040
        0x3C038001u,   // 2 lui $3,0x8001
        0xAC620100u,   // 3 sw $2,0x100($3)           -> slot 0x80010100
    };
    if (second_value) {
        w.push_back(0x24420010u);   // addiu $2,$2,0x10  -> 0x80010050
        w.push_back(0xAC620100u);   // sw $2,0x100($3)
    }
    const uint32_t tail[] = {
        0x3C048001u,   // lui $4,0x8001
        0x8C850100u,   // lw $5,0x100($4)
        0x00A0F809u,   // jalr $31,$5
        kNop,
        kJrRa, kNop,
    };
    w.insert(w.end(), tail, tail + 6);
    for (size_t i = 0; i < w.size(); ++i)
        for (int b = 0; b < 4; ++b) code[i * 4 + b] = static_cast<uint8_t>(w[i] >> (8 * b));
    // Two candidate functions: jr $ra at 0x...40 and 0x...50.
    const size_t f1 = 0x40, f2 = 0x50;
    for (int b = 0; b < 4; ++b) {
        code[f1 + b] = static_cast<uint8_t>(kJrRa >> (8 * b));
        code[f2 + b] = static_cast<uint8_t>(kJrRa >> (8 * b));
    }
    return code;
}

static void TestCfgFollowAssignedPointers() {
    const std::vector<uint8_t> one = MakeSlotImage(false);
    CfgOptions off;
    CfgOptions on;
    on.follow_assigned_pointers = true;

    // Default: the candidate is reported but not followed.
    CfgReport a = AnalyzeControlFlow(one.data(), one.size(), kBase, kBase);
    CHECK(a.indirect_calls_assigned == 1 && a.indirect_calls_followed == 0 && a.followed_candidates == 0);
    CfgReport a2 = AnalyzeControlFlow(one.data(), one.size(), kBase, kBase, off);
    CHECK(a2.reachable_words == a.reachable_words && a2.functions == a.functions);

    // Enabled: the single constant-stored candidate becomes a function, its code is reached.
    CfgReport b = AnalyzeControlFlow(one.data(), one.size(), kBase, kBase, on);
    CHECK(b.indirect_calls_assigned == 1 && b.indirect_calls_followed == 1 && b.followed_candidates == 1);
    CHECK(b.functions == a.functions + 1);
    CHECK(b.reachable_words == a.reachable_words + 2);   // jr $ra + delay slot
    CHECK(b.indirect_calls == a.indirect_calls);

    // Two different constants stored to the slot: ambiguous, so not followed even when enabled.
    const std::vector<uint8_t> two = MakeSlotImage(true);
    CfgReport c = AnalyzeControlFlow(two.data(), two.size(), kBase, kBase, on);
    CHECK(c.indirect_calls_assigned == 1 && c.indirect_calls_followed == 0 && c.followed_candidates == 0);
    CfgReport c0 = AnalyzeControlFlow(two.data(), two.size(), kBase, kBase);
    CHECK(c.reachable_words == c0.reachable_words && c.functions == c0.functions);
}

int main() {
    TestDecode();
    TestCfgCallsAndDelaySlots();
    TestCfgInvalidReached();
    TestCfgOutOfRange();
    TestCfgCoverage();
    TestCfgJumpTable();
    TestCfgJumpTableOutOfRange();
    TestCfgSwitchOffsetLoad();
    TestCfgBiosCall();
    TestCfgGlobalPointers();
    TestCfgGlobalPointerStored();
    TestCfgCallSplitCounters();
    TestCfgFollowAssignedPointers();
    TestCfgUnresolvedJumpCounter();
    TestDescribeInsn();
    TestCfgUnresolvedContextLimit();
    std::cout << "recomp tests passed (" << g_checks << " checks)\n";
    return 0;
}
