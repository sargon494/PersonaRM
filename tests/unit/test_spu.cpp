// Synthetic SPU tests (no game data).
#include "audio/spu_emulator.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

using namespace personarm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void Run(SpuEmulator& s, size_t frames) { std::vector<int16_t> o; s.GenerateSamples(o, frames); }

// Block header + 14 data bytes (nibble i = low nibble of byte i/2 for even i).
static void PutBlock(SpuEmulator& s, uint32_t addr, uint8_t shift, uint8_t filter, uint8_t flags, const uint8_t* data14) {
    uint8_t b[16] = { (uint8_t)(shift | (filter << 4)), flags };
    for (int i = 0; i < 14; ++i) b[2 + i] = data14 ? data14[i] : 0;
    s.UploadToRam(addr, b, 16);
}

static int ref_pos[5] = { 0, 60, 115, 98, 122 }, ref_neg[5] = { 0, 0, -52, -55, -60 };

static void TestAdpcm() {
    uint8_t d[14] = { 0x07, 0x8F, 0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB, 0xED, 0x0F, 0x12, 0x34, 0x56, 0x78 };
    for (int filter = 0; filter < 5; ++filter) {
        SpuEmulator s;
        PutBlock(s, 0x1000, 4, (uint8_t)filter, 0, d);
        s.SetVoiceStartAddress(0, 0x1000);
        s.KeyOn(1);
        Run(s, 1);
        int h1 = 0, h2 = 0; bool ok = true;
        for (int i = 0; i < 28; ++i) {
            int nib = (d[i / 2] >> ((i & 1) * 4)) & 0xF;
            int v = (int)(int16_t)(nib << 12) >> 4;
            v += (h1 * ref_pos[filter] + h2 * ref_neg[filter] + 32) >> 6;
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            h2 = h1; h1 = v;
            ok = ok && s.GetVoice(0).block_samples[i] == v;
        }
        CHECK(ok);
    }
    { // hand-computed: shift 0, filter 1, nibbles 7,0 -> 0x7000, (28672*60+32)>>6 = 26880
        SpuEmulator s; uint8_t e[14] = { 0x07 };
        PutBlock(s, 0, 0, 1, 0, e); s.KeyOn(1); Run(s, 1);
        CHECK(s.GetVoice(0).block_samples[0] == 28672);
        CHECK(s.GetVoice(0).block_samples[1] == 26880);
    }
    { // shift > 12 must not be UB and behaves as 9: 0x7000 >> 9 = 56
        SpuEmulator s; uint8_t e[14] = { 0x07 };
        PutBlock(s, 0, 15, 0, 0, e); s.KeyOn(1); Run(s, 1);
        CHECK(s.GetVoice(0).block_samples[0] == 56);
    }
    { // negative nibble: 0x8 << 12 = -32768 at shift 0
        SpuEmulator s; uint8_t e[14] = { 0x08 };
        PutBlock(s, 0, 0, 0, 0, e); s.KeyOn(1); Run(s, 1);
        CHECK(s.GetVoice(0).block_samples[0] == -32768);
    }
}

static uint32_t FramesUntilEndx(uint16_t pitch) {
    SpuEmulator s; uint8_t e[14] = { 0x11, 0x11 };
    PutBlock(s, 0, 0, 0, 1, e); // loop end, no repeat
    s.SetVoicePitch(0, pitch); s.KeyOn(1);
    for (uint32_t f = 1; f < 500; ++f) { Run(s, 1); if (s.GetEndx() & 1) return f; }
    return 0;
}

