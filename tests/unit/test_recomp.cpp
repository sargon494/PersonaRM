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

int main() {
    TestDecode();
    TestCfgCallsAndDelaySlots();
    TestCfgInvalidReached();
    TestCfgOutOfRange();
    TestCfgCoverage();
    std::cout << "recomp tests passed (" << g_checks << " checks)\n";
    return 0;
}
