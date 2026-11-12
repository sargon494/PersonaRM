#include "audio/spu_emulator.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace personarm {

// psx-spx "SPU ADPCM Samples": filter 0..4 positive/negative coefficient tables (/64).
static const int32_t ADPCM_POS[5] = { 0, 60, 115,  98, 122 };
static const int32_t ADPCM_NEG[5] = { 0,  0, -52, -55, -60 };

// shortcut: psx-spx "SPU Interpolation" 512-entry Gaussian table could not be transcribed
// reliably offline; this is a cubic B-spline kernel with the same layout (indices used as
// gauss[0FFh-i], gauss[1FFh-i], gauss[100h+i], gauss[000h+i]) and per-phase sum ~0x8000
// (hardware: 0x7F7F..0x7F81). Upgrade: replace with the verbatim table (checksum test).
static const std::array<int16_t, 512>& GaussTable() {
    static const std::array<int16_t, 512> t = [] {
        std::array<int16_t, 512> g{};
        for (int i = 0; i < 256; ++i) {
            double u = (i + 1) / 256.0;                  // tail: (1-t)^3/6
            g[i] = (int16_t)std::lround(0x8000 * u * u * u / 6.0);
            double x = (i + 0.5) / 256.0;                // body: (1+3t+3t^2-3t^3)/6
            g[256 + i] = (int16_t)std::lround(0x8000 * (1 + 3 * x + 3 * x * x - 3 * x * x * x) / 6.0);
        }
        return g;
    }();
    return t;
}
const std::array<int16_t, 512>& SpuGaussTable() { return GaussTable(); }

static inline int16_t Clamp16(int32_t v) { return (int16_t)std::min(32767, std::max(-32768, v)); }
static inline int16_t DecodeVol(uint16_t reg, int16_t previous) {
    // shortcut: sweep mode (bit15) not emulated, previous effective volume is kept.
    return (reg & 0x8000) ? previous : (int16_t)(uint16_t)(reg << 1);
}

SpuEmulator::SpuEmulator() {
    m_spu_ram.resize(SPU_RAM_SIZE, 0);
    Reset();
}

void SpuEmulator::Reset() {
    std::fill(m_spu_ram.begin(), m_spu_ram.end(), 0);
    for (auto& v : m_voices) {
        v = SpuVoice{};
        v.vol_left_reg = v.vol_right_reg = 0x3FFF;
        v.volume_left = v.volume_right = 0x7FFE;
    }
    m_glob.fill(0);
    m_main_l = m_main_r = 0x7FFE;
    m_glob[0] = m_glob[1] = 0x3FFF;
    m_endx = 0; m_transfer_addr = 0;
    // shortcut: real power-on SPUCNT is 0 (disabled/muted) and the BIOS enables it; the engine
    // does not route SPU writes yet, so default to enabled+unmuted.
    m_spucnt = 0xC000;
    m_rev_cur = 0; m_rev_phase = false; m_rev_out_l = m_rev_out_r = 0;
}

uint8_t SpuEmulator::ReadRam(uint32_t a) const { return m_spu_ram[a & (SPU_RAM_SIZE - 1)]; }
void SpuEmulator::WriteRam(uint32_t a, uint8_t b) { m_spu_ram[a & (SPU_RAM_SIZE - 1)] = b; }

void SpuEmulator::UploadToRam(uint32_t dst, const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    dst &= (SPU_RAM_SIZE - 1);
    std::memcpy(m_spu_ram.data() + dst, data, std::min<size_t>(size, SPU_RAM_SIZE - dst));
}

void SpuEmulator::DmaWrite(const uint8_t* data, size_t size) {
    for (size_t i = 0; data && i < size; ++i) WriteRam(m_transfer_addr++, data[i]);
    m_transfer_addr &= (SPU_RAM_SIZE - 1);
}

void SpuEmulator::KeyOn(uint32_t mask) {
    for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i) {
        if (!(mask & (1u << i))) continue;
        auto& v = m_voices[i];
        v.current_address = v.start_address;
        v.loop_address = v.start_address; // psx-spx: KON copies start address to repeat address
        v.hist1 = v.hist2 = 0;
        v.interp.fill(0);
        v.pitch_counter = 0;
        v.adsr_level = 0; v.adsr_counter = 0;
        v.adsr_phase = AdsrPhase::Attack;
        v.sample_index = 28; v.block_valid = false;
        m_endx &= ~(1u << i);
    }
}

