#include "cpu/gte.h"
#include <algorithm>
#include <cmath>

namespace personarm {

Gte::Gte() {
    Reset();
}

void Gte::Reset() {
    m_data_regs.fill(0);
    m_ctrl_regs.fill(0);
}

uint32_t Gte::ReadDataRegister(uint32_t reg) const {
    if (reg >= 32) return 0;
    return m_data_regs[reg];
}

void Gte::WriteDataRegister(uint32_t reg, uint32_t value) {
    if (reg >= 32) return;
    m_data_regs[reg] = value;
}

uint32_t Gte::ReadControlRegister(uint32_t reg) const {
    if (reg >= 32) return 0;
    return m_ctrl_regs[reg];
}

void Gte::WriteControlRegister(uint32_t reg, uint32_t value) {
    if (reg >= 32) return;
    m_ctrl_regs[reg] = value;
}

void Gte::ExecuteCommand(uint32_t cmd) {
    uint32_t opcode = cmd & 0x3F;
    switch (opcode) {
        case 0x01: CommandRTPS();  break;
        case 0x30: CommandRTPT();  break;
        case 0x06: CommandNCLIP(); break;
        case 0x2D: CommandAVSZ3(); break;
        case 0x2E: CommandAVSZ4(); break;
        case 0x12: CommandMVMVA(); break;
        default:
            break;
    }
}

// ───────────────────────────────────────────────────────────────────────────
// NCLIP: Normal Clipping (cross-product Z component of 2D screen vertices)
// Computes: MAC0 = SXY0.X * (SXY1.Y - SXY2.Y) + SXY1.X * (SXY2.Y - SXY0.Y) + SXY2.X * (SXY0.Y - SXY1.Y)
// ───────────────────────────────────────────────────────────────────────────
void Gte::CommandNCLIP() {
    // SXY0 = reg 12, SXY1 = reg 13, SXY2 = reg 14
    int16_t sx0 = static_cast<int16_t>(m_data_regs[12] & 0xFFFF);
    int16_t sy0 = static_cast<int16_t>((m_data_regs[12] >> 16) & 0xFFFF);

    int16_t sx1 = static_cast<int16_t>(m_data_regs[13] & 0xFFFF);
    int16_t sy1 = static_cast<int16_t>((m_data_regs[13] >> 16) & 0xFFFF);

    int16_t sx2 = static_cast<int16_t>(m_data_regs[14] & 0xFFFF);
    int16_t sy2 = static_cast<int16_t>((m_data_regs[14] >> 16) & 0xFFFF);

    int64_t mac0 = static_cast<int64_t>(sx0) * (sy1 - sy2) +
                   static_cast<int64_t>(sx1) * (sy2 - sy0) +
                   static_cast<int64_t>(sx2) * (sy0 - sy1);

    // MAC0 is data reg 24
    m_data_regs[24] = static_cast<uint32_t>(mac0);
}

// ───────────────────────────────────────────────────────────────────────────
// AVSZ3: Average of 3 Z values
// Averaging Z0, Z1, Z2 using average factor ZSF3 (ctrl reg 29)
// ───────────────────────────────────────────────────────────────────────────
void Gte::CommandAVSZ3() {
    uint32_t zsf3 = m_ctrl_regs[29]; // 1.3.12 fixed point
    uint16_t sz1 = static_cast<uint16_t>(m_data_regs[17] & 0xFFFF);
    uint16_t sz2 = static_cast<uint16_t>(m_data_regs[18] & 0xFFFF);
    uint16_t sz3 = static_cast<uint16_t>(m_data_regs[19] & 0xFFFF);

    int64_t sum = static_cast<int64_t>(sz1) + sz2 + sz3;
    int64_t mac0 = sum * zsf3;

    m_data_regs[24] = static_cast<uint32_t>(mac0); // MAC0
    m_data_regs[20] = static_cast<uint32_t>(std::clamp<int64_t>(mac0 >> 12, 0, 0xFFFF)); // OTZ (reg 20)
}

// ───────────────────────────────────────────────────────────────────────────
// AVSZ4: Average of 4 Z values
// Averaging Z0, Z1, Z2, Z3 using average factor ZSF4 (ctrl reg 30)
// ───────────────────────────────────────────────────────────────────────────
void Gte::CommandAVSZ4() {
    uint32_t zsf4 = m_ctrl_regs[30];
    uint16_t sz0 = static_cast<uint16_t>(m_data_regs[16] & 0xFFFF);
    uint16_t sz1 = static_cast<uint16_t>(m_data_regs[17] & 0xFFFF);
    uint16_t sz2 = static_cast<uint16_t>(m_data_regs[18] & 0xFFFF);
    uint16_t sz3 = static_cast<uint16_t>(m_data_regs[19] & 0xFFFF);

    int64_t sum = static_cast<int64_t>(sz0) + sz1 + sz2 + sz3;
    int64_t mac0 = sum * zsf4;

    m_data_regs[24] = static_cast<uint32_t>(mac0);
    m_data_regs[20] = static_cast<uint32_t>(std::clamp<int64_t>(mac0 >> 12, 0, 0xFFFF)); // OTZ
}

// ───────────────────────────────────────────────────────────────────────────
// RTPS: Perspective Transformation of Single Vector (V0)
// Transforms V0 by Rotation Matrix (ctrl 0-4) + Translation Vector (ctrl 5-7),
// computes perspective division H = (H * projection_dist) / Z, pushes to SXY FIFO
// ───────────────────────────────────────────────────────────────────────────
void Gte::CommandRTPS() {
    // V0: VX0 (reg 0, 16-bit), VY0 (reg 0 >> 16), VZ0 (reg 1, 16-bit)
    int16_t vx = static_cast<int16_t>(m_data_regs[0] & 0xFFFF);
    int16_t vy = static_cast<int16_t>((m_data_regs[0] >> 16) & 0xFFFF);
    int16_t vz = static_cast<int16_t>(m_data_regs[1] & 0xFFFF);

    // Rotation Matrix: R11..R33 in ctrl regs 0..4 (each 32-bit holds two 16-bit values)
    int16_t r11 = static_cast<int16_t>(m_ctrl_regs[0] & 0xFFFF);
    int16_t r12 = static_cast<int16_t>((m_ctrl_regs[0] >> 16) & 0xFFFF);
    int16_t r13 = static_cast<int16_t>(m_ctrl_regs[1] & 0xFFFF);
    int16_t r21 = static_cast<int16_t>((m_ctrl_regs[1] >> 16) & 0xFFFF);
    int16_t r22 = static_cast<int16_t>(m_ctrl_regs[2] & 0xFFFF);
    int16_t r23 = static_cast<int16_t>((m_ctrl_regs[2] >> 16) & 0xFFFF);
    int16_t r31 = static_cast<int16_t>(m_ctrl_regs[3] & 0xFFFF);
    int16_t r32 = static_cast<int16_t>((m_ctrl_regs[3] >> 16) & 0xFFFF);
    int16_t r33 = static_cast<int16_t>(m_ctrl_regs[4] & 0xFFFF);

    // Translation Vector: TRX (ctrl 5), TRY (ctrl 6), TRZ (ctrl 7)
    int32_t trx = static_cast<int32_t>(m_ctrl_regs[5]);
    int32_t try_ = static_cast<int32_t>(m_ctrl_regs[6]);
    int32_t trz = static_cast<int32_t>(m_ctrl_regs[7]);

    // Matrix Multiply (1.3.12 fixed-point) + Translation
    int64_t mac1 = ((static_cast<int64_t>(r11) * vx + static_cast<int64_t>(r12) * vy + static_cast<int64_t>(r13) * vz) >> 12) + trx;
    int64_t mac2 = ((static_cast<int64_t>(r21) * vx + static_cast<int64_t>(r22) * vy + static_cast<int64_t>(r23) * vz) >> 12) + try_;
    int64_t mac3 = ((static_cast<int64_t>(r31) * vx + static_cast<int64_t>(r32) * vy + static_cast<int64_t>(r33) * vz) >> 12) + trz;

    // Shift FIFO for Z values: SZ0 <- SZ1 <- SZ2 <- SZ3
    m_data_regs[16] = m_data_regs[17];
    m_data_regs[17] = m_data_regs[18];
    m_data_regs[18] = m_data_regs[19];
    m_data_regs[19] = static_cast<uint32_t>(std::clamp<int64_t>(mac3, 0, 0xFFFF)); // SZ3

    // Shift FIFO for Screen coordinates: SXY0 <- SXY1 <- SXY2
    m_data_regs[12] = m_data_regs[13];
    m_data_regs[13] = m_data_regs[14];

    // Projection Distance H (ctrl reg 28), OFX (ctrl reg 24), OFY (ctrl reg 25)
    uint32_t h_val = m_ctrl_regs[28] & 0xFFFF;
    int32_t ofx = static_cast<int32_t>(m_ctrl_regs[24]);
    int32_t ofy = static_cast<int32_t>(m_ctrl_regs[25]);

    int64_t z_div = mac3 > 0 ? mac3 : 1;
    int64_t sx = ((mac1 * h_val) / z_div) + (ofx >> 16);
    int64_t sy = ((mac2 * h_val) / z_div) + (ofy >> 16);

    int16_t clamped_sx = static_cast<int16_t>(std::clamp<int64_t>(sx, -1024, 1023));
    int16_t clamped_sy = static_cast<int16_t>(std::clamp<int64_t>(sy, -1024, 1023));

    // Store SXY2 (reg 14)
    m_data_regs[14] = (static_cast<uint32_t>(static_cast<uint16_t>(clamped_sy)) << 16) |
                       static_cast<uint16_t>(clamped_sx);
}

void Gte::CommandRTPT() {
    // RTPT transforms 3 vectors in one call (V0, V1, V2). Emulate by sequential pipeline transformation
    CommandRTPS();
    CommandRTPS();
    CommandRTPS();
}

void Gte::CommandMVMVA() {
    // Basic Multiply Vector by Matrix Vector Add
    int16_t vx = static_cast<int16_t>(m_data_regs[0] & 0xFFFF);
    int16_t vy = static_cast<int16_t>((m_data_regs[0] >> 16) & 0xFFFF);
    int16_t vz = static_cast<int16_t>(m_data_regs[1] & 0xFFFF);

    int16_t r11 = static_cast<int16_t>(m_ctrl_regs[0] & 0xFFFF);
    int16_t r12 = static_cast<int16_t>((m_ctrl_regs[0] >> 16) & 0xFFFF);
    int16_t r13 = static_cast<int16_t>(m_ctrl_regs[1] & 0xFFFF);

    int64_t mac1 = ((static_cast<int64_t>(r11) * vx + static_cast<int64_t>(r12) * vy + static_cast<int64_t>(r13) * vz) >> 12);
    m_data_regs[25] = static_cast<uint32_t>(mac1); // MAC1
    m_data_regs[9]  = static_cast<uint32_t>(std::clamp<int64_t>(mac1, -32768, 32767) & 0xFFFF); // IR1
}

} // namespace personarm

