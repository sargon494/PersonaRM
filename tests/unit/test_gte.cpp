// GTE (COP2) unit tests. Expected values are hand-computed from the formulas in
// psx-spx "Geometry Transformation Engine (GTE)" (Registers, Saturation,
// Coordinate/General Purpose/Color Calculation Commands, Division Inaccuracy).
#include "cpu/gte.h"

#include <cstdio>
#include <cstdint>

using personarm::Gte;

static int g_failures = 0;

#define CHECK_EQ(actual, expected)                                                            \
    do {                                                                                      \
        const uint64_t a_ = static_cast<uint64_t>(static_cast<uint32_t>(actual));             \
        const uint64_t e_ = static_cast<uint64_t>(static_cast<uint32_t>(expected));           \
        if (a_ != e_) {                                                                       \
            std::printf("FAIL %s:%d: %s = 0x%08llX, expected 0x%08llX\n", __FILE__, __LINE__, \
                        #actual, (unsigned long long)a_, (unsigned long long)e_);             \
            ++g_failures;                                                                     \
        }                                                                                     \
    } while (0)

namespace {

constexpr uint32_t kCop2 = 0x4A000000u;
constexpr uint32_t SF = 1u << 19;
constexpr uint32_t LM = 1u << 10;
constexpr uint32_t FLAG_ERR = 0x80000000u;

uint32_t XY(int x, int y) { return (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) | static_cast<uint16_t>(x); }
uint32_t Mvmva(uint32_t mx, uint32_t v, uint32_t cv) { return kCop2 | 0x12 | (mx << 17) | (v << 15) | (cv << 13); }

// Diagonal matrix d at ctrl base (RT=0, LLM=8, LCM=16)
void SetDiag(Gte& g, uint32_t base, int16_t d) {
    g.WriteControlRegister(base + 0, static_cast<uint16_t>(d));
    g.WriteControlRegister(base + 1, 0);
    g.WriteControlRegister(base + 2, static_cast<uint16_t>(d));
    g.WriteControlRegister(base + 3, 0);
    g.WriteControlRegister(base + 4, static_cast<uint16_t>(d));
}
void SetIr(Gte& g, int a, int b, int c) {
    g.WriteDataRegister(9, a); g.WriteDataRegister(10, b); g.WriteDataRegister(11, c);
}

void TestRegisters() {
    Gte g;
    g.WriteDataRegister(1, 0x1234FFFF);  CHECK_EQ(g.ReadDataRegister(1), 0xFFFFFFFF); // VZ0 sign-extends
    g.WriteDataRegister(9, 0x00008000);  CHECK_EQ(g.ReadDataRegister(9), 0xFFFF8000); // IR1 sign-extends
    g.WriteDataRegister(7, 0xFFFF8000);  CHECK_EQ(g.ReadDataRegister(7), 0x8000);     // OTZ zero-extends
    g.WriteDataRegister(19, 0x12345678); CHECK_EQ(g.ReadDataRegister(19), 0x5678);    // SZ3 zero-extends

    // SXYP write pushes the FIFO; SXYP reads as SXY2
    g.WriteDataRegister(15, XY(1, 2));
    g.WriteDataRegister(15, XY(3, 4));
    g.WriteDataRegister(15, XY(5, 6));
    CHECK_EQ(g.ReadDataRegister(12), XY(1, 2));
    CHECK_EQ(g.ReadDataRegister(13), XY(3, 4));
    CHECK_EQ(g.ReadDataRegister(14), XY(5, 6));
    CHECK_EQ(g.ReadDataRegister(15), XY(5, 6));

    // IRGB -> IR1..3 = component*80h; ORGB = IR/80h saturated to 0..1Fh
    g.WriteDataRegister(28, (31u << 10) | (2u << 5) | 1u);
    CHECK_EQ(g.ReadDataRegister(9), 0x80);
    CHECK_EQ(g.ReadDataRegister(10), 0x100);
    CHECK_EQ(g.ReadDataRegister(11), 0xF80);
    CHECK_EQ(g.ReadDataRegister(29), (31u << 10) | (2u << 5) | 1u);
    SetIr(g, -5, 0x7FFF, 0x80);
    CHECK_EQ(g.ReadDataRegister(29), (1u << 10) | (0x1Fu << 5) | 0u);

    // LZCS/LZCR
    g.WriteDataRegister(30, 0);           CHECK_EQ(g.ReadDataRegister(31), 32);
    g.WriteDataRegister(30, 0xFFFFFFFF);  CHECK_EQ(g.ReadDataRegister(31), 32);
    g.WriteDataRegister(30, 0x0000FFFF);  CHECK_EQ(g.ReadDataRegister(31), 16);
    g.WriteDataRegister(30, 0xFFFF0000);  CHECK_EQ(g.ReadDataRegister(31), 16);
    g.WriteDataRegister(30, 0x40000000);  CHECK_EQ(g.ReadDataRegister(31), 1);
    g.WriteDataRegister(31, 7);           CHECK_EQ(g.ReadDataRegister(31), 1); // read-only

    // Control: H reads sign-extended (hardware quirk), R33/ZSF3 sign-extend, FLAG mask
    g.WriteControlRegister(26, 0x8000); CHECK_EQ(g.ReadControlRegister(26), 0xFFFF8000);
    g.WriteControlRegister(4, 0xFFFF);  CHECK_EQ(g.ReadControlRegister(4), 0xFFFFFFFF);
    g.WriteControlRegister(29, 0x1FFFF); CHECK_EQ(g.ReadControlRegister(29), 0xFFFFFFFF);
    g.WriteControlRegister(31, 0xFFFFFFFF); CHECK_EQ(g.ReadControlRegister(31), 0xFFFFF000);
    g.WriteControlRegister(31, 1u << 12);   CHECK_EQ(g.ReadControlRegister(31), 1u << 12); // bit12 not in summary
    g.WriteControlRegister(31, 1u << 13);   CHECK_EQ(g.ReadControlRegister(31), FLAG_ERR | (1u << 13));
}

void TestUnr() {
    CHECK_EQ(Gte::UnrTableEntry(0x000), 0xFF);
    CHECK_EQ(Gte::UnrTableEntry(0x001), 0xFD);
    CHECK_EQ(Gte::UnrTableEntry(0x002), 0xFB);
    CHECK_EQ(Gte::UnrTableEntry(0x0F4), 0x05);
    CHECK_EQ(Gte::UnrTableEntry(0x0FF), 0x00);
    CHECK_EQ(Gte::UnrTableEntry(0x100), 0x00);

    Gte g;
    CHECK_EQ(g.Divide(1000, 1000), 0x10000);
    CHECK_EQ(g.Divide(1000, 2000), 0x8000);
    CHECK_EQ(g.Divide(1000, 501), 0x1FEFB);   // ideal 130810.38 -> 130811
    CHECK_EQ(g.Divide(0xFFFF, 0x8000), 0x1FFFE);
    CHECK_EQ(g.Divide(1, 0xFFFF), 1);
    CHECK_EQ(g.Divide(0x155, 0x100), 0x15500);
    CHECK_EQ(g.GetFlag(), 0);
    CHECK_EQ(g.Divide(1000, 500), 0x1FFFF);   // H >= SZ3*2 -> overflow
    CHECK_EQ(g.GetFlag() & (1u << 17), 1u << 17);
}

void TestRtps() {
    Gte g;
    SetDiag(g, 0, 0x1000);
    g.WriteControlRegister(26, 1000);       // H (ctrl r26)
    g.WriteControlRegister(24, 160 << 16);  // OFX
    g.WriteControlRegister(25, 120 << 16);  // OFY
    g.WriteControlRegister(27, 0x100);      // DQA
    g.WriteControlRegister(28, 0x100000);   // DQB (ctrl r28, must NOT be used as H)
    g.WriteDataRegister(0, XY(100, 50));
    g.WriteDataRegister(1, 1000);
    g.ExecuteCommand(kCop2 | SF | 0x01);
    CHECK_EQ(g.ReadDataRegister(25), 100);
    CHECK_EQ(g.ReadDataRegister(26), 50);
    CHECK_EQ(g.ReadDataRegister(27), 1000);
    CHECK_EQ(g.ReadDataRegister(9), 100);
    CHECK_EQ(g.ReadDataRegister(11), 1000);
    CHECK_EQ(g.ReadDataRegister(19), 1000);          // SZ3
    CHECK_EQ(g.ReadDataRegister(14), XY(260, 170));  // SXY2 = proj + OF
    // MAC0 = 10000h*100h + 100000h; IR0 = MAC0>>12 = 1100h -> saturated 1000h (FLAG.12, no summary)
    CHECK_EQ(g.ReadDataRegister(24), 0x1100000);
    CHECK_EQ(g.ReadDataRegister(8), 0x1000);
    CHECK_EQ(g.GetFlag(), 1u << 12);

    // SX2 saturation (FLAG.14) and divide overflow (FLAG.17)
    g.WriteControlRegister(24, 0);
    g.WriteControlRegister(25, 0);
    g.WriteControlRegister(27, 0);
    g.WriteControlRegister(28, 0);
    g.WriteDataRegister(0, XY(2000, 0));
    g.WriteDataRegister(1, 1000);
    g.ExecuteCommand(kCop2 | SF | 0x01);
    CHECK_EQ(g.ReadDataRegister(14), XY(0x3FF, 0));
    CHECK_EQ(g.GetFlag(), FLAG_ERR | (1u << 14));
    g.WriteDataRegister(0, XY(1, 1));
    g.WriteDataRegister(1, 100);
    g.ExecuteCommand(kCop2 | SF | 0x01);
    CHECK_EQ(g.GetFlag(), FLAG_ERR | (1u << 17));
    CHECK_EQ(g.ReadDataRegister(14), XY(1, 1)); // 1FFFFh*1 >> 16 = 1

    // sf=0: IR3 saturates on MAC3 but FLAG.22 follows MAC3>>12
    Gte h;
    SetDiag(h, 0, 0x1000);
    h.WriteDataRegister(1, 0x10);
    h.ExecuteCommand(kCop2 | 0x01);
    CHECK_EQ(h.ReadDataRegister(27), 0x10000);
    CHECK_EQ(h.ReadDataRegister(11), 0x7FFF);
    CHECK_EQ(h.ReadDataRegister(19), 0x10);
    CHECK_EQ(h.GetFlag() & (1u << 22), 0);
    h.WriteDataRegister(1, 0);
    h.WriteControlRegister(7, 0x8000); // TRZ
    h.ExecuteCommand(kCop2 | 0x01);
    CHECK_EQ(h.GetFlag() & (1u << 22), 1u << 22);
}

void TestRtpt() {
    Gte g;
    SetDiag(g, 0, 0x1000);
    g.WriteControlRegister(26, 1000);
    g.WriteDataRegister(0, XY(100, 50));  g.WriteDataRegister(1, 1000);
    g.WriteDataRegister(2, XY(-200, 0));  g.WriteDataRegister(3, 1000);
    g.WriteDataRegister(4, XY(0, 300));   g.WriteDataRegister(5, 2000);
    g.ExecuteCommand(kCop2 | SF | 0x30);
    CHECK_EQ(g.ReadDataRegister(12), XY(100, 50));
    CHECK_EQ(g.ReadDataRegister(13), XY(-200, 0));
    CHECK_EQ(g.ReadDataRegister(14), XY(0, 150));
    CHECK_EQ(g.ReadDataRegister(16), 0);
    CHECK_EQ(g.ReadDataRegister(17), 1000);
    CHECK_EQ(g.ReadDataRegister(18), 1000);
    CHECK_EQ(g.ReadDataRegister(19), 2000);
    CHECK_EQ(g.ReadDataRegister(27), 2000); // MAC3 from V2
    CHECK_EQ(g.GetFlag(), 0);
}

void TestNclipAvsz() {
    Gte g;
    g.WriteDataRegister(12, XY(0, 0));
    g.WriteDataRegister(13, XY(10, 0));
    g.WriteDataRegister(14, XY(0, 10));
    g.ExecuteCommand(kCop2 | 0x06);
    CHECK_EQ(g.ReadDataRegister(24), 100);
    g.WriteDataRegister(13, XY(0, 10));
    g.WriteDataRegister(14, XY(10, 0));
    g.ExecuteCommand(kCop2 | 0x06);
    CHECK_EQ(g.ReadDataRegister(24), static_cast<uint32_t>(-100));

    g.WriteControlRegister(29, 0x555);
    g.WriteDataRegister(17, 300); g.WriteDataRegister(18, 300); g.WriteDataRegister(19, 300);
    g.WriteDataRegister(20, 0xABCDEF); // RGB0 must stay untouched (old bug wrote OTZ here)
    g.ExecuteCommand(kCop2 | 0x2D);
    CHECK_EQ(g.ReadDataRegister(24), 1365 * 900);
    CHECK_EQ(g.ReadDataRegister(7), 299);
    CHECK_EQ(g.ReadDataRegister(20), 0xABCDEF);

    g.WriteControlRegister(30, 0x400);
    g.WriteDataRegister(16, 100); g.WriteDataRegister(17, 200);
    g.WriteDataRegister(18, 300); g.WriteDataRegister(19, 400);
    g.ExecuteCommand(kCop2 | 0x2E);
    CHECK_EQ(g.ReadDataRegister(7), 250);

    g.WriteControlRegister(29, static_cast<uint32_t>(-0x1000));
    g.ExecuteCommand(kCop2 | 0x2D);
    CHECK_EQ(g.ReadDataRegister(7), 0);
    CHECK_EQ(g.GetFlag(), FLAG_ERR | (1u << 18));
}

void TestMvmva() {
    Gte g;
    SetDiag(g, 0, 0x2000);
    g.WriteControlRegister(5, 10); g.WriteControlRegister(6, 20); g.WriteControlRegister(7, 30);
    g.WriteDataRegister(2, XY(1, 2)); g.WriteDataRegister(3, 3); // V1
    g.ExecuteCommand(Mvmva(0, 1, 0) | SF);
    CHECK_EQ(g.ReadDataRegister(25), 12);
    CHECK_EQ(g.ReadDataRegister(26), 24);
    CHECK_EQ(g.ReadDataRegister(27), 36);
    CHECK_EQ(g.ReadDataRegister(11), 36);

    // lm=1 clamps negative IR to 0 (FLAG.24)
    SetDiag(g, 0, 0x1000);
    g.WriteDataRegister(0, XY(-5, 0)); g.WriteDataRegister(1, 0);
    g.ExecuteCommand(Mvmva(0, 0, 3) | SF | LM);
    CHECK_EQ(g.ReadDataRegister(25), static_cast<uint32_t>(-5));
    CHECK_EQ(g.ReadDataRegister(9), 0);
    CHECK_EQ(g.GetFlag(), FLAG_ERR | (1u << 24));

    // cv=FC bug: result is only Mx2*Vy + Mx3*Vz
    g.WriteControlRegister(21, 100);
    g.WriteDataRegister(0, XY(1, 2)); g.WriteDataRegister(1, 3);
    g.ExecuteCommand(Mvmva(0, 0, 2) | SF);
    CHECK_EQ(g.ReadDataRegister(25), 0);
    CHECK_EQ(g.ReadDataRegister(26), 2);
    CHECK_EQ(g.ReadDataRegister(27), 3);

    // mx=3 garbage matrix: [-R*10h, R*10h, IR0; RT13 x3; RT22 x3], sf=0
    g.WriteDataRegister(6, 0x01);              // R = 1
    g.WriteControlRegister(1, 5);              // RT13 = 5 (RT21 = 0)
    g.WriteControlRegister(2, 7);              // RT22 = 7
    g.WriteDataRegister(0, XY(2, 1)); g.WriteDataRegister(1, 0);
    g.ExecuteCommand(Mvmva(3, 0, 3));
    CHECK_EQ(g.ReadDataRegister(25), static_cast<uint32_t>(-16));
    CHECK_EQ(g.ReadDataRegister(26), 15);
    CHECK_EQ(g.ReadDataRegister(27), 21);

    // MAC1 44-bit overflow, positive (FLAG.30) and negative (FLAG.27)
    Gte o;
    o.WriteControlRegister(0, 0x7FFF);
    o.WriteControlRegister(5, 0x7FFFFFFF);
    o.WriteDataRegister(0, XY(0x7FFF, 0));
    o.ExecuteCommand(Mvmva(0, 0, 0) | SF);
    CHECK_EQ(o.GetFlag() & ((1u << 30) | FLAG_ERR), (1u << 30) | FLAG_ERR);
    o.WriteControlRegister(5, 0x80000000);
    o.WriteDataRegister(0, XY(-0x8000, 0));
    o.ExecuteCommand(Mvmva(0, 0, 0) | SF);
    CHECK_EQ(o.GetFlag() & ((1u << 27) | FLAG_ERR), (1u << 27) | FLAG_ERR);
}

void TestOpSqrGp() {
    Gte g;
    SetDiag(g, 0, 0x1000);
    SetIr(g, 1, 2, 3);
    g.ExecuteCommand(kCop2 | SF | 0x0C); // OP
    CHECK_EQ(g.ReadDataRegister(25), 1);
    CHECK_EQ(g.ReadDataRegister(26), static_cast<uint32_t>(-2));
    CHECK_EQ(g.ReadDataRegister(27), 1);

    SetIr(g, 3, -4, 0x100);
    g.ExecuteCommand(kCop2 | 0x28); // SQR sf=0
    CHECK_EQ(g.ReadDataRegister(25), 9);
    CHECK_EQ(g.ReadDataRegister(26), 16);
    CHECK_EQ(g.ReadDataRegister(27), 0x10000);
    CHECK_EQ(g.ReadDataRegister(11), 0x7FFF);
    CHECK_EQ(g.GetFlag(), 1u << 22); // FLAG.31 ORs bits 30..23 and 18..13 only: IR3 (bit 22) is excluded

    g.WriteDataRegister(6, 0x30000000); // CODE
    g.WriteDataRegister(8, 0x800);
    SetIr(g, 0x1000, 0x2000, 0x100);
    g.ExecuteCommand(kCop2 | SF | 0x3D); // GPF
    CHECK_EQ(g.ReadDataRegister(25), 0x800);
    CHECK_EQ(g.ReadDataRegister(22), 0x3008FF80);
    CHECK_EQ(g.GetFlag(), 1u << 20); // color G saturation is not in the summary

    g.WriteDataRegister(8, 0x1000);
    SetIr(g, 0x10, 0, 0);
    g.ExecuteCommand(kCop2 | SF | 0x3E); // GPL: MAC1 = 800h + 10h
    CHECK_EQ(g.ReadDataRegister(25), 0x810);
}

// LLM = LCM = identity, BK = 0
Gte LightGte() {
    Gte g;
    SetDiag(g, 8, 0x1000);
    SetDiag(g, 16, 0x1000);
    g.WriteDataRegister(6, 0x55808080); // RGBC = (80h,80h,80h), CODE 55h
    return g;
}

void TestColor() {
    Gte g = LightGte();
    g.WriteDataRegister(0, XY(0x800, 0x400)); g.WriteDataRegister(1, 0x200);
    g.ExecuteCommand(kCop2 | SF | LM | 0x1E); // NCS
    CHECK_EQ(g.ReadDataRegister(22), 0x55204080);

    g.ExecuteCommand(kCop2 | SF | LM | 0x1B); // NCCS: R*IR1*16 >> 12 >> 4
    CHECK_EQ(g.ReadDataRegister(22), 0x55102040);

    g.WriteDataRegister(8, 0x1000);          // IR0 = 1.0 -> result = far color
    g.WriteControlRegister(21, 0x100); g.WriteControlRegister(22, 0x200); g.WriteControlRegister(23, 0x300);
    g.ExecuteCommand(kCop2 | SF | LM | 0x13); // NCDS
    CHECK_EQ(g.ReadDataRegister(25), 0x100);
    CHECK_EQ(g.ReadDataRegister(22), 0x55302010);

    Gte c = LightGte();
    SetIr(c, 0x800, 0x400, 0x200);
    c.ExecuteCommand(kCop2 | SF | LM | 0x1C); // CC
    CHECK_EQ(c.ReadDataRegister(22), 0x55102040);
    SetIr(c, 0x800, 0x400, 0x200);
    c.ExecuteCommand(kCop2 | SF | LM | 0x14); // CDP with IR0 = 0
    CHECK_EQ(c.ReadDataRegister(22), 0x55102040);
    SetIr(c, 0x800, 0x400, 0x200);
    c.ExecuteCommand(kCop2 | SF | LM | 0x29); // DCPL
    CHECK_EQ(c.ReadDataRegister(22), 0x55102040);

    Gte d;
    d.WriteDataRegister(6, 0x55302010);
    d.ExecuteCommand(kCop2 | SF | 0x10);      // DPCS, IR0 = 0
    CHECK_EQ(d.ReadDataRegister(22), 0x55302010);
    d.WriteDataRegister(8, 0x800);            // halfway to FC = 0
    d.ExecuteCommand(kCop2 | SF | 0x10);
    CHECK_EQ(d.ReadDataRegister(22), 0x55181008);

    d.WriteDataRegister(8, 0);
    d.WriteDataRegister(6, 0x77000000);
    d.WriteDataRegister(20, 0x01000010); d.WriteDataRegister(21, 0x02000020); d.WriteDataRegister(22, 0x03000030);
    d.ExecuteCommand(kCop2 | SF | 0x2A);      // DPCT on RGB0..2
    CHECK_EQ(d.ReadDataRegister(20), 0x77000010);
    CHECK_EQ(d.ReadDataRegister(21), 0x77000020);
    CHECK_EQ(d.ReadDataRegister(22), 0x77000030);

    SetIr(d, 0x100, 0x200, 0x300);
    d.ExecuteCommand(kCop2 | SF | 0x11);      // INTPL
    CHECK_EQ(d.ReadDataRegister(22), 0x77302010);

    // Triple variants use V0, V1, V2
    Gte t = LightGte();
    t.WriteDataRegister(0, XY(0x100, 0)); t.WriteDataRegister(1, 0);
    t.WriteDataRegister(2, XY(0, 0x200)); t.WriteDataRegister(3, 0);
    t.WriteDataRegister(4, XY(0, 0));     t.WriteDataRegister(5, 0x300);
    t.ExecuteCommand(kCop2 | SF | LM | 0x20); // NCT
    CHECK_EQ(t.ReadDataRegister(20), 0x55000010);
    CHECK_EQ(t.ReadDataRegister(21), 0x55002000);
    CHECK_EQ(t.ReadDataRegister(22), 0x55300000);
    t.ExecuteCommand(kCop2 | SF | LM | 0x3F); // NCCT: halves (R=80h)
    CHECK_EQ(t.ReadDataRegister(20), 0x55000008);
    CHECK_EQ(t.ReadDataRegister(21), 0x55001000);
    CHECK_EQ(t.ReadDataRegister(22), 0x55180000);
    t.ExecuteCommand(kCop2 | SF | LM | 0x16); // NCDT, IR0 = 0
    CHECK_EQ(t.ReadDataRegister(20), 0x55000008);
    CHECK_EQ(t.ReadDataRegister(22), 0x55180000);
    CHECK_EQ(t.GetFlag(), 0);
}

} // namespace

int main() {
    TestRegisters();
    TestUnr();
    TestRtps();
    TestRtpt();
    TestNclipAvsz();
    TestMvmva();
    TestOpSqrGp();
    TestColor();
    if (g_failures) {
        std::printf("%d GTE check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All GTE tests passed\n");
    return 0;
}