static void TestLoopAndPitch() {
    { // loop end without repeat: ENDX set, voice silenced
        SpuEmulator s; uint8_t e[14] = { 0x11 };
        PutBlock(s, 0, 0, 0, 1, e); s.KeyOn(1);
        CHECK(!(s.GetEndx() & 1)); Run(s, 40);
        CHECK(s.GetEndx() & 1); CHECK(s.GetActiveVoiceMask() == 0);
    }
    { // block0 loop start, block1 loop end + repeat: voice stays on, jumps back to block0
        SpuEmulator s; uint8_t e[14] = { 0x11 };
        PutBlock(s, 0x2000, 0, 0, 4, e); PutBlock(s, 0x2010, 0, 0, 3, e);
        s.SetVoiceStartAddress(0, 0x2000); s.KeyOn(1); Run(s, 70);
        CHECK(s.GetEndx() & 1); CHECK(s.GetActiveVoiceMask() == 1);
        CHECK(s.GetVoice(0).loop_address == 0x2000);
        CHECK(s.GetVoice(0).current_address == 0x2000 || s.GetVoice(0).current_address == 0x2010);
    }
    uint32_t a = FramesUntilEndx(0x1000), b = FramesUntilEndx(0x800), c = FramesUntilEndx(0x2000);
    CHECK(a >= 28 && a <= 30);
    CHECK(b >= 2 * a - 2 && b <= 2 * a + 2);
    CHECK(c >= a / 2 - 2 && c <= a / 2 + 2);
}

static void TestAdsr() {
    { // linear attack shift 12 step 3 -> +4 every 2 samples: 8 frames -> 16
        SpuEmulator s; s.SetVoiceStartAddress(0, 0);
        s.Write16(0x08, (12 << 10) | (3 << 8)); s.KeyOn(1); Run(s, 8);
        CHECK(s.GetVoice(0).adsr_level == 16 && s.GetVoice(0).adsr_phase == AdsrPhase::Attack);
    }
    { // fastest attack: +0x3800/sample -> 0x3800, 0x7000, then 0x7FFF (3 frames) -> Decay
        SpuEmulator s; s.Write16(0x08, 0x000F); // sustain level 15, decay shift 0
        s.KeyOn(1); Run(s, 1); CHECK(s.GetVoice(0).adsr_level == 0x3800);
        Run(s, 1); CHECK(s.GetVoice(0).adsr_level == 0x7000);
        Run(s, 1); CHECK(s.GetVoice(0).adsr_level == 0x7FFF || s.GetVoice(0).adsr_phase == AdsrPhase::Decay);
    }
    { // decay (exp) reaches sustain level 8 -> <= 0x4800 and Sustain
        SpuEmulator s; s.Write16(0x08, 0x0008); s.Write16(0x0A, 0x4000 | (31 << 8) | (3 << 6)); // sustain frozen
        s.KeyOn(1); Run(s, 12);
        CHECK(s.GetVoice(0).adsr_phase == AdsrPhase::Sustain);
        CHECK(s.GetVoice(0).adsr_level <= 0x4800);
    }
    { // sustain linear decrease shift 11 step 0: -8/sample
        SpuEmulator s; s.Write16(0x08, 0x000F); s.Write16(0x0A, 0x4000 | (11 << 8)); // dec, linear
        s.KeyOn(1); Run(s, 6);
        CHECK(s.GetVoice(0).adsr_phase == AdsrPhase::Sustain);
        uint16_t l0 = s.GetVoice(0).adsr_level; Run(s, 100);
        CHECK(s.GetVoice(0).adsr_level == l0 - 800);
    }
    { // release linear shift 11: -8/sample then Off at 0; release shift 0 = -0x4000/sample
        SpuEmulator s; s.Write16(0x08, 0x000F); s.Write16(0x0A, 0x0000); // sustain +0x3800/sample (linear), release shift 0 linear
        s.KeyOn(1); Run(s, 6); CHECK(s.GetVoice(0).adsr_level == 0x7FFF);
        s.KeyOff(1); Run(s, 1); CHECK(s.GetVoice(0).adsr_level == 0x3FFF);
        Run(s, 1); CHECK(s.GetVoice(0).adsr_phase == AdsrPhase::Off && s.GetVoice(0).adsr_level == 0);
    }
}

