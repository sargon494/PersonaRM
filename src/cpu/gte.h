#pragma once

#include <cstdint>
#include <array>
#include <vector>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 Geometry Transformation Engine (GTE - Coprocessor 2)
// ───────────────────────────────────────────────────────────────────────────

struct GteVector16 {
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    int16_t pad = 0;
};

struct GteVector32 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct GteMatrix {
    int16_t m[3][3]{}; // 1.3.12 fixed-point representation
};

class Gte {
public:
    Gte();
    ~Gte() = default;

    void Reset();

    // Register access
    uint32_t ReadDataRegister(uint32_t reg) const;
    void WriteDataRegister(uint32_t reg, uint32_t value);

    uint32_t ReadControlRegister(uint32_t reg) const;
    void WriteControlRegister(uint32_t reg, uint32_t value);

    // Command Execution (called on COP2 instruction)
    void ExecuteCommand(uint32_t cmd);

    // Core GTE operations
    void CommandRTPS();  // Perspective Transformation Single
    void CommandRTPT();  // Perspective Transformation Triple
    void CommandNCLIP(); // Normal Clipping (Backface culling test)
    void CommandAVSZ3(); // Average of 3 Z values
    void CommandAVSZ4(); // Average of 4 Z values
    void CommandMVMVA(); // Multiply Vector by Matrix and Vector Add

    uint32_t GetFlag() const { return m_ctrl_regs[31]; }

private:
    // 32 Data Registers (R0 - R31)
    std::array<uint32_t, 32> m_data_regs{};

    // 32 Control Registers (R0 - R31)
    std::array<uint32_t, 32> m_ctrl_regs{};

    // Helper functions for 1.3.12 fixed-point multiplication & saturation
    static int64_t SaturateMAC(int64_t val, int mac_idx, uint32_t& flag);
};

} // namespace personarm

