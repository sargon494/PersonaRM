#pragma once

// Synthetic MIPS-I program for the G2 differential test (no game data). Shared by the build-time
// writer (g2_synth_writer.cpp -> .bin + entry list -> tools/recomp_gen) and the test itself.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace g2 {

constexpr uint32_t kBase = 0x80010000u;

enum Reg : uint32_t { zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
                      s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra };

class Asm {
public:
    uint32_t Here() const { return kBase + 4 * static_cast<uint32_t>(words.size()); }
    void Label(const std::string& n) { labels[n] = Here(); }
    void W(uint32_t w) { words.push_back(w); }

    void R(uint32_t fn, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh = 0) { W((rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn); }
    void I(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) { W((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu)); }
    void Br(uint32_t op, uint32_t rs, uint32_t rt, const std::string& l) { fix.push_back({words.size(), l}); I(op, rs, rt, 0); }
    void Jal(const std::string& l) { jfix.push_back({words.size(), l}); W(0x0C000000u); }
    void Hi(uint32_t rt, const std::string& l) { hfix.push_back({words.size(), l}); I(0x0F, 0, rt, 0); }
    void Lo(uint32_t op, uint32_t rs, uint32_t rt, const std::string& l) { lfix.push_back({words.size(), l}); I(op, rs, rt, 0); }

    void Nop() { W(0); }
    void JrRa() { R(0x08, ra, 0, 0); }

    void Resolve() {
        for (auto& f : fix) {
            const uint32_t at_addr = kBase + 4 * static_cast<uint32_t>(f.first);
            words[f.first] |= ((labels.at(f.second) - (at_addr + 4)) >> 2) & 0xFFFFu;
        }
        for (auto& f : jfix) words[f.first] |= (labels.at(f.second) >> 2) & 0x03FFFFFFu;
        for (auto& f : hfix) words[f.first] |= labels.at(f.second) >> 16;
        for (auto& f : lfix) words[f.first] |= labels.at(f.second) & 0xFFFFu;
    }

    std::vector<uint32_t> words;
    std::map<std::string, uint32_t> labels;

private:
    std::vector<std::pair<size_t, std::string>> fix, jfix, hfix, lfix;
};

// Named entry points; `entries` excludes "unlisted" (reached only through the interpreter fallback).
struct Program {
    std::vector<uint32_t> words;
    std::map<std::string, uint32_t> fn;
    std::vector<uint32_t> entries;
};

inline Program Build() {
    Asm A;
    auto addu = [&](uint32_t d, uint32_t s, uint32_t t) { A.R(0x21, s, t, d); };
    auto addiu = [&](uint32_t t, uint32_t s, int32_t i) { A.I(0x09, s, t, static_cast<uint32_t>(i)); };
    const std::vector<std::string> names = {"arith", "divs", "loop", "branches", "bal_sub", "mem", "ovf", "ovf_ds",
                                            "chain_a", "chain_b", "chain_c", "switch", "jalr", "jalr_unk", "unlisted",
                                            "gte", "cop0", "sys", "brk", "adel", "below", "hazard"};

    A.Label("arith");                                   // every ALU form, delay slot does work
    addu(v0, a0, a1); A.R(0x23, a0, a1, v1); A.R(0x24, a0, a1, t0); A.R(0x25, a0, a1, t1);
    A.R(0x26, a0, a1, t2); A.R(0x27, a0, a1, t3); A.R(0x2A, a0, a1, t4); A.R(0x2B, a0, a1, t5);
    A.R(0x00, 0, a0, t6, 5); A.R(0x02, 0, a0, t7, 7); A.R(0x03, 0, a0, t8, 9);
    A.R(0x04, a1, a0, t9); A.R(0x06, a1, a2, s0); A.R(0x07, a1, a2, s1);
    A.R(0x18, a0, a1, 0); A.R(0x12, 0, 0, s2); A.R(0x10, 0, 0, s3);
    A.R(0x19, a2, a3, 0); A.R(0x12, 0, 0, s4); A.R(0x10, 0, 0, s5);
    A.R(0x11, a3, 0, 0); A.R(0x13, a2, 0, 0);           // mthi/mtlo
    addiu(s6, a0, -12345); A.I(0x0A, a0, s7, 0xFF00u); A.I(0x0B, a1, k0, 0x8001u);
    A.I(0x0C, a2, k1, 0xF0F0u); A.I(0x0D, a2, gp, 0x1234u); A.I(0x0E, a3, fp, 0xFFFFu); A.I(0x0F, 0, at, 0xBEEFu);
    A.R(0x21, a0, a1, 0);                               // write to $zero is discarded
    A.JrRa(); addu(v0, v0, t0);

    A.Label("divs");
    A.R(0x1A, a0, a1, 0); A.R(0x12, 0, 0, v0); A.R(0x10, 0, 0, v1);
    A.R(0x1B, a0, a1, 0); A.R(0x12, 0, 0, t0); A.R(0x10, 0, 0, t1);
    A.R(0x1A, a2, a3, 0); A.R(0x12, 0, 0, t2); A.JrRa(); A.R(0x10, 0, 0, t3);

    A.Label("loop");                                    // v0 = sum, backward bne with work in the slot
    A.I(0x0C, a0, a0, 0x3F); A.R(0x21, 0, 0, v0);
    A.Br(0x04, a0, zero, "loop_end"); A.Nop();
    A.Label("loop_top");
    addiu(a0, a0, -1); A.Br(0x05, a0, zero, "loop_top"); addu(v0, v0, a0);
    A.Label("loop_end");
    A.JrRa(); A.Nop();

    A.Label("branches");                                // every conditional form, bltzal/bgezal
    A.R(0x21, ra, 0, t9); A.R(0x21, 0, 0, v0); A.R(0x21, 0, 0, v1);
    A.Br(0x01, a0, 0x00, "b1"); A.I(0x0D, v0, v0, 1); A.I(0x0D, v0, v0, 2); A.Label("b1");     // bltz
    A.Br(0x01, a0, 0x01, "b2"); A.I(0x0D, v0, v0, 4); A.I(0x0D, v0, v0, 8); A.Label("b2");     // bgez
    A.Br(0x06, a1, 0, "b3"); A.I(0x0D, v0, v0, 16); A.I(0x0D, v0, v0, 32); A.Label("b3");     // blez
    A.Br(0x07, a1, 0, "b4"); A.I(0x0D, v0, v0, 64); A.I(0x0D, v0, v0, 128); A.Label("b4");    // bgtz
    A.Br(0x04, a2, a3, "b5"); A.I(0x0D, v0, v0, 256); A.I(0x0D, v0, v0, 512); A.Label("b5");  // beq
    A.Br(0x05, a2, a3, "b6"); A.I(0x0D, v0, v0, 1024); A.I(0x0D, v0, v0, 2048); A.Label("b6"); // bne
    A.Br(0x01, a0, 0x10, "bal_sub"); A.Nop();           // bltzal
    A.Br(0x01, a1, 0x11, "bal_sub"); A.Nop();           // bgezal
    A.R(0x21, t9, 0, ra); A.JrRa(); A.Nop();

    A.Label("bal_sub");
    A.JrRa(); addiu(v1, v1, 100);

    A.Label("mem");                                     // a0 = word-aligned buffer, a1 = 0..3
    A.I(0x0C, a1, a1, 3); addu(t0, a0, a1);
    A.I(0x26, t0, t1, 0); A.I(0x22, t0, t1, 3); A.Nop(); // lwr/lwl unaligned word
    A.I(0x26, t0, t2, 1); A.I(0x22, t0, t2, 6); A.Nop(); // other pairing (rt merge)
    A.I(0x2E, t0, t1, 16); A.I(0x2A, t0, t1, 19);       // swr/swl
    A.I(0x2A, t0, t2, 24); A.I(0x2E, t0, t2, 29);
    A.I(0x20, t0, t3, 1); A.I(0x24, t0, t4, 2); A.I(0x21, a0, t5, 2); A.I(0x25, a0, t6, 6);
    A.I(0x23, a0, t7, 4); A.Nop();
    A.I(0x28, t0, t7, 40); A.I(0x29, a0, t3, 42); A.I(0x2B, a0, t4, 44);
    A.I(0x2B, a0, t5, 48); A.JrRa(); A.I(0x2B, a0, t6, 52);

    A.Label("ovf");                                     // traps leave rd untouched
    A.R(0x20, a0, a1, v0); A.I(0x08, a0, v1, 0x7FFF); A.I(0x08, a1, t0, 0x8000);
    A.R(0x22, a0, a2, t1); A.JrRa(); A.Nop();

    A.Label("ovf_ds");                                  // overflow in a delay slot: EPC/BD
    A.JrRa(); A.R(0x20, a0, a1, v0);

    A.Label("chain_a");                                 // jal chain with stack frames
    addiu(sp, sp, -8); A.I(0x2B, sp, ra, 0);
    A.Jal("chain_b"); addiu(a0, a0, 1);
    A.I(0x23, sp, ra, 0); addiu(sp, sp, 8); A.JrRa(); A.Nop();
    A.Label("chain_b");
    addiu(sp, sp, -8); A.I(0x2B, sp, ra, 0);
    A.Jal("chain_c"); A.R(0x00, 0, a0, a0, 1);
    A.I(0x23, sp, ra, 0); A.Nop(); A.JrRa(); addiu(sp, sp, 8);
    A.Label("chain_c");
    addu(v0, a0, a0); A.JrRa(); addiu(v0, v0, 3);

    A.Label("switch");                                  // jr through a bounded jump table
    A.I(0x0B, a0, t0, 4); A.Br(0x04, t0, zero, "sw_def"); A.R(0x00, 0, a0, t1, 2);
    A.Hi(t2, "sw_table"); addu(t2, t2, t1); A.Lo(0x23, t2, t3, "sw_table"); A.Nop();
    A.R(0x08, t3, 0, 0); A.Nop();
    A.Label("sw_0"); A.JrRa(); addiu(v0, zero, 10);
    A.Label("sw_1"); addiu(v1, zero, 7); A.JrRa(); addiu(v0, zero, 11);
    A.Label("sw_2"); A.Br(0x04, zero, zero, "sw_1"); addiu(v0, zero, 12);
    A.Label("sw_3"); A.JrRa(); addiu(v0, zero, 13);
    A.Label("sw_def"); A.JrRa(); addiu(v0, zero, -1);

    A.Label("jalr");                                    // jalr to a compiled function
    addiu(sp, sp, -8); A.I(0x2B, sp, ra, 0);
    A.Hi(t0, "chain_c"); A.Lo(0x0D, t0, t0, "chain_c");
    A.R(0x09, t0, 0, ra); A.Nop();
    A.I(0x23, sp, ra, 0); A.Nop(); A.JrRa(); addiu(sp, sp, 8);

    A.Label("jalr_unk");                                // jalr to code nobody compiled: fallback
    addiu(sp, sp, -8); A.I(0x2B, sp, ra, 0);
    A.Hi(t0, "unlisted"); A.Lo(0x0D, t0, t0, "unlisted");
    A.R(0x09, t0, 0, ra); addiu(a0, a0, 5);
    addiu(v1, v0, 1); A.I(0x23, sp, ra, 0); A.Nop(); A.JrRa(); addiu(sp, sp, 8);
    A.Label("unlisted");
    A.R(0x00, 0, a0, v0, 3); A.JrRa(); A.Nop();

    A.Label("gte");                                     // GTE through the same Gte class
    A.I(0x12, 0x04, a0, 9 << 11); A.I(0x12, 0x04, a1, 10 << 11); A.I(0x12, 0x04, a2, 11 << 11);
    A.I(0x12, 0x06, a3, 0 << 11);                       // ctc2 RT11RT12
    A.W(0x4AA00428u);                                   // sqr sf=1
    A.I(0x12, 0x00, v0, 25 << 11); A.I(0x12, 0x02, v1, 31 << 11); A.Nop();
    A.I(0x12, 0x04, a0, 0 << 11); A.I(0x12, 0x04, a1, 1 << 11); A.W(0x4A280030u);   // rtps
    A.I(0x12, 0x00, t0, 14 << 11); A.Nop();
    A.Hi(t1, "gte_buf"); A.Lo(0x09, t1, t1, "gte_buf");
    A.I(0x3A, t1, 25, 0); A.I(0x32, t1, 8, 0); A.JrRa(); A.Nop();

    A.Label("cop0");
    A.I(0x10, 0x00, t0, 12 << 11); A.Nop(); A.I(0x0D, t0, t0, 0x0300);
    A.I(0x10, 0x04, t0, 12 << 11); A.I(0x10, 0x00, v0, 12 << 11);
    A.I(0x10, 0x04, a0, 13 << 11); A.I(0x10, 0x00, v1, 13 << 11);
    A.I(0x10, 0x04, a1, 14 << 11); A.I(0x10, 0x00, t1, 14 << 11);
    A.I(0x10, 0x00, t2, 3 << 11);                       // unmodelled: keeps its value
    A.W(0x42000010u);                                   // rfe
    A.JrRa(); A.Nop();

    A.Label("sys"); addiu(v0, zero, 5); A.W(0x0000000Cu); A.JrRa(); A.Nop();
    A.Label("brk"); A.JrRa(); A.W(0x0001000Du);         // break in a delay slot
    A.Label("adel"); A.I(0x0D, a0, t0, 1); A.I(0x23, t0, v0, 0); A.JrRa(); A.Nop();

    A.Label("below_body");                              // reached only from "below", at a lower address
    addiu(v0, v0, 1); A.JrRa(); A.Nop();
    A.Label("below");
    addiu(v0, zero, 5); A.Br(0x04, zero, zero, "below_body"); A.Nop();

    A.Label("hazard");                                 // uses a load in its delay: never generated
    A.I(0x23, a0, t0, 0); addu(v0, t0, zero); A.JrRa(); A.Nop();

    A.Label("sw_table");
    for (const char* c : {"sw_0", "sw_1", "sw_2", "sw_3"}) A.W(A.labels.at(c));
    A.Label("gte_buf"); A.W(0); A.W(0x00400010u);
    A.Resolve();

    Program p;
    p.words = A.words;
    for (const std::string& n : names) {
        p.fn[n] = A.labels.at(n);
        if (n != "unlisted") p.entries.push_back(A.labels.at(n));
    }
    return p;
}

} // namespace g2
