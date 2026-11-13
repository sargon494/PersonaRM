#include "hw/mdec.h"

#include <cstring>

namespace personarm::hw {

namespace {
uint32_t Depth(uint32_t cmd) { return (cmd >> 27) & 3; }  // 0=4bit 1=8bit 2=24bit 3=15bit
bool IsColor(uint32_t cmd) { return Depth(cmd) >= 2; }
} // namespace

// psx-spx "MDEC I/O Ports": control bit31 reset sets status = 80040000h. Tables survive a reset
// (unverified: psx-spx does not say).
void Mdec::Reset() {
    m_cmd = 0;
    m_left = 0;
    m_count_field = 0;
    m_last_word = false;
    m_param_index = 0;
    m_dec = mdec::BlockDecoder{};
    m_block = 0;
    m_out.clear();
    m_ready = 0;
    m_pending.clear();
}

void Mdec::Tick(uint32_t cycles) {
    while (!m_pending.empty()) {
        Pending& p = m_pending.front();
        if (p.cycles > cycles) { p.cycles -= cycles; return; }
        cycles -= p.cycles;
        m_ready += p.words;
        m_pending.pop_front();
    }
}

// psx-spx MDEC status: 31 out-FIFO empty, 30 in-FIFO full/last word, 29 busy, 28 DREQ0, 27 DREQ1,
// 26-23 = command bits 28-25, 18-16 current block (0..3=Y1..Y4, 4=Cr, 5=Cb, mono 4=Y),
// 15-0 parameter words remaining minus 1.
// unverified: busy also covers undrained output; current block always reports the incoming block.
uint32_t Mdec::Status() const {
    const bool out_empty = m_ready == 0;
    uint32_t s = 0;
    if (out_empty) s |= 1u << 31;
    if (m_last_word) s |= 1u << 30;
    if (m_left > 0 || !m_out.empty()) s |= 1u << 29;
    if (m_en_in && m_left > 0) s |= 1u << 28;
    if (m_en_out && !out_empty) s |= 1u << 27;
    s |= ((m_cmd >> 25) & 0xF) << 23;
    const uint32_t block = IsColor(m_cmd) ? (m_block < 2 ? 4u + m_block : m_block - 2u) : 4u;
    s |= block << 16;
    return s | m_count_field;
}

uint32_t Mdec::Read32(uint32_t offset) {
    if ((offset & 4) != 0) return Status();
    if (m_ready == 0) return 0;
    const uint32_t w = m_out.front();
    m_out.pop_front();
    --m_ready;
    return w;
}

void Mdec::Write32(uint32_t offset, uint32_t value) {
    if ((offset & 4) != 0) {
        if (value & (1u << 31)) Reset();
        m_en_in = (value >> 30) & 1;
        m_en_out = (value >> 29) & 1;
        return;
    }
    if (m_left == 0)
        Command(value);
    else
        Parameter(value);
}

// psx-spx MDEC(1) decode (bits 15-0 = parameter words), MDEC(2) quant tables (16 or 32 words),
// MDEC(3) scale table (32 words), MDEC(0)/(4..7) no function: bits 15-0 go to status unchanged.
void Mdec::Command(uint32_t word) {
    m_cmd = word;
    m_last_word = false;
    m_param_index = 0;
    switch (word >> 29) {
    case 1:
        m_left = word & 0xFFFF;
        m_dec = mdec::BlockDecoder{};  // unverified: a new decode command restarts at a macroblock boundary
        m_block = 0;
        break;
    case 2: m_left = (word & 1) ? 32 : 16; break;
    case 3: m_left = 32; break;
    default:
        m_left = 0;
        m_count_field = static_cast<uint16_t>(word);
        return;
    }
    m_count_field = static_cast<uint16_t>(m_left - 1);
}

void Mdec::Parameter(uint32_t word) {
    --m_left;
    m_count_field = static_cast<uint16_t>(m_left - 1);
    m_last_word = m_left == 0;
    switch (m_cmd >> 29) {
    case 1:
        DecodeHalfword(static_cast<uint16_t>(word));  // little-endian halfword order
        DecodeHalfword(static_cast<uint16_t>(word >> 16));
        break;
    case 2:
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t idx = m_param_index * 4 + i;
            (idx < 64 ? m_qt_y[idx] : m_qt_uv[idx - 64]) = static_cast<uint8_t>(word >> (8 * i));
        }
        break;
    case 3:
        m_scale[m_param_index * 2] = static_cast<int16_t>(word);
        m_scale[m_param_index * 2 + 1] = static_cast<int16_t>(word >> 16);
        break;
    default: break;
    }
    ++m_param_index;
}

void Mdec::DecodeHalfword(uint16_t n) {
    const bool color = IsColor(m_cmd);
    if (!m_dec.Push(n, color && m_block < 2 ? m_qt_uv : m_qt_y)) return;
    int16_t* blk = m_blocks[color ? m_block : 0];
    std::memcpy(blk, m_dec.blk, sizeof(m_dec.blk));
    mdec::Idct(blk, m_scale);
    if (!color) {
        EmitMono(blk);
    } else if (++m_block == 6) {
        EmitColor();
        m_block = 0;
    }
}

// unverified: output packing (psx-spx MDEC page leaves it out): 24bit = R,G,B bytes per pixel,
// 15bit = bit15|B5<<10|G5<<5|R5, both 16x16 pixels row by row; 8bit = 64 bytes; 4bit = low nibble first.
void Mdec::EmitColor() {
    int8_t rgb[768];
    for (int i = 0; i < 4; ++i) mdec::YuvToRgb(m_blocks[0], m_blocks[1], m_blocks[2 + i], (i & 1) * 8, (i >> 1) * 8, rgb);
    const uint8_t x = ((m_cmd >> 26) & 1) ? 0x00 : 0x80;  // unsigned output = xor 80h
    uint8_t bytes[768];
    for (int i = 0; i < 768; ++i) bytes[i] = static_cast<uint8_t>(rgb[i]) ^ x;
    if (Depth(m_cmd) == 2) {
        PushBytes(bytes, 768, 6);
        return;
    }
    uint8_t out[512];
    const uint16_t stp = ((m_cmd >> 25) & 1) ? 0x8000 : 0;
    for (int p = 0; p < 256; ++p) {
        const uint8_t* c = bytes + p * 3;
        const uint16_t h = static_cast<uint16_t>(stp | ((c[2] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[0] >> 3));
        out[p * 2] = static_cast<uint8_t>(h);
        out[p * 2 + 1] = static_cast<uint8_t>(h >> 8);
    }
    PushBytes(out, 512, 6);
}

void Mdec::EmitMono(const int16_t y[64]) {
    const uint8_t x = ((m_cmd >> 26) & 1) ? 0x00 : 0x80;
    uint8_t px[64];
    for (int i = 0; i < 64; ++i) px[i] = static_cast<uint8_t>(mdec::YToMono(y[i])) ^ x;
    if (Depth(m_cmd) == 1) {
        PushBytes(px, 64, 1);
        return;
    }
    uint8_t out[32];
    for (int i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>((px[2 * i] >> 4) | (px[2 * i + 1] & 0xF0));
    PushBytes(out, 32, 1);
}

void Mdec::PushBytes(const uint8_t* b, size_t n, uint32_t blocks) {
    m_pending.push_back({static_cast<uint32_t>(n / 4), blocks * kCyclesPerBlock});
    for (size_t i = 0; i + 3 < n; i += 4)
        m_out.push_back(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (static_cast<uint32_t>(b[i + 3]) << 24));
}

} // namespace personarm::hw
