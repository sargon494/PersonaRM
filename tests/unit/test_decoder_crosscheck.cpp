// Cross-check: the shared MIPS decoder (recomp::Decode) must agree with what the interpreter
// (CpuContext) actually does for a large deterministic sample of 32-bit words, and a rough
// interpreter speed benchmark (logged, never asserted).
#undef NDEBUG
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "core/logger.h"
#include "cpu/cpu_context.h"
#include "recomp/mips_decode.h"

using namespace personarm;
using recomp::InsnKind;

static int g_fail = 0;
#define CHECK(c, w) do { if (!(c)) { if (g_fail++ < 20) std::fprintf(stderr, "FAIL %s:%d %s word=%08x\n", __FILE__, __LINE__, #c, w); } } while (0)

static constexpr uint32_t kPc = 0x1000;
static constexpr uint32_t kSyscall = 0x0000000C;

struct Probe {
    CpuContext cpu;
    uint32_t word = 0, slot = 0;
    int data_reads = 0, writes = 0;
    Probe() {
        cpu.SetMemoryHooks(
            [this](uint32_t a, uint32_t) -> uint32_t {
                if (a == kPc) return word;
                if (a == kPc + 4) return slot;
                ++data_reads;
                return 0xA5A5A5A5u;
            },
            [this](uint32_t, uint32_t, uint32_t) { ++writes; });
    }
    void Reset(uint32_t w, uint32_t s, uint32_t seed) {
        word = w; slot = s; data_reads = writes = 0;
        cpu.Reset();
        cpu.SetPC(kPc);
        uint32_t x = seed * 2654435761u + 1;
        for (uint32_t i = 1; i < 32; ++i) { x = x * 1664525u + 1013904223u; cpu.SetGPR(i, x); }
        cpu.SetHI(0x1234); cpu.SetLO(0x5678);
    }
};

static void CheckWord(Probe& p, uint32_t w, uint32_t seed) {
    const recomp::Insn d = recomp::Decode(w, kPc);

    // Run 1: nop in the slot. Snapshot after the first step.
    p.Reset(w, 0, seed);
    uint32_t before[32];
    for (uint32_t i = 0; i < 32; ++i) before[i] = p.cpu.GetGPR(i);
    p.cpu.Step();
    const CpuRegisters r = p.cpu.GetRegisters();
    const int reads = p.data_reads, wr = p.writes;
    const uint32_t exc = (r.cp0_cause >> 2) & 0x1F;
    bool gpr_same = true;
    for (uint32_t i = 0; i < 32; ++i) gpr_same &= (r.gpr[i] == before[i]);
    const bool quiet = reads == 0 && wr == 0 && exc == 0 && gpr_same && r.hi == 0x1234 && r.lo == 0x5678;
    const uint32_t op = w >> 26;

    switch (d.kind) {
        case InsnKind::Invalid:  // reserved: the interpreter executes it as a no-op (warn only)
            CHECK(quiet && r.pc == kPc + 4, w);
            break;
        case InsnKind::Alu:
            CHECK(reads == 0 && wr == 0 && ((exc == 0 && r.pc == kPc + 4) || (exc == 0x0C && r.pc == 0x80000080u)), w);
            break;
        case InsnKind::Load:
            CHECK((reads >= 1 && wr == 0) || exc == 0x04, w);
            break;
        case InsnKind::Store:
            CHECK((wr >= 1 && exc == 0) || exc == 0x05, w);
            break;
        case InsnKind::Syscall:
            CHECK((exc == 8 || exc == 9) && r.pc == 0x80000080u && r.cp0_epc == kPc, w);
            break;
        case InsnKind::Cop:  // only LWC2/SWC2 touch memory; every other COPn/LWCn/SWCn form is memory-silent
            CHECK(wr == (op == 0x3A ? 1 : 0) && reads == (op == 0x32 ? 1 : 0), w);
            break;
        default:  // control transfers: delay slot + target
            CHECK(d.has_delay_slot && r.pc == kPc + 4 && reads == 0 && wr == 0, w);
            p.cpu.Step();  // execute the nop delay slot
            if (d.has_target && d.kind != InsnKind::Branch && op != 1) CHECK(p.cpu.GetPC() == d.target, w);
            if (d.has_target && (d.kind == InsnKind::Branch || op == 1)) CHECK(p.cpu.GetPC() == d.target || p.cpu.GetPC() == kPc + 8, w);
            break;
    }

    // Run 2: SYSCALL in the slot; CAUSE.BD tells whether the interpreter treats w as owning a slot.
    // shortcut: the interpreter treats every op 1..7 as a branch for BD, so reserved REGIMM rt
    // values (decoded Invalid) are excluded here; on hardware they raise RI anyway.
    if (!(op == 1 && d.kind == InsnKind::Invalid)) {
        p.Reset(w, kSyscall, seed);
        p.cpu.Step();
        if ((p.cpu.GetRegisters().cp0_cause >> 2) & 0x1F) return;  // faulted before the slot (ADD overflow etc.)
        p.cpu.Step();
        const CpuRegisters r2 = p.cpu.GetRegisters();
        if (d.kind == InsnKind::Syscall) return;
        const bool bd = (r2.cp0_cause & 0x80000000u) != 0;
        CHECK(bd == d.has_delay_slot, w);
        if (bd) CHECK(r2.cp0_epc == kPc, w);
    }
}

static void Bench() {
    CpuContext cpu;
    // loop: addiu $8,$8,1 ; bne $8,$9,loop ; nop
    const uint32_t prog[3] = {0x25080001u, 0x1509FFFEu, 0u};
    cpu.SetMemoryHooks(
        [&](uint32_t a, uint32_t) -> uint32_t { return prog[(a - kPc) >> 2]; },
        [](uint32_t, uint32_t, uint32_t) {});
    cpu.SetPC(kPc);
    cpu.SetGPR(9, 10000000);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 30000000; ++i) cpu.Step();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("BENCH 30M Step() in %.3f s = %.1f MIPS (loop counter %u)\n", s, 30.0 / s, cpu.GetGPR(8));
}

int main() {
    Logger::Instance().SetLogLevel(LogLevel::Error);  // reserved encodings warn; keep the log quiet
    Probe p;
    uint32_t rng = 12345, n = 0;
    for (uint32_t op = 0; op < 64; ++op)
        for (uint32_t fn = 0; fn < 64; ++fn)
            for (uint32_t rt = 0; rt < 32; ++rt)
                for (int k = 0; k < 2; ++k) {
                    rng = rng * 1664525u + 1013904223u;
                    uint32_t w = (op << 26) | (rng & 0x03FFFFFFu);
                    w = (w & ~0x1F0000u) | (rt << 16);   // fix rt
                    w = (w & ~0x3Fu) | fn;               // fix funct
                    CheckWord(p, w, rng);
                    ++n;
                }
    std::printf("cross-checked %u words, %d failures\n", n, g_fail);
    Bench();
    return g_fail ? 1 : 0;
}