static void TestGaussAndMix() {
    const auto& g = SpuGaussTable();
    for (int i = 0; i < 256; ++i) { // each phase sums to ~0x8000 (hardware: 0x7F7F..0x7F81)
        int sum = g[0xFF - i] + g[0x1FF - i] + g[0x100 + i] + g[i];
        CHECK(sum >= 0x7F00 && sum <= 0x8100);
    }
    // table is generated (shortcut), so the per-phase sum property above is the contract, not a checksum
    CHECK(g[511] > g[256] && g[511] > 0x5000);

    { // DC input: constant samples mixed at full volume stay ~ constant (volume/ADSR/main = unity-ish)
        SpuEmulator s; uint8_t e[14]; for (auto& x : e) x = 0x44; // nibble 4, shift 12 filter 0 -> small; use shift 0
        PutBlock(s, 0, 0, 0, 2, e); PutBlock(s, 16, 0, 0, 3, e); // repeat flags: keep looping
        s.Write16(0x08, 0x000F); s.KeyOn(1);
        std::vector<int16_t> o; s.GenerateSamples(o, 200);
        CHECK(o[2 * 199] > 0 && o[2 * 199] == o[2 * 199 + 1]);
    }
    { // clamping: 24 loud voices cannot overflow int16
        SpuEmulator s; uint8_t e[14]; for (auto& x : e) x = 0x77;
        PutBlock(s, 0, 0, 0, 3, e);
        for (int v = 0; v < 24; ++v) { s.SetVoiceStartAddress(v, 0); s.Write16(v * 16 + 8, 0x000F); }
        s.KeyOn(0xFFFFFF);
        std::vector<int16_t> o; s.GenerateSamples(o, 64);
        CHECK(o[2 * 63] >= 32760);
        s.Write16(0x1AA, 0x0000); s.GenerateSamples(o, 8); CHECK(o[0] == 0); // muted
    }
    { // fixed volume decode: reg 0x4000 is -0x8000 (phase invert), sweep bit keeps previous
        SpuEmulator s; s.Write16(0x00, 0x3FFF); CHECK(s.GetVoice(0).volume_left == 0x7FFE);
        s.Write16(0x00, 0x4000); CHECK(s.GetVoice(0).volume_left == -32768);
        s.Write16(0x00, 0xC000); CHECK(s.GetVoice(0).volume_left == -32768);
        CHECK(s.Read16(0x1F801C00) == 0xC000);
    }
}

static void TestRegsAndReverb() {
    SpuEmulator s;
    s.Write16(0x1F801C36, 0x0200); // voice 3 start = 0x200*8
    CHECK(s.GetVoice(3).start_address == 0x1000 && s.Read16(0x1F801C36) == 0x0200);
    s.Write16(0x1F801DA6, 0x0100); s.Write16(0x1F801DA8, 0xBEEF); // manual transfer at 0x800
    CHECK(s.ReadRam(0x800) == 0xEF && s.ReadRam(0x801) == 0xBE);
    { // reverb: voice with EON + reverb enabled writes the work area; disabled does not
        for (int en = 0; en < 2; ++en) {
            SpuEmulator r; uint8_t e[14]; for (auto& x : e) x = 0x77;
            PutBlock(r, 0, 0, 0, 2, e);
            r.Write16(0x1F801DA2, 0x1000); // mBASE = 0x8000
            r.Write16(0x1F801DFC, 0x7FFF); r.Write16(0x1F801DFE, 0x7FFF); // vLIN/vRIN
            r.Write16(0x1F801DC4, 0x7FFF); r.Write16(0x1F801DCE, 0x4000); // vIIR / vWALL
            r.Write16(0x1F801DD4, 0x0010); r.Write16(0x1F801DD6, 0x0010); // mLSAME/mRSAME
            r.Write16(0x1F801DE0, 0x0020); r.Write16(0x1F801DE2, 0x0020);
            r.Write16(0x1F801D84, 0x4000); r.Write16(0x1F801D86, 0x4000);
            r.Write16(0x1F801D98, 1); r.Write16(0x08, 0x000F);
            r.Write16(0x1F801DAA, en ? 0xC080 : 0xC000);
            r.KeyOn(1); Run(r, 60); // < 64 ticks: before cursor revisits written cells
            bool any = false; for (uint32_t a = 0x8000; a < 0x80000; ++a) any = any || r.ReadRam(a);
            CHECK(any == (en == 1));
        }
    }
}

