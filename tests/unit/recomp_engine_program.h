#pragma once

// Synthetic boot program for the recompiled-execution integration test (no game data): a main loop
// that keeps Timer2 and VBlank IRQs coming (guest handler registered with SysEnqIntRP), polls MMIO,
// makes syscalls and A0/C0 BIOS calls through stubs, calls a function nobody compiled (the native
// stack unwinds and resumes at the return site) and one the generator refuses (load-delay hazard).
// Shared by the build-time writer (-> tools/recomp_gen, module "rengine") and the test.

#include "g2_synth_program.h"

namespace rengine {

using namespace g2;

constexpr uint32_t kData = 0x80100000u;   // s1: counters, MMIO log ring at +0x40/+0x80, memset area

inline Program Build() {
    Asm A;
    auto addu = [&](uint32_t d, uint32_t s, uint32_t t) { A.R(0x21, s, t, d); };
    auto addiu = [&](uint32_t t, uint32_t s, int32_t i) { A.I(0x09, s, t, static_cast<uint32_t>(i)); };
    auto lw = [&](uint32_t t, int32_t off, uint32_t b) { A.I(0x23, b, t, static_cast<uint32_t>(off)); };
    auto sw = [&](uint32_t t, int32_t off, uint32_t b) { A.I(0x2B, b, t, static_cast<uint32_t>(off)); };
    auto la = [&](uint32_t r, const std::string& l) { A.Hi(r, l); A.Lo(0x0D, r, r, l); };   // lui + ori
    auto syscall = [&]() { A.W(0x0000000Cu); };
    auto stub = [&](const std::string& name, uint32_t vec, uint32_t fn) {   // li t2,vec; jr t2; li t1,fn
        A.Label(name); addiu(t2, zero, static_cast<int32_t>(vec)); A.R(0x08, t2, 0, 0); addiu(t1, zero, static_cast<int32_t>(fn));
    };

    A.Label("main");
    A.I(0x0F, 0, t0, 0x1F80);
    addiu(t1, zero, 0x1000); sw(t1, 0x1128, t0);          // Timer2 target
    addiu(t1, zero, 0x0258); sw(t1, 0x1124, t0);          // sysclk/8, IRQ at target, repeat, reset at target
    addiu(t1, zero, 0x41); sw(t1, 0x1074, t0);            // I_MASK: VBlank, Timer2
    A.I(0x0F, 0, s1, kData >> 16);
    addiu(s0, s1, 0x200);                                  // SysEnqIntRP element {next, func2, func1, 0}
    sw(zero, 0, s0); sw(zero, 4, s0); la(t1, "irq_handler"); sw(t1, 8, s0); sw(zero, 12, s0);
    addiu(a0, zero, 1); A.Jal("enqint"); addu(a1, s0, zero);
    addiu(t1, zero, 0x401); A.W(0x40800000u | (t1 << 16) | (12u << 11));   // mtc0 t1, SR: IEc + IM2
    A.Label("main_loop");
    A.Jal("work"); A.Nop();
    A.Jal("poll"); A.Nop();
    A.Jal("crit"); A.Nop();
    A.Jal("memset_call"); A.Nop();
    A.Jal("unlisted"); A.Nop();
    A.Jal("hazard"); A.Nop();
    A.Jal("spin"); A.Nop();
    A.Br(0x04, zero, zero, "main_loop"); A.Nop();

    // Sums Timer0 reads with IRQs masked (like a DrawSync wait), so a frame can end and the next
    // one start right at the MMIO load: any lag between the CPU and the devices shows in RAM.
    A.Label("spin");
    A.W(0x40000000u | (t6 << 16) | (12u << 11));           // mfc0 t6, SR
    A.I(0x0F, 0, t0, 0x1F80); addiu(t7, zero, -2); A.R(0x24, t6, t7, t7);
    A.W(0x40800000u | (t7 << 16) | (12u << 11));           // mtc0 t7, SR: IEc off
    addiu(t3, zero, 3000); lw(t5, 0x3C, s1);
    A.Label("spin_top");
    lw(t1, 0x1100, t0); addiu(t3, t3, -1); addu(t5, t5, t1);
    A.I(0x22, s1, t7, 0x45); A.I(0x26, s1, t7, 0x42);     // lwl/lwr pair into t7 (unaligned word)
    A.Br(0x05, t3, zero, "spin_top"); addu(t5, t5, t3);
    A.W(0x40800000u | (t6 << 16) | (12u << 11));           // mtc0 t6, SR: restore
    A.JrRa(); sw(t5, 0x3C, s1);

    A.Label("work");                                       // RAM loads (stalls), a load in a delay slot
    addiu(t3, zero, 50);
    A.Label("work_top");
    lw(t4, 0, s1); addiu(t3, t3, -1); addu(t4, t4, t3); sw(t4, 0, s1);
    lw(t5, 4, s1); A.Nop(); addu(t5, t5, t4); sw(t5, 4, s1);
    A.Br(0x05, t3, zero, "work_top"); lw(t6, 8, s1);
    A.Nop(); addu(v0, t5, t6); sw(v0, 8, s1);
    A.JrRa(); A.Nop();

    A.Label("poll");                                       // MMIO reads logged to RAM: device timing shows
    A.I(0x0F, 0, t0, 0x1F80);
    lw(t1, 0x1120, t0); lw(t2, 0x1070, t0); lw(t3, 16, s1); A.Nop();
    A.I(0x0C, t3, t3, 0x3C); addu(t4, s1, t3);
    sw(t1, 0x40, t4); sw(t2, 0x80, t4); addiu(t3, t3, 4);
    A.JrRa(); sw(t3, 16, s1);

    A.Label("crit");                                       // syscalls: exceptions out of native code
    addiu(a0, zero, 1); syscall();
    lw(t0, 20, s1); A.Nop(); addiu(t0, t0, 1); sw(t0, 20, s1);
    addiu(a0, zero, 2); syscall();
    A.JrRa(); A.Nop();

    A.Label("memset_call");                                // A0:2B memset through a stub, from native code
    addiu(sp, sp, -8); sw(ra, 0, sp);
    addiu(a0, s1, 0x100); lw(a1, 0, s1); addiu(a2, zero, 32);
    A.Jal("memset_stub"); A.Nop();
    lw(ra, 0, sp); A.Nop(); A.JrRa(); addiu(sp, sp, 8);

    stub("memset_stub", 0xA0, 0x2B);
    stub("enqint", 0xC0, 0x02);

    A.Label("unlisted");                                   // not in the entry list: dispatch miss
    lw(t0, 24, s1); A.Nop(); addiu(t0, t0, 3); A.JrRa(); sw(t0, 24, s1);

    A.Label("hazard");                                     // delayed load read at once: never generated
    lw(t0, 28, s1); addiu(t0, t0, 1); sw(t0, 28, s1); A.JrRa(); A.Nop();

    A.Label("irq_handler");                                // guest IRQ handler: count, log, acknowledge
    A.I(0x0F, 0, t0, 0x1F80); A.I(0x0F, 0, t3, kData >> 16);
    lw(t1, 0x1070, t0); lw(t2, 32, t3); A.Nop(); addiu(t2, t2, 1); sw(t2, 32, t3);
    sw(t1, 36, t3); A.R(0x27, t1, zero, t1); sw(t1, 0x1070, t0);
    A.JrRa(); addu(v0, zero, zero);

    A.Resolve();
    Program p;
    p.words = A.words;
    p.fn = A.labels;
    for (const char* n : {"main", "spin", "work", "poll", "crit", "memset_call", "memset_stub", "enqint", "hazard", "irq_handler"})
        p.entries.push_back(A.labels.at(n));
    return p;
}

} // namespace rengine