void SpuEmulator::KeyOff(uint32_t mask) {
    for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i)
        if ((mask & (1u << i)) && m_voices[i].adsr_phase != AdsrPhase::Off)
            m_voices[i].adsr_phase = AdsrPhase::Release;
}

void SpuEmulator::SetVoicePitch(uint32_t v, uint16_t p) { if (v < SPU_NUM_VOICES) m_voices[v].sample_rate_pitch = p; }
void SpuEmulator::SetVoiceVolume(uint32_t v, int16_t l, int16_t r) {
    if (v >= SPU_NUM_VOICES) return;
    auto& s = m_voices[v];
    s.vol_left_reg = (uint16_t)l; s.vol_right_reg = (uint16_t)r;
    s.volume_left = DecodeVol(s.vol_left_reg, s.volume_left);
    s.volume_right = DecodeVol(s.vol_right_reg, s.volume_right);
}
void SpuEmulator::SetVoiceStartAddress(uint32_t v, uint32_t a) { if (v < SPU_NUM_VOICES) m_voices[v].start_address = a & (SPU_RAM_SIZE - 1) & ~7u; }
void SpuEmulator::SetMasterVolume(int16_t l, int16_t r) {
    m_glob[0] = (uint16_t)l; m_glob[1] = (uint16_t)r;
    m_main_l = DecodeVol(m_glob[0], m_main_l);
    m_main_r = DecodeVol(m_glob[1], m_main_r);
}

uint32_t SpuEmulator::GetActiveVoiceMask() const {
    uint32_t m = 0;
    for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i)
        if (m_voices[i].adsr_phase != AdsrPhase::Off) m |= 1u << i;
    return m;
}

// psx-spx "SPU Voice Registers" (0x1F801C00 + n*0x10) and "SPU Control Registers" (0x1D80..).
void SpuEmulator::Write16(uint32_t addr, uint16_t val) {
    uint32_t off = (addr >= SPU_REG_BASE ? addr - SPU_REG_BASE : addr) & 0x1FE;
    if (off < 0x180) {
        auto& v = m_voices[off >> 4];
        switch (off & 0xF) {
            case 0x0: v.vol_left_reg = val;  v.volume_left  = DecodeVol(val, v.volume_left);  break;
            case 0x2: v.vol_right_reg = val; v.volume_right = DecodeVol(val, v.volume_right); break;
            case 0x4: v.sample_rate_pitch = val; break;
            case 0x6: v.start_address = ((uint32_t)val << 3) & (SPU_RAM_SIZE - 1); break;
            case 0x8: v.adsr1 = val; break;
            case 0xA: v.adsr2 = val; break;
            case 0xC: v.adsr_level = val & 0x7FFF; break;
            case 0xE: v.loop_address = ((uint32_t)val << 3) & (SPU_RAM_SIZE - 1); break;
        }
        return;
    }
    m_glob[(off - 0x180) >> 1] = val;
    switch (off) {
        case 0x180: m_main_l = DecodeVol(val, m_main_l); break;
        case 0x182: m_main_r = DecodeVol(val, m_main_r); break;
        case 0x188: KeyOn(val); break;
        case 0x18A: KeyOn((uint32_t)(val & 0xFF) << 16); break;
        case 0x18C: KeyOff(val); break;
        case 0x18E: KeyOff((uint32_t)(val & 0xFF) << 16); break;
        case 0x1A2: m_rev_cur = ((uint32_t)val << 3) & (SPU_RAM_SIZE - 1); break;
        case 0x1A6: m_transfer_addr = ((uint32_t)val << 3) & (SPU_RAM_SIZE - 1); break;
        case 0x1A8: // manual FIFO write
            WriteRam(m_transfer_addr, val & 0xFF); WriteRam(m_transfer_addr + 1, val >> 8);
            m_transfer_addr = (m_transfer_addr + 2) & (SPU_RAM_SIZE - 1); break;
        case 0x1AA: m_spucnt = val; break;
    }
}

