// CPU conformance (roadmap Phase 5 / gate G0): table-driven R3000A checks.
// Expected values come from psx-spx (nocash), sections named per group below.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cstdint>
#include <cstdio>
#include <vector>
#include "cpu/cpu_context.h"
#include "loader/ps1_bios_hle.h"

using personarm::CpuContext;
using personarm::CpuRegisters;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_EQ(name, got, want)                                                          \
    do {                                                                                   \
        ++g_checks;                                                                        \
        uint32_t g_ = (got), w_ = (want);                                                  \
        if (g_ != w_) {                                                                    \
            ++g_failures;                                                                  \
            std::printf("  FAIL %s: %s = 0x%08X, want 0x%08X\n", name, #got, g_, w_);      \
        }                                                                                  \
    } while (0)

// Instruction encoders.
static uint32_t R(uint32_t funct, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh = 0) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
static uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF);
}
static uint32_t COP(uint32_t cop, uint32_t sub, uint32_t rt, uint32_t rd) {
    return ((0x10 | cop) << 26) | (sub << 21) | (rt << 16) | (rd << 11);
}
static const uint32_t kNop = 0;
static const uint32_t kSyscall = 0x0000000C;
static const uint32_t kBreak = 0x0000000D;
static const uint32_t kRfe = 0x42000010;
static const uint32_t kMfc0 = 0x00, kMtc0 = 0x04, kMfc2 = 0x00, kCfc2 = 0x02;

// 64 KiB little-endian RAM mirrored over the whole address space (addr & 0xFFFF),
// so 0x80000080 and 0xBFC00180 land at 0x0080 / 0x0180.
struct Machine {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000, 0);
    CpuContext cpu;

    Machine() {
        cpu.SetMemoryHooks(
            [this](uint32_t a, uint32_t n) {
                uint32_t v = 0;
                for (uint32_t i = 0; i < n; ++i) v |= uint32_t(mem[(a + i) & 0xFFFF]) << (8 * i);
                return v;
            },
            [this](uint32_t a, uint32_t v, uint32_t n) {
                for (uint32_t i = 0; i < n; ++i) mem[(a + i) & 0xFFFF] = uint8_t(v >> (8 * i));
            });
    }
    void W32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) mem[(a + i) & 0xFFFF] = uint8_t(v >> (8 * i)); }
    uint32_t R32(uint32_t a) const {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(mem[(a + i) & 0xFFFF]) << (8 * i);
        return v;
    }
    void Code(uint32_t a, std::initializer_list<uint32_t> words) { for (uint32_t w : words) { W32(a, w); a += 4; } }
    void SetSR(uint32_t sr) { CpuRegisters r = cpu.GetRegisters(); r.cp0_status = sr; cpu.SetRegisters(r); }
    const CpuRegisters& Regs() const { return cpu.GetRegisters(); }
    uint32_t ExcCode() const { return (Regs().cp0_cause >> 2) & 0x1F; }
    void Run(uint32_t pc, int steps) { cpu.SetPC(pc); for (int i = 0; i < steps; ++i) cpu.Step(); }
};

static const uint32_t kVecBev0 = 0x80000080, kVecBev1 = 0xBFC00180;
static const uint32_t kExcAdEL = 4, kExcAdES = 5, kExcSys = 8, kExcBp = 9, kExcOv = 12;

