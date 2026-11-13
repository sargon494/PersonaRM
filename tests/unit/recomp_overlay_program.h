#pragma once

// Synthetic overlay program for the recompiled-execution integration test (no game data). A resident
// main loop keeps loading two different overlays into one slot (A with CPU stores, B with the HLE
// memcpy), calls each natively (resident -> overlay -> resident), lets a resident callee patch the
// instruction its overlay caller runs next (native code must not resume the stale copy), trashes a
// code word of B by DMA (OTC) and polls a timer (lazy device time). A and B start with the same word,
// so only the code hash tells them apart. Shared by the build-time writer (-> tools/recomp_gen,
// modules "rovl_main", "rovl_a", "rovl_b") and the test.

#include "g2_synth_program.h"

namespace rovl {

using namespace g2;

constexpr uint32_t kSlot = 0x80040000u;
constexpr uint32_t kData = 0x80100000u;   // s1: +0 iteration, +8 timer sum, +12/+16 A/B sums, +20 helper calls

inline uint32_t JalTo(uint32_t a) { return 0x0C000000u | ((a >> 2) & 0x03FFFFFFu); }
inline uint32_t Addiu(uint32_t t, uint32_t s, int32_t i) { return (0x09u << 26) | (s << 21) | (t << 16) | (static_cast<uint32_t>(i) & 0xFFFFu); }

struct Emit {
    Asm& A;
    void addu(uint32_t d, uint32_t s, uint32_t t) { A.R(0x21, s, t, d); }
    void addiu(uint32_t t, uint32_t s, int32_t i) { A.W(Addiu(t, s, i)); }
    void lw(uint32_t t, int32_t off, uint32_t b) { A.I(0x23, b, t, static_cast<uint32_t>(off)); }
    void sw(uint32_t t, int32_t off, uint32_t b) { A.I(0x2B, b, t, static_cast<uint32_t>(off)); }
    void li(uint32_t r, uint32_t v) { A.I(0x0F, 0, r, v >> 16); A.I(0x0D, r, r, v & 0xFFFFu); }
    void la(uint32_t r, const std::string& l) { A.Hi(r, l); A.Lo(0x0D, r, r, l); }
};

// variant 0 = A, 1 = B; `helper` and `patch` are resident functions.
inline Program Overlay(int variant, uint32_t helper, uint32_t patch) {
    Asm A(kSlot);
    Emit e{A};
    A.Label("ovl_main");
    e.addiu(sp, sp, -8); e.sw(ra, 0, sp);
    e.addiu(t3, zero, variant ? 7 : 20);
    A.Label("loop");
    e.addiu(t3, t3, -1); A.Br(0x05, t3, zero, "loop"); e.addu(v0, v0, t3);
    A.W(JalTo(helper)); e.addu(a0, v0, zero);
    if (variant == 0) { A.W(JalTo(patch)); A.Nop(); }
    else { A.R(0x26, v0, t3, v0); }                      // B: xor, different code
    A.Label("after_patch");                               // A: patched to addiu v0,v0,100 on odd iterations
    e.addiu(v0, v0, variant ? 5 : 3);
    e.lw(t0, variant ? 16 : 12, s1); A.Nop(); e.addu(t0, t0, v0); e.sw(t0, variant ? 16 : 12, s1);
    A.Hi(t1, "ovl_data"); A.Lo(0x2B, t1, v0, "ovl_data");  // data inside the image: no retire
    e.lw(ra, 0, sp); A.Nop(); A.JrRa(); e.addiu(sp, sp, 8);
    A.Label("ovl_data"); A.W(0);
    A.Resolve();
    Program p;
    p.words = A.words;
    p.fn = A.labels;
    p.entries = {A.labels.at("ovl_main")};
    return p;
}

inline Program Main(const Program& a, const Program& b) {
    Asm A;
    Emit e{A};
    auto stub = [&](const std::string& name, uint32_t vec, uint32_t fn) {   // li t2,vec; jr t2; li t1,fn
        A.Label(name); e.addiu(t2, zero, static_cast<int32_t>(vec)); A.R(0x08, t2, 0, 0); e.addiu(t1, zero, static_cast<int32_t>(fn));
    };
    A.Label("main");
    A.I(0x0F, 0, s1, kData >> 16);
    A.Label("main_loop");
    e.lw(t0, 0, s1); A.Nop(); e.addiu(t0, t0, 1); e.sw(t0, 0, s1);
    A.I(0x0F, 0, t0, 0x1F80); e.lw(t1, 0x1100, t0); e.lw(t2, 8, s1); A.Nop(); e.addu(t2, t2, t1); e.sw(t2, 8, s1);
    // A: copied word by word, then called.
    e.la(a0, "img_a"); A.I(0x0F, 0, a1, kSlot >> 16); e.addiu(a2, zero, static_cast<int32_t>(a.words.size()));
    A.Jal("copy_words"); A.Nop();
    A.W(JalTo(kSlot)); A.Nop();
    e.sw(v0, 24, s1);
    // B: HLE memcpy (A0:2A) over A, then called.
    A.I(0x0F, 0, a0, kSlot >> 16); e.la(a1, "img_b"); e.addiu(a2, zero, static_cast<int32_t>(b.words.size() * 4));
    A.Jal("memcpy_stub"); A.Nop();
    A.W(JalTo(kSlot)); A.Nop();
    e.sw(v0, 28, s1);
    // DMA6 (OTC, one word) over B's code: B retires.
    A.I(0x0F, 0, t0, 0x1F80);
    e.lw(t1, 0x10F0, t0); A.I(0x0F, 0, t2, 0x0800); A.R(0x25, t1, t2, t1); e.sw(t1, 0x10F0, t0);  // DPCR: ch6 on
    e.li(t1, b.fn.at("after_patch")); e.sw(t1, 0x10E0, t0);                                       // MADR
    e.addiu(t1, zero, 1); e.sw(t1, 0x10E4, t0);                                                   // BCR
    e.li(t1, 0x11000002u); e.sw(t1, 0x10E8, t0);                                                  // start + trigger
    A.Br(0x04, zero, zero, "main_loop"); A.Nop();

    A.Label("copy_words");                                   // a0 src, a1 dst, a2 words
    A.Label("cw_top");
    e.lw(t0, 0, a0); e.addiu(a2, a2, -1); e.sw(t0, 0, a1); e.addiu(a0, a0, 4);
    A.Br(0x05, a2, zero, "cw_top"); e.addiu(a1, a1, 4);
    A.JrRa(); A.Nop();

    stub("memcpy_stub", 0xA0, 0x2A);

    A.Label("res_helper");                                   // v0 = 3 * a0, counts calls
    e.lw(t0, 20, s1); A.Nop(); e.addiu(t0, t0, 1); e.sw(t0, 20, s1);
    e.addu(v0, a0, a0); A.JrRa(); e.addu(v0, v0, a0);

    A.Label("res_patch");                                    // odd iterations: patch A's next instruction
    e.lw(t0, 0, s1); A.Nop(); A.I(0x0C, t0, t0, 1); A.Br(0x04, t0, zero, "rp_done"); A.Nop();
    e.li(t1, Addiu(v0, v0, 100)); e.li(t2, a.fn.at("after_patch")); e.sw(t1, 0, t2);
    A.Label("rp_done");
    A.JrRa(); A.Nop();

    A.Label("img_a");
    for (uint32_t w : a.words) A.W(w);
    A.Label("img_b");
    for (uint32_t w : b.words) A.W(w);

    A.Resolve();
    Program p;
    p.words = A.words;
    p.fn = A.labels;
    for (const char* n : {"main", "copy_words", "memcpy_stub", "res_helper", "res_patch"}) p.entries.push_back(A.labels.at(n));
    return p;
}

struct Set { Program main, a, b; };

// Layouts do not depend on the addresses filled in, so a first pass finds the resident helpers.
inline Set Build() {
    const Program a0 = Overlay(0, 0, 0), b0 = Overlay(1, 0, 0);
    const Program m0 = Main(a0, b0);
    Set s;
    s.a = Overlay(0, m0.fn.at("res_helper"), m0.fn.at("res_patch"));
    s.b = Overlay(1, m0.fn.at("res_helper"), m0.fn.at("res_patch"));
    s.main = Main(s.a, s.b);
    return s;
}

} // namespace rovl