uint16_t SpuEmulator::Read16(uint32_t addr) const {
    uint32_t off = (addr >= SPU_REG_BASE ? addr - SPU_REG_BASE : addr) & 0x1FE;
    if (off < 0x180) {
        const auto& v = m_voices[off >> 4];
        switch (off & 0xF) {
            case 0x0: return v.vol_left_reg;
            case 0x2: return v.vol_right_reg;
            case 0x4: return v.sample_rate_pitch;
            case 0x6: return (uint16_t)(v.start_address >> 3);
            case 0x8: return v.adsr1;
            case 0xA: return v.adsr2;
            case 0xC: return v.adsr_level;
            case 0xE: return (uint16_t)(v.loop_address >> 3);
        }
        return 0;
    }
    switch (off) {
        case 0x19C: return (uint16_t)(m_endx & 0xFFFF);
        case 0x19E: return (uint16_t)(m_endx >> 16);
        case 0x1A6: return (uint16_t)(m_transfer_addr >> 3);
        case 0x1AE: return m_spucnt & 0x3F; // SPUSTAT mirrors SPUCNT bits 0-5; busy/IRQ flags not modelled
        case 0x1B8: return (uint16_t)m_main_l;
        case 0x1BA: return (uint16_t)m_main_r;
    }
    return m_glob[(off - 0x180) >> 1];
}