// ── 1. Arithmetic overflow ────────────────────────────────────────────────────
// psx-spx "CPU Arithmetic Opcodes": add/addi/sub raise an exception on signed
// overflow (ExcCode 0Ch Ov, "COP0 - Exception Handling") and leave rd untouched;
// addu/addiu/subu never trap and wrap modulo 2^32.
static void TestOverflow() {
    struct Case { const char* name; uint32_t op; uint32_t a, b; bool trap; uint32_t result; };
    const uint32_t ADD = R(0x20, 1, 2, 3), ADDU = R(0x21, 1, 2, 3);
    const uint32_t SUB = R(0x22, 1, 2, 3), SUBU = R(0x23, 1, 2, 3);
    const Case cases[] = {
        {"ADD max+1",        ADD,  0x7FFFFFFF, 1,          true,  0},
        {"ADD min+(-1)",     ADD,  0x80000000, 0xFFFFFFFF, true,  0},
        {"ADD -1+1",         ADD,  0xFFFFFFFF, 1,          false, 0},
        {"ADDU max+1",       ADDU, 0x7FFFFFFF, 1,          false, 0x80000000},
        {"SUB min-1",        SUB,  0x80000000, 1,          true,  0},
        {"SUB max-(-1)",     SUB,  0x7FFFFFFF, 0xFFFFFFFF, true,  0},
        {"SUB 5-7",          SUB,  5,          7,          false, 0xFFFFFFFE},
        {"SUBU min-1",       SUBU, 0x80000000, 1,          false, 0x7FFFFFFF},
        {"ADDI max+1",       I(0x08, 1, 3, 1),      0x7FFFFFFF, 0, true,  0},
        {"ADDI min+(-1)",    I(0x08, 1, 3, 0xFFFF), 0x80000000, 0, true,  0},
        {"ADDI 5+(-1)",      I(0x08, 1, 3, 0xFFFF), 5,          0, false, 4},
        {"ADDIU max+1",      I(0x09, 1, 3, 1),      0x7FFFFFFF, 0, false, 0x80000000},
    };
    for (const Case& c : cases) {
        Machine m;
        m.Code(0x1000, {c.op});
        m.cpu.SetGPR(1, c.a);
        m.cpu.SetGPR(2, c.b);
        m.cpu.SetGPR(3, 0xDEADBEEF);
        m.Run(0x1000, 1);
        if (c.trap) {
            CHECK_EQ(c.name, m.cpu.GetGPR(3), 0xDEADBEEF);
            CHECK_EQ(c.name, m.ExcCode(), kExcOv);
            CHECK_EQ(c.name, m.Regs().cp0_epc, 0x1000);
            CHECK_EQ(c.name, m.cpu.GetPC(), kVecBev0);
        } else {
            CHECK_EQ(c.name, m.cpu.GetGPR(3), c.result);
            CHECK_EQ(c.name, m.cpu.GetPC(), 0x1004);
        }
    }
}

