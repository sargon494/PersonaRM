#pragma once

#include <cstdint>
#include <array>

namespace personarm {

// -----------------------------------------------------------------------------
// PlayStation 1 Geometry Transformation Engine (GTE - Coprocessor 2)
// Reference: psx-spx "Geometry Transformation Engine (GTE)" and its sub-sections
// ("GTE Registers", "GTE Saturation", "GTE Coordinate Calculation Commands",
// "GTE General Purpose Calculation Commands", "GTE Color Calculation Commands",
// "GTE Division Inaccuracy").
// -----------------------------------------------------------------------------
class Gte {
public:
    Gte();
    ~Gte() = default;

    void Reset();

    // Register access (MFC2/MTC2/LWC2/SWC2 and CFC2/CTC2), with the side effects
    // described in psx-spx "GTE Registers" (sign extension, SXYP push, IRGB/ORGB,
    // LZCS/LZCR, FLAG bit 31).
    uint32_t ReadDataRegister(uint32_t reg) const;
    void WriteDataRegister(uint32_t reg, uint32_t value);

    uint32_t ReadControlRegister(uint32_t reg) const;
    void WriteControlRegister(uint32_t reg, uint32_t value);

    // Execute a COP2 command word (the full 32-bit instruction: sf = bit 19,
    // mx = bits 17-18, v = bits 15-16, cv = bits 13-14, lm = bit 10, op = bits 0-5).
    void ExecuteCommand(uint32_t cmd);

    uint32_t GetFlag() const { return m_ctrl[31]; }

    // psx-spx "GTE Division Inaccuracy": UNR Newton-Raphson division.
    // Returns min(1FFFFh, H*10000h/SZ3) as the hardware computes it; sets
    // FLAG.17 when H >= SZ3*2. Public for unit tests.
    uint32_t Divide(uint32_t h, uint32_t sz3);
    static uint8_t UnrTableEntry(uint32_t index); // 0..100h

private:
    std::array<uint32_t, 32> m_data{};
    std::array<uint32_t, 32> m_ctrl{};

    // Decoded fields of the command being executed.
    int m_shift = 0; // sf*12
    bool m_lm = false;

    // Register field helpers
    int16_t Lo(uint32_t v) const { return static_cast<int16_t>(v & 0xFFFF); }
    int16_t Hi(uint32_t v) const { return static_cast<int16_t>(v >> 16); }
    int32_t IR(int i) const { return static_cast<int32_t>(m_data[8 + i]); }
    int32_t MAC(int i) const { return static_cast<int32_t>(m_data[24 + i]); }
    void Vec(int n, int64_t out[3]) const; // n=0..2 -> V0..V2, n=3 -> [IR1,IR2,IR3]
    void Mat(uint32_t base, int64_t out[3][3]) const;
    void CtrlVec(uint32_t base, int64_t out[3]) const; // TR / BK / FC (3x s32)

    // Saturation / flag helpers (psx-spx "GTE Saturation")
    int64_t CheckMac(int i, int64_t v);              // 44-bit overflow, MAC1..3
    void SetMac(int i, int64_t v);                   // check, SAR shift, store MAC_i
    void SetIr(int i, int64_t v, bool lm);           // saturate into IR_i
    void SetMacIr(int i, int64_t v, bool lm) { SetMac(i, v); SetIr(i, MAC(i), lm); }
    int64_t SetMac0(int64_t v);                      // 32-bit overflow flag, store MAC0
    void PushColor();                                // RGB FIFO <- MAC/16
    void PushSz(int64_t v);
    void PushSxy(int64_t x, int64_t y);

    // Shared building blocks
    void MulMatVec(const int64_t m[3][3], const int64_t v[3], const int64_t t[3], bool lm);
    void Interpolate(const int64_t in[3]);           // DPCS/DCPL/INTPL/NCD*/CDP tail
    void ColorTimesIr(int64_t out[3]) const;         // [R*IR1,G*IR2,B*IR3] SHL 4 (pre-shift values)
    void Rtp(int n, bool last);
    void Nc(int n, int mode);                        // mode 0=NCS, 1=NCCS, 2=NCDS

    void Op_RTPS(); void Op_RTPT(); void Op_NCLIP(); void Op_OP();
    void Op_DPCS(); void Op_DPCT(); void Op_INTPL(); void Op_MVMVA(uint32_t cmd);
    void Op_CDP(); void Op_CC(); void Op_SQR(); void Op_DCPL();
    void Op_AVSZ3(); void Op_AVSZ4(); void Op_GPF(); void Op_GPL();
};

} // namespace personarm
