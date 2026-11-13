#pragma once
// PS1 MDEC (0x1F801820 command/data, 0x1F801824 control/status). Source: psx-spx
// "Macroblock Decoder (MDEC)" (HW-050 in docs/research/PS1_HARDWARE_REQUIREMENTS.md).
// Decode math lives in rendering/mdec_decoder (mdec::BlockDecoder/Idct/YuvToRgb).
// FMV code is in overlay F0000; the resident EXE never touches these registers.
//
// WIRING (Engine, later change):
//   1. Own a `hw::Mdec mdec;`.
//   2. Bus: for addr in [Mdec::kBase, kBase+7] route Read32/Write32 with offset = addr - kBase
//      (0 = command/data, 4 = control/status; MDEC is a 32-bit-only port).
//   3. DMA: dma.SetPort(Dma::kMdecIn,  {nullptr, [&](uint32_t w){ mdec.DmaIn(w); }});
//           dma.SetPort(Dma::kMdecOut, {[&]{ return mdec.DmaOut(); }, nullptr});
//      Caveat: hw::Dma moves a whole block at start. PsyQ starts DMA1 (out) before DMA0 (in),
//      so DMA1 would drain an empty FIFO; DMA1 must wait for DataOutRequest() (DRQ pacing)
//      or be retried after DMA0 completes.
//   4. Call mdec.Tick(cycles) every step/batch (no-op today). MDEC raises no IRQ.
#include <cstdint>
#include <deque>

#include "rendering/mdec_decoder.h"

namespace personarm::hw {

class Mdec {
public:
    static constexpr uint32_t kBase = 0x1F801820;
    static constexpr uint32_t kResetStatus = 0x80040000;

    Mdec() { Reset(); }

    uint32_t Read32(uint32_t offset);
    void Write32(uint32_t offset, uint32_t value);

    /// DMA0 (RAM -> MDEC): same as a write to the command/parameter register.
    void DmaIn(uint32_t word) { Write32(0, word); }
    /// DMA1 (MDEC -> RAM): pops one word of decoded output (0 when empty).
    uint32_t DmaOut() { return Read32(0); }

    // shortcut: decoding is instant (output is ready as soon as a block's last halfword arrives);
    // real hw takes cycles per block. Add a cycle budget here if FMV timing ever matters.
    void Tick(uint32_t /*cycles*/) {}

    void Reset();
    uint32_t Status() const;
    bool DataInRequest() const { return (Status() >> 28) & 1; }
    bool DataOutRequest() const { return (Status() >> 27) & 1; }

private:
    void Command(uint32_t word);
    void Parameter(uint32_t word);
    void DecodeHalfword(uint16_t n);
    void EmitColor();
    void EmitMono(const int16_t y[64]);
    void PushBytes(const uint8_t* bytes, size_t n);

    uint32_t m_cmd = 0;          // current command word
    uint32_t m_left = 0;         // parameter words still expected
    uint16_t m_count_field = 0;  // status bits 15-0
    bool m_last_word = false;    // status bit 30 ("or last word received")
    bool m_en_in = false, m_en_out = false;
    uint32_t m_param_index = 0;  // word index within a table upload

    uint8_t m_qt_y[64] = {}, m_qt_uv[64] = {};
    int16_t m_scale[64] = {};

    mdec::BlockDecoder m_dec;
    int m_block = 0;  // index in Cr,Cb,Y1..Y4 (color) or 0 (mono)
    int16_t m_blocks[6][64] = {};
    std::deque<uint32_t> m_out;
};

} // namespace personarm::hw