// ── 2. Exceptions ─────────────────────────────────────────────────────────────
// psx-spx "COP0 - Exception Handling": EPC = faulting opcode, or the branch when
// the fault is in its delay slot (then CAUSE.BD, bit 31, is set); CAUSE bits 2-6
// = ExcCode (04h AdEL, 05h AdES, 08h Syscall, 09h BP); SR bits 0-5 are pushed
// (SR = SR AND NOT 3Fh OR (SR SHL 2) AND 3Fh); vector 80000080h (BEV=0) or
// BFC00180h (BEV=1, SR bit 22). "COP0 - Exception Handling / rfe": bits 5-2
// are copied to 3-0, bits 5-4 unchanged. BadVaddr is COP0 r8.
static void TestExceptions() {
    struct Case { const char* name; uint32_t op; uint32_t sr; uint32_t exc; uint32_t vec; uint32_t sr_after; };
    const Case cases[] = {
        {"SYSCALL BEV0", kSyscall, 0x0000FF0D, kExcSys, kVecBev0, 0x0000FF34},
        {"BREAK BEV0",   kBreak,   0x0000FF0D, kExcBp,  kVecBev0, 0x0000FF34},
        {"SYSCALL BEV1", kSyscall, 0x00400001, kExcSys, kVecBev1, 0x00400004},
    };
    for (const Case& c : cases) {
        Machine m;
        m.Code(0x1000, {c.op});
        m.SetSR(c.sr);
        m.Run(0x1000, 1);
        CHECK_EQ(c.name, m.ExcCode(), c.exc);
        CHECK_EQ(c.name, m.Regs().cp0_epc, 0x1000);
        CHECK_EQ(c.name, m.Regs().cp0_cause >> 31, 0);
        CHECK_EQ(c.name, m.cpu.GetPC(), c.vec);
        CHECK_EQ(c.name, m.Regs().cp0_status, c.sr_after);
    }

    // SYSCALL in a taken branch's delay slot: EPC = branch, BD set, vector wins over target.
    {
        Machine m;
        m.Code(0x1000, {I(0x04, 0, 0, 4), kSyscall}); // beq r0,r0,+4 ; syscall
        m.Code(0x0080, {kRfe});                        // handler: rfe
        m.SetSR(0x0000FF0D);
        m.Run(0x1000, 2);
        CHECK_EQ("SYSCALL in delay slot", m.ExcCode(), kExcSys);
        CHECK_EQ("SYSCALL in delay slot", m.Regs().cp0_epc, 0x1000);
        CHECK_EQ("SYSCALL in delay slot", m.Regs().cp0_cause >> 31, 1);
        CHECK_EQ("SYSCALL in delay slot", m.cpu.GetPC(), kVecBev0);
        m.cpu.Step(); // rfe at the vector
        CHECK_EQ("RFE pop", m.cpu.GetPC(), kVecBev0 + 4);
        CHECK_EQ("RFE pop", m.Regs().cp0_status, 0x0000FF3D);
    }

    // BIOS HLE (no kernel in RAM) at vector 0x80: resume after the SYSCALL with SR popped.
    {
        Machine m;
        m.Code(0x1000, {kSyscall});
        m.SetSR(0x0000FF0D);
        m.Run(0x1000, 1);
        bool handled = personarm::Ps1BiosHle::Instance().HandleBiosHook(m.cpu, m.mem.data(), m.mem.size());
        CHECK_EQ("HLE syscall return", handled, 1);
        CHECK_EQ("HLE syscall return", m.cpu.GetPC(), 0x1004);
        CHECK_EQ("HLE syscall return", m.Regs().cp0_status & 0x0F, 0x0D);
    }

    // Misaligned loads/stores: AdEL/AdES, BadVaddr = address, no register/memory write.
    struct AdCase { const char* name; uint32_t op; uint32_t exc; };
    const AdCase ad[] = {
        {"LW  0x2001", I(0x23, 0, 1, 0x2001), kExcAdEL},
        {"LW  0x2002", I(0x23, 0, 1, 0x2002), kExcAdEL},
        {"LH  0x2001", I(0x21, 0, 1, 0x2001), kExcAdEL},
        {"LHU 0x2003", I(0x25, 0, 1, 0x2003), kExcAdEL},
        {"SW  0x2002", I(0x2B, 0, 1, 0x2002), kExcAdES},
        {"SH  0x2001", I(0x29, 0, 1, 0x2001), kExcAdES},
    };
    for (const AdCase& c : ad) {
        Machine m;
        m.Code(0x1000, {c.op});
        m.Code(0x0080, {COP(0, kMfc0, 2, 8), kNop}); // handler: mfc0 r2,BadVaddr ; nop
        m.cpu.SetGPR(1, 0xCAFEBABE);
        m.Run(0x1000, 1);
        uint32_t addr = c.op & 0xFFFF;
        CHECK_EQ(c.name, m.ExcCode(), c.exc);
        CHECK_EQ(c.name, m.Regs().cp0_epc, 0x1000);
        CHECK_EQ(c.name, m.cpu.GetPC(), kVecBev0);
        m.cpu.Step();
        m.cpu.Step();
        CHECK_EQ(c.name, m.cpu.GetGPR(2), addr);
        CHECK_EQ(c.name, m.cpu.GetGPR(1), 0xCAFEBABE);
        CHECK_EQ(c.name, m.R32(0x2000), 0);
    }
}

// ── 3. Branches and jumps ─────────────────────────────────────────────────────
// psx-spx "CPU Jump Opcodes": bltz/bgez/bltzal/bgezal compare rs with 0;
// the ...al forms set ra = $+8 even when the branch is not taken. jal/jalr
// link $+8; jr/jalr jump to rs. All have one delay slot.
static void TestBranches() {
    struct Case { const char* name; uint32_t op; uint32_t r1; uint32_t pc_after; uint32_t link_reg; uint32_t link; };
    const uint32_t kTaken = 0x1004 + (4 << 2), kNot = 0x1008, kUntouched = 0xCAFEF00D;
    const Case cases[] = {
        {"BLTZ -1 taken",       I(0x01, 1, 0x00, 4), 0xFFFFFFFF, kTaken, 31, kUntouched},
        {"BLTZ 0 not taken",    I(0x01, 1, 0x00, 4), 0,          kNot,   31, kUntouched},
        {"BGEZ 0 taken",        I(0x01, 1, 0x01, 4), 0,          kTaken, 31, kUntouched},
        {"BGEZ -1 not taken",   I(0x01, 1, 0x01, 4), 0xFFFFFFFF, kNot,   31, kUntouched},
        {"BLTZAL -1 taken",     I(0x01, 1, 0x10, 4), 0xFFFFFFFF, kTaken, 31, 0x1008},
        {"BLTZAL 1 not taken",  I(0x01, 1, 0x10, 4), 1,          kNot,   31, 0x1008},
        {"BGEZAL 0 taken",      I(0x01, 1, 0x11, 4), 0,          kTaken, 31, 0x1008},
        {"BGEZAL -1 not taken", I(0x01, 1, 0x11, 4), 0xFFFFFFFF, kNot,   31, 0x1008},
        {"JAL",                 (0x03u << 26) | (0x3000 >> 2), 0, 0x3000, 31, 0x1008},
        {"JR r1",               R(0x08, 1, 0, 0),    0x4000,     0x4000, 31, kUntouched},
        {"JALR r4,r1",          R(0x09, 1, 0, 4),    0x5000,     0x5000, 4,  0x1008},
    };
    for (const Case& c : cases) {
        Machine m;
        m.Code(0x1000, {c.op, kNop});
        m.cpu.SetGPR(1, c.r1);
        m.cpu.SetGPR(31, kUntouched);
        m.cpu.SetGPR(4, kUntouched);
        m.Run(0x1000, 2); // branch + delay slot
        CHECK_EQ(c.name, m.cpu.GetPC(), c.pc_after);
        CHECK_EQ(c.name, m.cpu.GetGPR(c.link_reg), c.link);
    }
}