static void TestTimedOutputAndCd() {
    { // pacing: N cycles -> floor(N/768) frames, remainder carried across calls
        SpuEmulator s; std::vector<int16_t> o;
        s.Tick(767); s.TakeAudio(o); CHECK(o.empty());
        s.Tick(1); s.TakeAudio(o); CHECK(o.size() == 2);
        for (int i = 0; i < 60; ++i) s.Tick(564480); // one second of NTSC frames
        s.TakeAudio(o); CHECK(o.size() == 2 * 44100);
        s.TakeAudio(o); CHECK(o.empty());
    }
    { // CD audio: scaled by CD volume when SPUCNT bit 0 set; independent of SPU mute; FIFO consumed
        SpuEmulator s; std::vector<int16_t> o;
        const int16_t cd[8] = { 16000, -16000, 16000, -16000, 16000, -16000, 16000, -16000 };
        s.Write16(0x1F801DB0, 0x4000); s.Write16(0x1F801DB2, 0x7FFF); // CD vol L 0.5, R ~1.0
        s.Write16(0x1F801DAA, 0x0001);                                // CD on, SPU off+muted
        s.SetMasterVolume(0x3FFF, 0x3FFF);                            // main ~1.0
        s.PushCdAudio(cd, 4); CHECK(s.CdAudioQueued() == 4);
        s.GenerateSamples(o, 5);
        CHECK(std::abs(o[0] - 8000) <= 2 && std::abs(o[1] + 16000) <= 2);
        CHECK(o[8] == 0 && o[9] == 0 && s.CdAudioQueued() == 0); // underrun -> silence
        s.Write16(0x1F801DAA, 0xC000);                              // CD audio disabled: consumed, not heard
        s.PushCdAudio(cd, 2); s.GenerateSamples(o, 2);
        CHECK(o[0] == 0 && o[3] == 0 && s.CdAudioQueued() == 0);
        s.Write16(0x1F801DAA, 0x0001); s.Write16(0x1F801DB0, 0); s.Write16(0x1F801DB2, 0);
        s.PushCdAudio(cd, 1); s.GenerateSamples(o, 1); CHECK(o[0] == 0 && o[1] == 0); // volume 0
        s.SetMasterVolume(0x2000, 0x2000); s.Write16(0x1F801DB0, 0x7FFF);
        s.PushCdAudio(cd, 1); s.GenerateSamples(o, 1); CHECK(std::abs(o[0] - 8000) <= 2); // main vol applies
    }
    { // SPU RAM -> DMA read at the transfer address, wrapping at 512 KiB
        SpuEmulator s; const uint8_t d[4] = { 1, 2, 3, 4 };
        s.UploadToRam(0x7FFFE, d, 2); s.UploadToRam(0, d + 2, 2);
        s.Write16(0x1F801DA6, 0x7FFFE >> 3); // 8-byte units: 0x7FFF8
        uint8_t r[8] = {}; s.DmaRead(r, 8);
        CHECK(r[6] == 1 && r[7] == 2 && s.Read16(0x1F801DA6) == 0);
        uint8_t r2[2]; s.DmaRead(r2, 2); CHECK(r2[0] == 3 && r2[1] == 4);
    }
    { // SPUSTAT: mode bits mirror SPUCNT, DMA write/read request bits follow the transfer mode
        SpuEmulator s;
        s.Write16(0x1F801DAA, 0xC020); CHECK(s.Read16(0x1F801DAE) == 0x01A0); // DMA write: 5,7,8
        s.Write16(0x1F801DAA, 0xC030); CHECK(s.Read16(0x1F801DAE) == 0x02B0); // DMA read: 4,5,7,9
        s.Write16(0x1F801DAA, 0xC001); CHECK(s.Read16(0x1F801DAE) == 0x0001);
    }
}

int main() {
    TestAdpcm(); TestLoopAndPitch(); TestAdsr(); TestGaussAndMix(); TestRegsAndReverb(); TestTimedOutputAndCd();
    std::printf(g_fail ? "SPU tests FAILED (%d)\n" : "SPU tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
