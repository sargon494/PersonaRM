#include "audio/xa_adpcm.h"

namespace personarm {

namespace {
// psx-spx XA filter tables (only four filters, unlike SPU-ADPCM).
constexpr int kPos[4] = {0, 60, 115, 98};
constexpr int kNeg[4] = {0, 0, -52, -55};

int16_t Clamp16(int32_t v) { return static_cast<int16_t>(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v)); }
} // namespace

void XaAdpcmDecoder::Reset() {
    m_old[0] = m_old[1] = 0;
    m_older[0] = m_older[1] = 0;
}

// psx-spx decode_28_nibbles: header byte 4+unit holds shift (bits 0-3) and filter (bits 4-5);
// 4-bit sample j of unit u is nibble (u&1) of byte 16+(u>>1)+j*4, 8-bit sample is byte 16+u+j*4.
// s = (t << (12 or 8)) >> range + (old*pos + older*neg + 32) / 64, clamped to 16 bit.
// unverified: range 13..15 behaves as 9 (psx-spx "reserved/same as 9", 4-bit); same rule applied to
// 8-bit; "/64" taken as arithmetic shift.
void XaAdpcmDecoder::DecodeGroup(const uint8_t* g, const XaFormat& fmt, std::vector<int16_t>& out) {
    const int units = fmt.eight_bit ? 4 : 8;
    int16_t pcm[8][28];
    for (int u = 0; u < units; ++u) {
        const uint8_t param = g[4 + u];
        int range = param & 0x0F;
        if (range > 12) range = 9;
        const int filter = (param >> 4) & 3;
        const int ch = fmt.stereo ? (u & 1) : 0;
        for (int j = 0; j < 28; ++j) {
            int32_t t;
            if (fmt.eight_bit) {
                t = static_cast<int8_t>(g[16 + u + j * 4]) * (1 << 8);
            } else {
                const int nib = (g[16 + (u >> 1) + j * 4] >> ((u & 1) * 4)) & 0x0F;
                t = (static_cast<int8_t>(nib << 4) >> 4) * (1 << 12);
            }
            const int32_t s = (t >> range) + ((m_old[ch] * kPos[filter] + m_older[ch] * kNeg[filter] + 32) >> 6);
            pcm[u][j] = Clamp16(s);
            m_older[ch] = m_old[ch];
            m_old[ch] = pcm[u][j];
        }
    }
    if (!fmt.stereo) {
        for (int u = 0; u < units; ++u) out.insert(out.end(), pcm[u], pcm[u] + 28);
        return;
    }
    for (int u = 0; u < units; u += 2) {
        for (int j = 0; j < 28; ++j) {
            out.push_back(pcm[u][j]);
            out.push_back(pcm[u + 1][j]);
        }
    }
}

void XaAdpcmDecoder::DecodeSector(const uint8_t* data, uint8_t coding_info, std::vector<int16_t>& out) {
    const XaFormat fmt = XaFormat::FromCodingInfo(coding_info);
    for (size_t i = 0; i < kGroupsPerSector; ++i) DecodeGroup(data + i * kGroupBytes, fmt, out);
}

bool XaAdpcmDecoder::DecodeRawSector(const uint8_t* raw, std::vector<int16_t>& out) {
    const uint8_t submode = raw[0x12];
    if ((submode & 0x24) != 0x24) return false;
    DecodeSector(raw + 0x18, raw[0x13], out);
    return true;
}

void XaResampler::Reset() {
    m_prev[0] = m_prev[1] = 0;
    m_acc = 0;
}

void XaResampler::Process(const int16_t* in, size_t frames, int channels, uint32_t in_rate, std::vector<int16_t>& out) {
    for (size_t f = 0; f < frames; ++f) {
        const int16_t* x = in + f * channels;
        while (m_acc < kOutRate) {
            for (int c = 0; c < channels; ++c)
                out.push_back(static_cast<int16_t>(m_prev[c] + static_cast<int64_t>(x[c] - m_prev[c]) * m_acc / kOutRate));
            m_acc += in_rate;
        }
        m_acc -= kOutRate;
        for (int c = 0; c < channels; ++c) m_prev[c] = x[c];
    }
}

} // namespace personarm
