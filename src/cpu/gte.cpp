#include "cpu/gte.h"
#include <algorithm>

namespace personarm {

namespace {

constexpr uint32_t kFlagErrorMask = 0x7F87E000u; // FLAG bits 30-23 and 18-13 feed bit 31

int64_t SignExtend44(int64_t v) {
    return static_cast<int64_t>(static_cast<uint64_t>(v) << 20) >> 20;
}

int CountLeadingZeros16(uint32_t v) {
    int n = 0;
    for (uint32_t bit = 0x8000; bit != 0 && !(v & bit); bit >>= 1) ++n;
    return n;
}

} // namespace

Gte::Gte() {
    Reset();
}

void Gte::Reset() {
    m_data.fill(0);
    m_ctrl.fill(0);
    m_data[31] = 32; // LZCR of LZCS=0 (psx-spx "GTE Registers": LZCS/LZCR)
}

// -----------------------------------------------------------------------------
// psx-spx "GTE Registers"
// -----------------------------------------------------------------------------
uint32_t Gte::ReadDataRegister(uint32_t reg) const {
    switch (reg) {
        case 15: // SXYP reads as SXY2
            return m_data[14];
        case 28: // IRGB reads back ORGB (from memory: Mednafen/psx-spx behaviour)
        case 29: { // ORGB = IR1..3 / 80h saturated to 0..1Fh
            auto c = [this](int i) { return static_cast<uint32_t>(std::clamp(IR(i) >> 7, 0, 0x1F)); };
            return c(1) | (c(2) << 5) | (c(3) << 10);
        }
        default:
            return reg < 32 ? m_data[reg] : 0;
    }
}

void Gte::WriteDataRegister(uint32_t reg, uint32_t value) {
    switch (reg) {
        case 1: case 3: case 5:            // VZ0..2 (s16, sign-extended on read)
        case 8: case 9: case 10: case 11:  // IR0..3 (s16)
            m_data[reg] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
            break;
        case 7: case 16: case 17: case 18: case 19: // OTZ, SZ0..3 (u16)
            m_data[reg] = value & 0xFFFF;
            break;
        case 15: // SXYP write pushes the screen XY FIFO
            m_data[12] = m_data[13];
            m_data[13] = m_data[14];
            m_data[14] = value;
            break;
        case 28: // IRGB: IRn = 5-bit component * 80h
            m_data[28] = value & 0x7FFF;
            m_data[9]  = (value & 0x1F) << 7;
            m_data[10] = ((value >> 5) & 0x1F) << 7;
            m_data[11] = ((value >> 10) & 0x1F) << 7;
            break;
        case 29: case 31: // ORGB, LZCR are read-only
            break;
        case 30: { // LZCS -> LZCR = count of leading bits equal to bit 31
            m_data[30] = value;
            uint32_t v = (value & 0x80000000u) ? ~value : value;
            uint32_t n = 0;
            for (uint32_t bit = 0x80000000u; bit != 0 && !(v & bit); bit >>= 1) ++n;
            m_data[31] = n;
            break;
        }
        default:
            if (reg < 32) m_data[reg] = value;
            break;
    }
}

uint32_t Gte::ReadControlRegister(uint32_t reg) const {
    return reg < 32 ? m_ctrl[reg] : 0;
}

void Gte::WriteControlRegister(uint32_t reg, uint32_t value) {
    switch (reg) {
        // RT33, L33, LB3, H, DQA, ZSF3, ZSF4 are 16-bit and read sign-extended
        // (H is unsigned but reads sign-expanded: hardware bug per psx-spx "GTE Registers").
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            m_ctrl[reg] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
            break;
        case 31: // FLAG: bits 12-30 writable, bit 31 is the error summary
            value &= 0x7FFFF000u;
            m_ctrl[31] = value | ((value & kFlagErrorMask) ? 0x80000000u : 0);
            break;
        default:
            if (reg < 32) m_ctrl[reg] = value;
            break;
    }
}

// -----------------------------------------------------------------------------
// Operand helpers
// -----------------------------------------------------------------------------
void Gte::Vec(int n, int64_t out[3]) const {
    if (n == 3) {
        for (int i = 0; i < 3; ++i) out[i] = IR(i + 1);
        return;
    }
    out[0] = Lo(m_data[n * 2]);
    out[1] = Hi(m_data[n * 2]);
    out[2] = Lo(m_data[n * 2 + 1]);
}

void Gte::Mat(uint32_t b, int64_t m[3][3]) const {
    m[0][0] = Lo(m_ctrl[b]);     m[0][1] = Hi(m_ctrl[b]);     m[0][2] = Lo(m_ctrl[b + 1]);
    m[1][0] = Hi(m_ctrl[b + 1]); m[1][1] = Lo(m_ctrl[b + 2]); m[1][2] = Hi(m_ctrl[b + 2]);
    m[2][0] = Lo(m_ctrl[b + 3]); m[2][1] = Hi(m_ctrl[b + 3]); m[2][2] = Lo(m_ctrl[b + 4]);
}

void Gte::CtrlVec(uint32_t b, int64_t out[3]) const {
    for (int i = 0; i < 3; ++i) out[i] = static_cast<int32_t>(m_ctrl[b + i]);
}

// -----------------------------------------------------------------------------
// psx-spx "GTE Saturation": MAC1..3 overflow at 44 bits (checked after every
// addition), IR1..3 to -8000h..7FFFh (0..7FFFh with lm=1), MAC0 at 32 bits,
// IR0 to 0..1000h, SZ3/OTZ to 0..FFFFh, SX2/SY2 to -400h..3FFh, colors 0..FFh.
// -----------------------------------------------------------------------------
int64_t Gte::CheckMac(int i, int64_t v) {
    if (v > (int64_t(1) << 43) - 1) m_ctrl[31] |= 1u << (31 - i);  // FLAG.30..28
    else if (v < -(int64_t(1) << 43)) m_ctrl[31] |= 1u << (28 - i); // FLAG.27..25
    return SignExtend44(v);
}

void Gte::SetMac(int i, int64_t v) {
    m_data[24 + i] = static_cast<uint32_t>(static_cast<int32_t>(CheckMac(i, v) >> m_shift));
}

void Gte::SetIr(int i, int64_t v, bool lm) {
    const int64_t lo = lm ? 0 : -0x8000;
    if (v < lo || v > 0x7FFF) {
        v = std::clamp<int64_t>(v, lo, 0x7FFF);
        m_ctrl[31] |= 1u << (25 - i); // FLAG.24..22
    }
    m_data[8 + i] = static_cast<uint32_t>(static_cast<int32_t>(v));
}

int64_t Gte::SetMac0(int64_t v) {
    if (v > INT32_MAX) m_ctrl[31] |= 1u << 16;
    else if (v < INT32_MIN) m_ctrl[31] |= 1u << 15;
    m_data[24] = static_cast<uint32_t>(v);
    return v;
}

void Gte::PushColor() {
    auto sat = [this](int32_t v, int flag_bit) -> uint32_t {
        if (v < 0 || v > 0xFF) { m_ctrl[31] |= 1u << flag_bit; v = std::clamp(v, 0, 0xFF); }
        return static_cast<uint32_t>(v);
    };
    const uint32_t r = sat(MAC(1) >> 4, 21), g = sat(MAC(2) >> 4, 20), b = sat(MAC(3) >> 4, 19);
    m_data[20] = m_data[21];
    m_data[21] = m_data[22];
    m_data[22] = r | (g << 8) | (b << 16) | (m_data[6] & 0xFF000000u); // CODE from RGBC
}

void Gte::PushSz(int64_t v) {
    if (v < 0 || v > 0xFFFF) { m_ctrl[31] |= 1u << 18; v = std::clamp<int64_t>(v, 0, 0xFFFF); }
    m_data[16] = m_data[17];
    m_data[17] = m_data[18];
    m_data[18] = m_data[19];
    m_data[19] = static_cast<uint32_t>(v);
}

void Gte::PushSxy(int64_t x, int64_t y) {
    if (x < -0x400 || x > 0x3FF) { m_ctrl[31] |= 1u << 14; x = std::clamp<int64_t>(x, -0x400, 0x3FF); }
    if (y < -0x400 || y > 0x3FF) { m_ctrl[31] |= 1u << 13; y = std::clamp<int64_t>(y, -0x400, 0x3FF); }
    m_data[12] = m_data[13];
    m_data[13] = m_data[14];
    m_data[14] = (static_cast<uint32_t>(y) << 16) | (static_cast<uint32_t>(x) & 0xFFFF);
}

// MAC = (T*1000h + M*V) SAR (sf*12); IR = MAC
void Gte::MulMatVec(const int64_t m[3][3], const int64_t v[3], const int64_t t[3], bool lm) {
    for (int i = 0; i < 3; ++i) {
        int64_t acc = CheckMac(i + 1, t[i] * 0x1000);
        acc = CheckMac(i + 1, acc + m[i][0] * v[0]);
        acc = CheckMac(i + 1, acc + m[i][1] * v[1]);
        SetMacIr(i + 1, acc + m[i][2] * v[2], lm);
    }
}

// psx-spx "GTE Color Calculation Commands", depth-cue interpolation:
//   [IR1,IR2,IR3] = (([RFC,GFC,BFC] SHL 12) - [MAC1,MAC2,MAC3]) SAR (sf*12)  (lm=0)
//   [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
// `in` holds the un-shifted MAC values entering the interpolation.
void Gte::Interpolate(const int64_t in[3]) {
    int64_t fc[3];
    CtrlVec(21, fc);
    for (int i = 0; i < 3; ++i) SetMacIr(i + 1, fc[i] * 0x1000 - in[i], false);
    const int64_t ir0 = IR(0);
    for (int i = 0; i < 3; ++i) SetMacIr(i + 1, IR(i + 1) * ir0 + in[i], m_lm);
}

// [MAC1,MAC2,MAC3] = [R*IR1, G*IR2, B*IR3] SHL 4 (un-shifted values)
void Gte::ColorTimesIr(int64_t out[3]) const {
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<int64_t>((m_data[6] >> (8 * i)) & 0xFF) * IR(i + 1) * 16;
}

// -----------------------------------------------------------------------------
// psx-spx "GTE Division Inaccuracy" (UNR division)
// -----------------------------------------------------------------------------
uint8_t Gte::UnrTableEntry(uint32_t i) {
    // unr_table[i] = max(0, (40000h/(i+100h)+1)/2 - 101h), i = 0..100h
    return static_cast<uint8_t>(std::max<int32_t>(0, (0x40000 / static_cast<int32_t>(i + 0x100) + 1) / 2 - 0x101));
}

uint32_t Gte::Divide(uint32_t h, uint32_t sz3) {
    static const auto table = [] {
        std::array<uint8_t, 0x101> t{};
        for (uint32_t i = 0; i < t.size(); ++i) t[i] = UnrTableEntry(i);
        return t;
    }();
    if (h >= sz3 * 2) {
        m_ctrl[31] |= 1u << 17; // divide overflow
        return 0x1FFFF;
    }
    const int z = CountLeadingZeros16(sz3);
    const uint64_t n = static_cast<uint64_t>(h) << z;
    uint64_t d = static_cast<uint64_t>(sz3) << z;
    const uint64_t u = table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    return static_cast<uint32_t>(std::min<uint64_t>(0x1FFFF, (n * d + 0x8000) >> 16));
}

// -----------------------------------------------------------------------------
// Command dispatch (psx-spx "GTE Command Encoding" / "GTE Command Summary")
// -----------------------------------------------------------------------------
void Gte::ExecuteCommand(uint32_t cmd) {
    m_shift = (cmd & (1u << 19)) ? 12 : 0;
    m_lm = (cmd & (1u << 10)) != 0;
    m_ctrl[31] = 0;

    switch (cmd & 0x3F) {
        case 0x01: Op_RTPS();  break;
        case 0x06: Op_NCLIP(); break;
        case 0x0C: Op_OP();    break;
        case 0x10: Op_DPCS();  break;
        case 0x11: Op_INTPL(); break;
        case 0x12: Op_MVMVA(cmd); break;
        case 0x13: Nc(0, 2);   break;                       // NCDS
        case 0x14: Op_CDP();   break;
        case 0x16: for (int n = 0; n < 3; ++n) Nc(n, 2); break; // NCDT
        case 0x1B: Nc(0, 1);   break;                       // NCCS
        case 0x1C: Op_CC();    break;
        case 0x1E: Nc(0, 0);   break;                       // NCS
        case 0x20: for (int n = 0; n < 3; ++n) Nc(n, 0); break; // NCT
        case 0x28: Op_SQR();   break;
        case 0x29: Op_DCPL();  break;
        case 0x2A: Op_DPCT();  break;
        case 0x2D: Op_AVSZ3(); break;
        case 0x2E: Op_AVSZ4(); break;
        case 0x30: Op_RTPT();  break;
        case 0x3D: Op_GPF();   break;
        case 0x3E: Op_GPL();   break;
        case 0x3F: for (int n = 0; n < 3; ++n) Nc(n, 1); break; // NCCT
        default:
            // shortcut: undocumented opcodes are treated as NOPs (hardware aliases some
            // of them to real commands); revisit if the game histogram shows any.
            break;
    }

    if (m_ctrl[31] & kFlagErrorMask) m_ctrl[31] |= 0x80000000u;
}

// -----------------------------------------------------------------------------
// psx-spx "GTE Coordinate Calculation Commands"
// -----------------------------------------------------------------------------
// RTPS/RTPT per vector:
//   IR1..3 = MAC1..3 = (TR*1000h + RT*Vn) SAR (sf*12)
//   SZ3 = MAC3 SAR ((1-sf)*12)
//   MAC0 = (H*20000h/SZ3+1)/2 * IR1 + OFX, SX2 = MAC0/10000h   (division = UNR)
//   MAC0 = ...                * IR2 + OFY, SY2 = MAC0/10000h
//   MAC0 = ...                * DQA + DQB, IR0 = MAC0/1000h   (RTPT: last vector only)
void Gte::Rtp(int n, bool last) {
    int64_t m[3][3], v[3], t[3];
    Mat(0, m);
    Vec(n, v);
    CtrlVec(5, t);
    for (int i = 0; i < 3; ++i) {
        int64_t acc = CheckMac(i + 1, t[i] * 0x1000);
        acc = CheckMac(i + 1, acc + m[i][0] * v[0]);
        acc = CheckMac(i + 1, acc + m[i][1] * v[1]);
        SetMac(i + 1, acc + m[i][2] * v[2]);
    }
    SetIr(1, MAC(1), m_lm);
    SetIr(2, MAC(2), m_lm);
    if (m_shift) {
        SetIr(3, MAC(3), m_lm);
    } else {
        // psx-spx "GTE Saturation": with sf=0, FLAG.22 is set from "MAC3 SAR 12"
        // although IR3 itself is saturated from MAC3.
        m_data[11] = static_cast<uint32_t>(std::clamp<int32_t>(MAC(3), m_lm ? 0 : -0x8000, 0x7FFF));
        const int32_t z = MAC(3) >> 12;
        if (z < -0x8000 || z > 0x7FFF) m_ctrl[31] |= 1u << 22;
    }
    PushSz(MAC(3) >> (12 - m_shift));

    const int64_t q = Divide(m_ctrl[26] & 0xFFFF, m_data[19]); // H is unsigned
    const int64_t sx = SetMac0(q * IR(1) + static_cast<int32_t>(m_ctrl[24]));
    const int64_t sy = SetMac0(q * IR(2) + static_cast<int32_t>(m_ctrl[25]));
    PushSxy(sx >> 16, sy >> 16);
    if (last) {
        const int64_t dq = SetMac0(q * static_cast<int16_t>(m_ctrl[27]) + static_cast<int32_t>(m_ctrl[28]));
        int64_t ir0 = dq >> 12;
        if (ir0 < 0 || ir0 > 0x1000) { m_ctrl[31] |= 1u << 12; ir0 = std::clamp<int64_t>(ir0, 0, 0x1000); }
        m_data[8] = static_cast<uint32_t>(ir0);
    }
}

void Gte::Op_RTPS() { Rtp(0, true); }

void Gte::Op_RTPT() {
    Rtp(0, false);
    Rtp(1, false);
    Rtp(2, true);
}

// MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
void Gte::Op_NCLIP() {
    const int64_t x0 = Lo(m_data[12]), y0 = Hi(m_data[12]);
    const int64_t x1 = Lo(m_data[13]), y1 = Hi(m_data[13]);
    const int64_t x2 = Lo(m_data[14]), y2 = Hi(m_data[14]);
    SetMac0(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
}

// MAC0 = ZSF3*(SZ1+SZ2+SZ3); OTZ = MAC0/1000h  (OTZ is data r7)
void Gte::Op_AVSZ3() {
    const int64_t v = SetMac0(int64_t(static_cast<int16_t>(m_ctrl[29])) * (m_data[17] + m_data[18] + m_data[19]));
    int64_t otz = v >> 12;
    if (otz < 0 || otz > 0xFFFF) { m_ctrl[31] |= 1u << 18; otz = std::clamp<int64_t>(otz, 0, 0xFFFF); }
    m_data[7] = static_cast<uint32_t>(otz);
}

// MAC0 = ZSF4*(SZ0+SZ1+SZ2+SZ3); OTZ = MAC0/1000h
void Gte::Op_AVSZ4() {
    const int64_t v = SetMac0(int64_t(static_cast<int16_t>(m_ctrl[30])) *
                              (m_data[16] + m_data[17] + m_data[18] + m_data[19]));
    int64_t otz = v >> 12;
    if (otz < 0 || otz > 0xFFFF) { m_ctrl[31] |= 1u << 18; otz = std::clamp<int64_t>(otz, 0, 0xFFFF); }
    m_data[7] = static_cast<uint32_t>(otz);
}

// -----------------------------------------------------------------------------
// psx-spx "GTE General Purpose Calculation Commands"
// -----------------------------------------------------------------------------
// MVMVA: MAC = (Tx*1000h + Mx*Vx) SAR (sf*12); IR = MAC
//   mx: 0=RT 1=LLM 2=LCM 3=garbage [-R*10h, R*10h, IR0; RT13 x3; RT22 x3]
//   v:  0=V0 1=V1 2=V2 3=[IR1,IR2,IR3]
//   cv: 0=TR 1=BK 2=FC (bugged) 3=none
void Gte::Op_MVMVA(uint32_t cmd) {
    const uint32_t mx = (cmd >> 17) & 3, vi = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    int64_t m[3][3], v[3], t[3] = {0, 0, 0};
    if (mx == 3) {
        const int64_t r = (m_data[6] & 0xFF) << 4;
        const int64_t rt13 = Lo(m_ctrl[1]), rt22 = Lo(m_ctrl[2]);
        m[0][0] = -r;   m[0][1] = r;    m[0][2] = IR(0);
        m[1][0] = rt13; m[1][1] = rt13; m[1][2] = rt13;
        m[2][0] = rt22; m[2][1] = rt22; m[2][2] = rt22;
    } else {
        Mat(mx * 8, m); // RT=ctrl0, LLM=ctrl8, LCM=ctrl16
    }
    Vec(static_cast<int>(vi), v);
    if (cv < 3) CtrlVec(cv == 0 ? 5 : cv == 1 ? 13 : 21, t);

    if (cv != 2) {
        MulMatVec(m, v, t, m_lm);
        return;
    }
    // FC bug: flags are computed for FC*1000h + Mx1*Vx, but the result is only Mx2*Vy + Mx3*Vz.
    for (int i = 0; i < 3; ++i) {
        int64_t acc = CheckMac(i + 1, t[i] * 0x1000);
        acc = CheckMac(i + 1, acc + m[i][0] * v[0]);
        SetIr(i + 1, acc >> m_shift, false);
        acc = CheckMac(i + 1, m[i][1] * v[1]);
        SetMacIr(i + 1, acc + m[i][2] * v[2], m_lm);
    }
}

// OP: [MAC1..3] = [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2] SAR (sf*12), D = RT11,RT22,RT33
void Gte::Op_OP() {
    const int64_t d1 = Lo(m_ctrl[0]), d2 = Lo(m_ctrl[2]), d3 = Lo(m_ctrl[4]);
    const int64_t i1 = IR(1), i2 = IR(2), i3 = IR(3);
    SetMac(1, i3 * d2 - i2 * d3);
    SetMac(2, i1 * d3 - i3 * d1);
    SetMac(3, i2 * d1 - i1 * d2);
    for (int i = 1; i <= 3; ++i) SetIr(i, MAC(i), m_lm);
}

// SQR: [MAC1..3] = [IR1..3]^2 SAR (sf*12); IR = MAC
void Gte::Op_SQR() {
    for (int i = 1; i <= 3; ++i) SetMacIr(i, int64_t(IR(i)) * IR(i), m_lm);
}

// GPF: MAC = (IR*IR0) SAR (sf*12); IR = MAC; Color FIFO
void Gte::Op_GPF() {
    for (int i = 1; i <= 3; ++i) SetMacIr(i, int64_t(IR(i)) * IR(0), m_lm);
    PushColor();
}

// GPL: MAC = (MAC SHL (sf*12) + IR*IR0) SAR (sf*12); IR = MAC; Color FIFO
void Gte::Op_GPL() {
    for (int i = 1; i <= 3; ++i)
        SetMacIr(i, int64_t(MAC(i)) * (int64_t(1) << m_shift) + int64_t(IR(i)) * IR(0), m_lm);
    PushColor();
}

// -----------------------------------------------------------------------------
// psx-spx "GTE Color Calculation Commands"
// -----------------------------------------------------------------------------
// NCS/NCCS/NCDS core for Vn:
//   IR = MAC = (LLM*Vn) SAR (sf*12)
//   IR = MAC = (BK*1000h + LCM*IR) SAR (sf*12)
//   NCCS: MAC = [R*IR1,G*IR2,B*IR3] SHL 4, SAR (sf*12)
//   NCDS: same, then depth-cue interpolation
//   Color FIFO = MAC/16
void Gte::Nc(int n, int mode) {
    int64_t m[3][3], v[3], t[3] = {0, 0, 0};
    Mat(8, m);
    Vec(n, v);
    MulMatVec(m, v, t, m_lm);
    Mat(16, m);
    Vec(3, v);
    CtrlVec(13, t);
    MulMatVec(m, v, t, m_lm);
    if (mode != 0) {
        int64_t c[3];
        ColorTimesIr(c);
        if (mode == 1) for (int i = 0; i < 3; ++i) SetMacIr(i + 1, c[i], m_lm);
        else Interpolate(c);
    }
    PushColor();
}

// CC: IR = MAC = (BK*1000h + LCM*IR) SAR sf*12; MAC = [R*IR1,G*IR2,B*IR3] SHL 4 SAR sf*12
void Gte::Op_CC() {
    int64_t m[3][3], v[3], t[3], c[3];
    Mat(16, m); Vec(3, v); CtrlVec(13, t);
    MulMatVec(m, v, t, m_lm);
    ColorTimesIr(c);
    for (int i = 0; i < 3; ++i) SetMacIr(i + 1, c[i], m_lm);
    PushColor();
}

// CDP: as CC, plus depth-cue interpolation
void Gte::Op_CDP() {
    int64_t m[3][3], v[3], t[3], c[3];
    Mat(16, m); Vec(3, v); CtrlVec(13, t);
    MulMatVec(m, v, t, m_lm);
    ColorTimesIr(c);
    Interpolate(c);
    PushColor();
}

// DCPL: MAC = [R*IR1,G*IR2,B*IR3] SHL 4, interpolate
void Gte::Op_DCPL() {
    int64_t c[3];
    ColorTimesIr(c);
    Interpolate(c);
    PushColor();
}

// DPCS: MAC = [R,G,B] SHL 16, interpolate
void Gte::Op_DPCS() {
    int64_t c[3];
    for (int i = 0; i < 3; ++i) c[i] = int64_t((m_data[6] >> (8 * i)) & 0xFF) << 16;
    Interpolate(c);
    PushColor();
}

// DPCT: DPCS three times on RGB0 (the FIFO push brings RGB1, RGB2 into RGB0); CODE stays RGBC's
void Gte::Op_DPCT() {
    for (int n = 0; n < 3; ++n) {
        int64_t c[3];
        for (int i = 0; i < 3; ++i) c[i] = int64_t((m_data[20] >> (8 * i)) & 0xFF) << 16;
        Interpolate(c);
        PushColor();
    }
}

// INTPL: MAC = [IR1,IR2,IR3] SHL 12, interpolate
void Gte::Op_INTPL() {
    int64_t c[3];
    for (int i = 0; i < 3; ++i) c[i] = int64_t(IR(i + 1)) * 0x1000;
    Interpolate(c);
    PushColor();
}

} // namespace personarm