// ── 4. Unaligned loads/stores ────────────────────────────────────────────────
// psx-spx "CPU Load/Store Opcodes" (little-endian tables):
//   LWL n=0..3: (rt AND 00FFFFFFh SHR 8n) OR (mem SHL (24-8n))
//   LWR n=0..3: (rt AND (FFFFFF00h SHL (24-8n)) [none for n=0]) OR (mem SHR 8n)
//   SWL n=0..3: (mem AND (FFFFFF00h SHL 8n)) OR (rt SHR (24-8n))
//   SWR n=0..3: (mem AND (00FFFFFFh SHR (24-8n)) [none for n=0]) OR (rt SHL 8n)
// LWL/LWR to the same register may be chained without a delay nop.
static void TestUnaligned() {
    const uint32_t kMem = 0x44332211, kRt = 0xAABBCCDD;
    struct Case { const char* name; uint32_t op; uint32_t n; uint32_t want; };
    const Case cases[] = {
        {"LWL +0", 0x22, 0, 0x11BBCCDD}, {"LWL +1", 0x22, 1, 0x2211CCDD},
        {"LWL +2", 0x22, 2, 0x332211DD}, {"LWL +3", 0x22, 3, 0x44332211},
        {"LWR +0", 0x26, 0, 0x44332211}, {"LWR +1", 0x26, 1, 0xAA443322},
        {"LWR +2", 0x26, 2, 0xAABB4433}, {"LWR +3", 0x26, 3, 0xAABBCC44},
        {"SWL +0", 0x2A, 0, 0x443322AA}, {"SWL +1", 0x2A, 1, 0x4433AABB},
        {"SWL +2", 0x2A, 2, 0x44AABBCC}, {"SWL +3", 0x2A, 3, 0xAABBCCDD},
        {"SWR +0", 0x2E, 0, 0xAABBCCDD}, {"SWR +1", 0x2E, 1, 0xBBCCDD11},
        {"SWR +2", 0x2E, 2, 0xCCDD2211}, {"SWR +3", 0x2E, 3, 0xDD332211},
    };
    for (const Case& c : cases) {
        Machine m;
        m.W32(0x2000, kMem);
        m.Code(0x1000, {I(c.op, 0, 1, 0x2000 + c.n), kNop});
        m.cpu.SetGPR(1, kRt);
        m.Run(0x1000, 2);
        bool is_store = (c.op & 0x08) != 0;
        CHECK_EQ(c.name, is_store ? m.R32(0x2000) : m.cpu.GetGPR(1), c.want);
    }

    // lwr r1,0x2001 ; lwl r1,0x2004 (no nop between) reads the unaligned word at 0x2001.
    Machine m;
    m.W32(0x2000, 0x44332211);
    m.W32(0x2004, 0x88776655);
    m.Code(0x1000, {I(0x26, 0, 1, 0x2001), I(0x22, 0, 1, 0x2004), kNop});
    m.cpu.SetGPR(1, kRt);
    m.Run(0x1000, 3);
    CHECK_EQ("LWR+LWL pair", m.cpu.GetGPR(1), 0x55443322);
}

