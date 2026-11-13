#pragma once
// CD-ROM XA-ADPCM decoder (Form2 audio sectors: 18 sound groups of 128 bytes) and a 44.1 kHz
// resampler. Source: psx-spx "CDROM XA Audio ADPCM Compression" (decode_28_nibbles) and the
// codinginfo layout in "CDROM Controller I/O Ports" (bit0 stereo, bit2 18900 Hz, bit4 8-bit).
// unverified: the sound-group layout and filters are recalled, not re-read this session (the
// codinginfo bits were re-read). Real-disc decode of F0865.EXT is the sanity check.
// Wiring: cd.SetXaCallback([&](const uint8_t* raw){ xa.DecodeRawSector(raw, pcm); ... }).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace personarm {

struct XaFormat {
    bool stereo = false;
    bool eight_bit = false;
    uint32_t rate = 37800;
    static XaFormat FromCodingInfo(uint8_t ci) { return {(ci & 0x01) != 0, (ci & 0x10) != 0, (ci & 0x04) ? 18900u : 37800u}; }
};

class XaAdpcmDecoder {
public:
    static constexpr size_t kGroupBytes = 128;
    static constexpr size_t kGroupsPerSector = 18;

    void Reset();

    /// Decodes one 128-byte sound group; appends samples (L,R interleaved when stereo).
    /// 4-bit: 8 units x 28 samples, 8-bit: 4 units x 28. Stereo: even units left, odd right.
    void DecodeGroup(const uint8_t* group, const XaFormat& fmt, std::vector<int16_t>& out);

    /// Decodes the 18 sound groups (900h bytes) of one audio sector.
    void DecodeSector(const uint8_t* data, uint8_t coding_info, std::vector<int16_t>& out);

    /// Raw 2352-byte sector (subheader at 10h, data at 18h). Returns false unless the submode
    /// marks a Form2 audio sector (bit2 audio, bit5 form2).
    bool DecodeRawSector(const uint8_t* raw2352, std::vector<int16_t>& out);

private:
    int32_t m_old[2] = {0, 0};
    int32_t m_older[2] = {0, 0};
};

/// shortcut: linear interpolation to 44100 Hz; the hardware upsamples with a 7-phase "zigzag"
/// FIR (unverified: psx-spx table not checked). Upgrade if XA audio sounds dull/aliased.
class XaResampler {
public:
    static constexpr uint32_t kOutRate = 44100;
    void Reset();
    /// Appends kOutRate frames converted from `frames` interleaved frames of `channels` (1 or 2).
    void Process(const int16_t* in, size_t frames, int channels, uint32_t in_rate, std::vector<int16_t>& out);

private:
    int32_t m_prev[2] = {0, 0};
    uint32_t m_acc = 0;  // position between m_prev and the next input, in 1/kOutRate units
};

} // namespace personarm