// psx-spx "SPU ADPCM Samples".
void SpuEmulator::DecodeBlock(SpuVoice& v) {
    uint32_t a = v.current_address & (SPU_RAM_SIZE - 1);
    const uint8_t* b = &m_spu_ram[a & ~15u];
    uint32_t base = a & ~15u;
    uint8_t shift = b[0] & 0xF, filter = (b[0] >> 4) & 7, flags = b[1];
    if (shift > 12) shift = 9; // unverified: shift 13..15 treated as 9 (as in other emulators)
    if (filter > 4) filter = 4; // unverified: filters 5..7 are undefined on hardware
    if (flags & 4) v.loop_address = base; // loop start
    for (int i = 0; i < 28; ++i) {
        int nib = (b[2 + (i >> 1)] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (v.hist1 * ADPCM_POS[filter] + v.hist2 * ADPCM_NEG[filter] + 32) >> 6; // unverified: SAR vs /64
        int16_t o = Clamp16(s);
        v.hist2 = v.hist1; v.hist1 = o;
        v.block_samples[i] = o;
    }
    v.current_address = base;
    v.sample_index = 0; v.block_valid = true;
}

void SpuEmulator::PushSample(SpuVoice& v) {
    int idx = (int)(&v - m_voices.data());
    if (v.sample_index >= 28) {
        if (v.block_valid) {
            uint8_t flags = m_spu_ram[(v.current_address & ~15u) + 1];
            if (flags & 1) { // loop end
                m_endx |= 1u << idx;
                v.current_address = v.loop_address;
                if (!(flags & 2)) { // no repeat: voice is silenced
                    v.adsr_phase = AdsrPhase::Off; v.adsr_level = 0;
                }
            } else {
                v.current_address = (v.current_address + 16) & (SPU_RAM_SIZE - 1);
            }
        }
        if (v.adsr_phase == AdsrPhase::Off) { v.block_samples.fill(0); v.sample_index = 0; }
        else DecodeBlock(v);
    }
    v.interp = { v.interp[1], v.interp[2], v.interp[3], v.block_samples[v.sample_index++] };
}

// psx-spx "SPU ADSR Generator", cycles/step formulation.
void SpuEmulator::StepEnvelope(SpuVoice& v) {
    uint32_t shift, stepv; bool expo, dec;
    switch (v.adsr_phase) {
        case AdsrPhase::Attack:  shift = (v.adsr1 >> 10) & 31; stepv = (v.adsr1 >> 8) & 3; expo = v.adsr1 >> 15; dec = false; break;
        case AdsrPhase::Decay:   shift = (v.adsr1 >> 4) & 15;  stepv = 0; expo = true; dec = true; break;
        case AdsrPhase::Sustain: shift = (v.adsr2 >> 8) & 31;  stepv = (v.adsr2 >> 6) & 3; expo = v.adsr2 >> 15; dec = (v.adsr2 >> 14) & 1; break;
        case AdsrPhase::Release: shift = v.adsr2 & 31;         stepv = 0; expo = (v.adsr2 >> 5) & 1; dec = true; break;
        default: return;
    }
    if (shift == 31 && stepv == 3 && v.adsr_phase != AdsrPhase::Decay && v.adsr_phase != AdsrPhase::Release) return; // rate 0x7F never steps
    uint32_t cycles = 1u << (shift > 11 ? shift - 11 : 0);
    if (expo && !dec && v.adsr_level > 0x6000) cycles *= 4; // unverified: older "x4 above 6000h" formulation
    if (++v.adsr_counter < cycles) return;
    v.adsr_counter = 0;
    int32_t step = dec ? -8 + (int32_t)stepv : 7 - (int32_t)stepv;
    step *= 1 << (shift < 11 ? 11 - shift : 0);
    if (expo && dec) step = (step * (int32_t)v.adsr_level) >> 15;
    int32_t lvl = std::min(0x7FFF, std::max(0, (int32_t)v.adsr_level + step));
    v.adsr_level = (uint16_t)lvl;

    if (v.adsr_phase == AdsrPhase::Attack && lvl == 0x7FFF) v.adsr_phase = AdsrPhase::Decay, v.adsr_counter = 0;
    else if (v.adsr_phase == AdsrPhase::Decay && lvl <= (int32_t)(((v.adsr1 & 0xF) + 1) << 11)) v.adsr_phase = AdsrPhase::Sustain, v.adsr_counter = 0;
    else if (v.adsr_phase == AdsrPhase::Release && lvl == 0) v.adsr_phase = AdsrPhase::Off;
}

// psx-spx "SPU Reverb Formulas". Offsets are register*8 bytes, relative to the cursor, wrapping
// inside [mBASE, 0x80000).
uint32_t SpuEmulator::RevAddr(int32_t off) const {
    uint32_t base = ((uint32_t)G(0x1A2) << 3) & (SPU_RAM_SIZE - 1);
    uint32_t size = SPU_RAM_SIZE - base;
    int64_t rel = ((int64_t)m_rev_cur - base + off) % (int64_t)size;
    if (rel < 0) rel += size;
    return (uint32_t)((base + rel) & ~1u);
}
int32_t SpuEmulator::RevRead(int32_t off) const {
    uint32_t a = RevAddr(off);
    return (int16_t)(m_spu_ram[a] | (m_spu_ram[a + 1] << 8));
}
void SpuEmulator::RevWrite(int32_t off, int32_t v) {
    uint32_t a = RevAddr(off); int16_t s = Clamp16(v);
    m_spu_ram[a] = (uint8_t)s; m_spu_ram[a + 1] = (uint8_t)((uint16_t)s >> 8);
}

void SpuEmulator::ReverbTick(int32_t in_l, int32_t in_r) {
    auto R = [&](uint32_t reg) { return (int32_t)((uint32_t)G(reg) << 3); };
    auto V = [&](uint32_t reg) { return (int32_t)(int16_t)G(reg); };
    int32_t vIIR = V(0x1C4), vWALL = V(0x1CE);
    int32_t Lin = (in_l * V(0x1FC)) >> 15, Rin = (in_r * V(0x1FE)) >> 15;
    bool write = (m_spucnt & 0x80) != 0;
    auto reflect = [&](uint32_t m, uint32_t d, int32_t in) {
        int32_t prev = RevRead(R(m) - 2);
        int32_t nv = ((in + ((RevRead(R(d)) * vWALL) >> 15) - prev) * vIIR >> 15) + prev;
        if (write) RevWrite(R(m), nv);
    };
    reflect(0x1D4, 0x1E0, Lin);  // mLSAME <- dLSAME
    reflect(0x1D6, 0x1E2, Rin);  // mRSAME <- dRSAME
    reflect(0x1E4, 0x1F2, Lin);  // mLDIFF <- dRDIFF
    reflect(0x1E6, 0x1F0, Rin);  // mRDIFF <- dLDIFF

    auto side = [&](uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, uint32_t a1, uint32_t a2) {
        int32_t o = (V(0x1C6) * RevRead(R(c1)) + V(0x1C8) * RevRead(R(c2)) +
                     V(0x1CA) * RevRead(R(c3)) + V(0x1CC) * RevRead(R(c4))) >> 15;
        int32_t dA1 = R(0x1C0), dA2 = R(0x1C2);
        o = Clamp16(o - ((V(0x1D0) * RevRead(R(a1) - dA1)) >> 15));
        if (write) RevWrite(R(a1), o);
        o = Clamp16(((o * V(0x1D0)) >> 15) + RevRead(R(a1) - dA1));
        o = Clamp16(o - ((V(0x1D2) * RevRead(R(a2) - dA2)) >> 15));
        if (write) RevWrite(R(a2), o);
        return Clamp16(((o * V(0x1D2)) >> 15) + RevRead(R(a2) - dA2));
    };
    int32_t lo = side(0x1D8, 0x1DC, 0x1E8, 0x1EC, 0x1F4, 0x1F8);
    int32_t ro = side(0x1DA, 0x1DE, 0x1EA, 0x1EE, 0x1F6, 0x1FA);
    m_rev_out_l = (lo * V(0x184)) >> 15;
    m_rev_out_r = (ro * V(0x186)) >> 15;

    uint32_t base = ((uint32_t)G(0x1A2) << 3) & (SPU_RAM_SIZE - 1);
    m_rev_cur += 2;
    if (m_rev_cur >= SPU_RAM_SIZE || m_rev_cur < base) m_rev_cur = base;
}

void SpuEmulator::GenerateSamples(std::vector<int16_t>& out, size_t num_frames) {
    out.assign(num_frames * 2, 0);
    const auto& gauss = GaussTable();
    const bool enabled = (m_spucnt & 0x8000) != 0, unmuted = (m_spucnt & 0x4000) != 0;
    const uint32_t eon = G(0x198) | ((uint32_t)(G(0x19A) & 0xFF) << 16);
    for (size_t f = 0; f < num_frames; ++f) {
        int32_t sl = 0, sr = 0, rl = 0, rr = 0;
        for (uint32_t i = 0; i < SPU_NUM_VOICES; ++i) {
            SpuVoice& v = m_voices[i];
            if (v.adsr_phase == AdsrPhase::Off) continue;
            uint32_t ph = (v.pitch_counter >> 4) & 0xFF;
            int32_t s = ((gauss[0x0FF - ph] * v.interp[0]) >> 15) + ((gauss[0x1FF - ph] * v.interp[1]) >> 15) +
                        ((gauss[0x100 + ph] * v.interp[2]) >> 15) + ((gauss[0x000 + ph] * v.interp[3]) >> 15);
            s = (Clamp16(s) * (int32_t)v.adsr_level) >> 15;
            int32_t l = (s * v.volume_left) >> 15, r = (s * v.volume_right) >> 15;
            sl += l; sr += r;
            if (eon & (1u << i)) { rl += l; rr += r; }
            StepEnvelope(v);
            v.pitch_counter += std::min<uint32_t>(v.sample_rate_pitch, 0x3FFF); // psx-spx: max pitch 3FFFh
            while (v.pitch_counter >= 0x1000) { v.pitch_counter -= 0x1000; PushSample(v); }
        }
        m_rev_phase = !m_rev_phase; // reverb runs at 22.05 kHz
        if (m_rev_phase) ReverbTick(Clamp16(rl), Clamp16(rr));
        if (!enabled || !unmuted) continue;
        int32_t ol = ((Clamp16(sl) * m_main_l) >> 15) + (m_spucnt & 0x80 ? m_rev_out_l : 0);
        int32_t or_ = ((Clamp16(sr) * m_main_r) >> 15) + (m_spucnt & 0x80 ? m_rev_out_r : 0);
        out[f * 2] = Clamp16(ol); out[f * 2 + 1] = Clamp16(or_);
    }
}

} // namespace personarm