// ── 5. Coprocessor-read load delay ──────────────────────────────────────────
// psx-spx "CPU Load/Store Opcodes" (Load Delay) and HW-003: mfc0/mfc2/cfc2
// results are, like memory loads, not visible to the next opcode.
static void TestCopLoadDelay() {
    struct Case { const char* name; uint32_t op; };
    const Case cases[] = {
        {"MFC0 SR delay", COP(0, kMfc0, 1, 12)},
        {"MFC2 r0 delay", COP(2, kMfc2, 1, 0)},
        {"CFC2 r0 delay", COP(2, kCfc2, 1, 0)},
    };
    const uint32_t kNew = 0x00001234;
    for (const Case& c : cases) {
        Machine m;
        m.SetSR(kNew);
        m.cpu.GetGTE().WriteDataRegister(0, kNew);
        m.cpu.GetGTE().WriteControlRegister(0, kNew);
        m.Code(0x1000, {c.op, R(0x21, 1, 0, 3), kNop}); // cop read r1 ; addu r3,r1,r0
        m.cpu.SetGPR(1, 5);
        m.Run(0x1000, 3);
        CHECK_EQ(c.name, m.cpu.GetGPR(3), 5);
        CHECK_EQ(c.name, m.cpu.GetGPR(1), kNew);
    }
}

// ── 6. MULT/MULTU, SLT*, variable shifts ────────────────────────────────────
// psx-spx "CPU Arithmetic Opcodes" / "CPU Logical/Shift": mult is signed and
// multu unsigned 64-bit into hi:lo; slti/sltiu sign-extend the immediate (sltiu
// then compares unsigned); sllv/srlv/srav use only the low 5 bits of rs.
static void TestAluMisc() {
    struct MulCase { const char* name; uint32_t funct; uint32_t a, b; uint32_t hi, lo; };
    const MulCase muls[] = {
        {"MULT -1*2",      0x18, 0xFFFFFFFF, 2,          0xFFFFFFFF, 0xFFFFFFFE},
        {"MULT -1*-1",     0x18, 0xFFFFFFFF, 0xFFFFFFFF, 0,          1},
        {"MULT min*min",   0x18, 0x80000000, 0x80000000, 0x40000000, 0},
        {"MULTU ~0*2",     0x19, 0xFFFFFFFF, 2,          1,          0xFFFFFFFE},
        {"MULTU ~0*~0",    0x19, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 1},
    };
    for (const MulCase& c : muls) {
        Machine m;
        m.Code(0x1000, {R(c.funct, 1, 2, 0)});
        m.cpu.SetGPR(1, c.a);
        m.cpu.SetGPR(2, c.b);
        m.Run(0x1000, 1);
        CHECK_EQ(c.name, m.cpu.GetHI(), c.hi);
        CHECK_EQ(c.name, m.cpu.GetLO(), c.lo);
    }

    struct AluCase { const char* name; uint32_t op; uint32_t r1, r2; uint32_t want; };
    const AluCase alu[] = {
        {"SLT -1<1",              R(0x2A, 1, 2, 3), 0xFFFFFFFF, 1, 1},
        {"SLTU ~0<1",             R(0x2B, 1, 2, 3), 0xFFFFFFFF, 1, 0},
        {"SLTI -2<-1",            I(0x0A, 1, 3, 0xFFFF), 0xFFFFFFFE, 0, 1},
        {"SLTI 0<-32768",         I(0x0A, 1, 3, 0x8000), 0, 0, 0},
        {"SLTIU 5<0xFFFFFFFF",    I(0x0B, 1, 3, 0xFFFF), 5, 0, 1},
        {"SLTIU ~0<0xFFFFFFFF",   I(0x0B, 1, 3, 0xFFFF), 0xFFFFFFFF, 0, 0},
        {"SLTIU 0x7FFF<0x8000s",  I(0x0B, 1, 3, 0x8000), 0x7FFF, 0, 1},
        {"SLLV by 33",            R(0x04, 1, 2, 3), 33, 1, 2},
        {"SRLV by 0x3F",          R(0x06, 1, 2, 3), 0x3F, 0x80000000, 1},
        {"SRAV by 0x24",          R(0x07, 1, 2, 3), 0x24, 0x80000000, 0xF8000000},
    };
    for (const AluCase& c : alu) {
        Machine m;
        m.Code(0x1000, {c.op});
        m.cpu.SetGPR(1, c.r1);
        m.cpu.SetGPR(2, c.r2);
        m.Run(0x1000, 1);
        CHECK_EQ(c.name, m.cpu.GetGPR(3), c.want);
    }
}

int main() {
    std::printf("[UNIT TEST] CPU conformance (R3000A, psx-spx)\n");
    TestOverflow();
    TestExceptions();
    TestBranches();
    TestUnaligned();
    TestCopLoadDelay();
    TestAluMisc();
    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
