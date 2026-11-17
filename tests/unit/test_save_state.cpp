// Save states: each device's Serialize round-trips synthetic mid-operation state (save, load into a fresh
// device, save again: same bytes; then the same stimulus on both: same result), and Engine::SaveState /
// LoadState resume a running machine exactly and reject foreign or broken states without touching it.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/spu_emulator.h"
#include "core/save_manager.h"
#include "core/state_io.h"
#include "cpu/cpu_context.h"
#include "engine/engine.h"
#include "hw/cdrom.h"
#include "hw/dma.h"
#include "hw/mdec.h"
#include "loader/exe_loader.h"
#include "rendering/ps1_gpu.h"

using namespace personarm;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

template <class D> static std::vector<uint8_t> Save(D& d) {
    std::vector<uint8_t> b;
    StateIO io(b);
    d.Serialize(io);
    return b;
}
template <class D> static void Load(D& d, const std::vector<uint8_t>& b) {
    StateIO io(b.data(), b.size());
    d.Serialize(io);
    CHECK(io.Ok() && io.Remaining() == 0);
}
// Save `a`, load into `b`, save `b` again: identical bytes.
template <class D> static void RoundTrip(D& a, D& b) {
    const std::vector<uint8_t> s = Save(a);
    Load(b, s);
    CHECK(Save(b) == s);
}

static void TestStateIO() {
    std::vector<uint8_t> buf;
    StateIO w(buf);
    uint32_t x = 0x12345678;
    std::vector<uint16_t> v = {1, 2, 3};
    std::string s = "abc";
    w.Tag("TEST");
    w(x);
    w.Seq(v);
    w.Seq(s);
    StateIO r(buf.data(), buf.size());
    uint32_t x2 = 0;
    std::vector<uint16_t> v2;
    std::string s2;
    r.Tag("TEST");
    r(x2);
    r.Seq(v2);
    r.Seq(s2);
    CHECK(r.Ok() && r.Remaining() == 0 && x2 == x && v2 == v && s2 == s);
    StateIO bad_tag(buf.data(), buf.size());
    bad_tag.Tag("NOPE");
    CHECK(!bad_tag.Ok());
    StateIO cut(buf.data(), 6);  // tag + part of x
    cut.Tag("TEST");
    cut(x2);
    CHECK(!cut.Ok());
    std::vector<uint8_t> huge = buf;  // a length past the end of the buffer must not allocate it
    std::memset(huge.data() + 8, 0xFF, 8);
    StateIO h(huge.data(), huge.size());
    h.Tag("TEST");
    h(x2);
    h.Seq(v2);
    CHECK(!h.Ok());
}

static void TestCpu() {
    CpuContext a, b;
    CpuRegisters r;
    for (uint32_t i = 1; i < 32; ++i) r.gpr[i] = i * 0x01010101u;
    r.hi = 7, r.lo = 9, r.cp0_status = 0x401, r.cp0_cause = 0x400, r.cp0_epc = 0x80012340;
    a.SetRegisters(r);
    a.SetPCInDelaySlot(0x80010008, 0x80020000);  // branch delay slot pending
    a.SetPendingLoad(5, 0xCAFEBABE);             // load in flight
    a.SetBadVaddr(0x1234);
    a.AddCycles(123456789);
    a.GetGTE().WriteControlRegister(26, 0x155);  // H
    a.GetGTE().WriteDataRegister(0, 0x00100020); // VXY0
    a.GetGTE().ExecuteCommand(0x4A400012);       // MVMVA: sets MAC/IR/FLAG
    RoundTrip(a, b);
    CHECK(!b.AtCleanBoundary() && b.GetPC() == 0x80010008 && b.GetCycleCount() == 123456789 && b.GetBadVaddr() == 0x1234);
    CHECK(b.GetRegisters().gpr == a.GetRegisters().gpr && b.GetHI() == 7 && b.GetRegisters().cp0_epc == 0x80012340);
    for (uint32_t i = 0; i < 32; ++i) {
        CHECK(a.GetGTE().ReadDataRegister(i) == b.GetGTE().ReadDataRegister(i));
        CHECK(a.GetGTE().ReadControlRegister(i) == b.GetGTE().ReadControlRegister(i));
    }
    // A state saved with widescreen on (tracker pointer, 3/4 factor) loaded into a session with it off:
    // the session keeps its own settings. Before the fix b got a's pointer and RTPS crashed on it.
    CpuContext c;
    a.GetGTE().SetWidescreen(3, 4);
    a.GetGTE().SetProjectionTracker(reinterpret_cast<Widescreen2D*>(uintptr_t(16)));  // never dereferenced
    RoundTrip(a, b);
    RoundTrip(a, c);
    c.GetGTE().SetWidescreen(1, 1);  // reference: same registers, widescreen off
    b.GetGTE().ExecuteCommand(0x00180001);  // RTPS: would dereference a's pointer
    c.GetGTE().ExecuteCommand(0x00180001);
    CHECK(b.GetGTE().ReadDataRegister(14) == c.GetGTE().ReadDataRegister(14));  // SXY2 unscaled
}

static void TestGpu() {
    Ps1Gpu a, b;
    a.WriteGP1(0x05000000 | 64 | (32 << 10));  // display start
    a.WriteGP0(0x02112233);                     // fill rect
    a.WriteGP0(0x00100010);
    a.WriteGP0(0x00200040);
    a.WriteGP0(0xA0000000);                     // CPU -> VRAM 4x2 at (100,50), stopped half way
    a.WriteGP0(0x00320064);
    a.WriteGP0(0x00020004);
    a.WriteGP0(0x11112222);
    a.WriteGP0(0x33334444);
    RoundTrip(a, b);
    CHECK(b.ReadGPUSTAT() == a.ReadGPUSTAT() && b.GetDisplayArea().x == 64 && b.GetVram() == a.GetVram());
    for (Ps1Gpu* g : {&a, &b}) { g->WriteGP0(0x55556666); g->WriteGP0(0x77778888); }  // finish the upload
    CHECK(b.GetVram() == a.GetVram() && b.ReadVram(103, 51) == 0x7777 && Save(a) == Save(b));
}

static void TestSpu() {
    SpuEmulator a, b;
    std::vector<uint8_t> block(16 * 8, 0);
    for (size_t i = 0; i < block.size(); i += 16) { block[i] = 0x10; for (int k = 2; k < 16; ++k) block[i + k] = uint8_t(i * 7 + k * 13); }
    block[block.size() - 16 + 1] = 0x03;  // last block: end + repeat
    a.UploadToRam(0x1000, block.data(), block.size());
    a.SetVoiceStartAddress(3, 0x1000);
    a.SetVoicePitch(3, 0x0800);
    a.SetVoiceVolume(3, 0x3000, 0x2000);
    a.SetMasterVolume(0x3FFF, 0x3FFF);
    a.Write16(0x1F801C00 + 3 * 16 + 8, 0x00FF);  // ADSR1: fast attack
    a.Write16(0x1F801DAA, 0xC000);               // SPUCNT: on, unmute
    a.KeyOn(1u << 3);
    const int16_t cd[8] = {100, -100, 200, -200, 300, -300, 400, -400};
    a.PushCdAudio(cd, 4);
    a.Tick(768 * 37 + 100);  // mid-sample
    std::vector<int16_t> out_a, out_b;
    a.TakeAudio(out_a);
    RoundTrip(a, b);
    CHECK(b.GetActiveVoiceMask() == a.GetActiveVoiceMask() && b.CdAudioQueued() == a.CdAudioQueued());
    a.Tick(768 * 200);
    b.Tick(768 * 200);
    a.TakeAudio(out_a);
    b.TakeAudio(out_b);
    CHECK(!out_a.empty() && out_a == out_b && Save(a) == Save(b));
}

struct PatternDisc : hw::SectorSource {
    bool ReadSector(uint32_t lba, uint8_t* out) override {
        for (uint32_t i = 0; i < 2352; ++i) out[i] = uint8_t(lba * 31 + i);
        return true;
    }
};

static void TestCdRom() {
    PatternDisc disc;
    hw::CdRom a(&disc), b(&disc);
    int irqs_a = 0, irqs_b = 0;
    a.SetIrqCallback([&] { ++irqs_a; });
    b.SetIrqCallback([&] { ++irqs_b; });
    auto cmd = [](hw::CdRom& c, uint8_t op, std::vector<uint8_t> params) {
        c.Write8(0, 0);
        for (uint8_t p : params) c.Write8(2, p);
        c.Write8(1, op);
    };
    auto ack = [](hw::CdRom& c) { c.Write8(0, 1); c.Write8(3, 0x1F); c.Write8(0, 0); };
    a.Write8(0, 1);
    a.Write8(2, 0x1F);  // interrupt enable
    cmd(a, 0x02, {0x00, 0x02, 0x16});  // Setloc 00:02:16
    a.Tick(200000);
    ack(a);
    cmd(a, 0x06, {});  // ReadN: reading, events queued, sector timer running
    a.Tick(300000);
    RoundTrip(a, b);
    for (int i = 0; i < 6; ++i) {  // same stimulus: acks, ticks, data reads
        for (hw::CdRom* c : {&a, &b}) {
            ack(*c);
            c->Write8(0, 0);
            c->Write8(3, 0x80);  // BFRD: sector -> data FIFO
            c->Tick(250000);
        }
        CHECK(a.DmaRead32() == b.DmaRead32() && a.Read8(1) == b.Read8(1) && a.Stat() == b.Stat());
    }
    CHECK(Save(a) == Save(b) && irqs_a > 0);
}

struct WordRam : hw::DmaMemory {
    std::vector<uint32_t> w = std::vector<uint32_t>(0x1000);
    uint32_t ReadWord(uint32_t a) override { return w[(a >> 2) & 0xFFF]; }
    void WriteWord(uint32_t a, uint32_t v) override { w[(a >> 2) & 0xFFF] = v; }
};

static void TestDma() {
    WordRam ma, mb;
    hw::Dma a(ma), b(mb);
    a.Write32(0x70, 0x0F654321);       // DPCR: OTC enabled
    a.Write32(0x74, 0x00FF8000);       // DICR: IRQ enable for all
    a.Write32(0x60, 0x400);            // OTC MADR
    a.Write32(0x64, 0x100);            // 256 words
    a.Write32(0x68, 0x11000002);       // start: completion pending
    a.Tick(10);
    mb.w = ma.w;
    RoundTrip(a, b);
    CHECK(b.Busy(hw::Dma::kOtc) == a.Busy(hw::Dma::kOtc) && a.Busy(hw::Dma::kOtc));
    a.Tick(1000);
    b.Tick(1000);
    CHECK(!b.Busy(hw::Dma::kOtc) && b.IrqLine() == a.IrqLine() && b.Read32(0x74) == a.Read32(0x74) && Save(a) == Save(b));
}

static void TestMdec() {
    hw::Mdec a, b;
    a.Write32(4, 0x60000000);           // enable DMA in/out
    a.Write32(0, 0x40000001);           // set quant tables (luma + chroma), 32 words
    for (uint32_t i = 0; i < 32; ++i) a.Write32(0, 0x01010101u * (i + 1));
    a.Write32(0, 0x30000000 | 64);       // decode colour macroblock, 64 halfword pairs
    for (uint32_t i = 0; i < 20; ++i) a.Write32(0, 0xFE00FE00u);  // part of the stream
    a.Tick(100);
    RoundTrip(a, b);
    for (hw::Mdec* m : {&a, &b}) {
        for (uint32_t i = 0; i < 44; ++i) m->Write32(0, 0xFE00FE00u);
        m->Tick(448 * 3);
    }
    CHECK(a.Status() == b.Status() && a.ReadyWords() == b.ReadyWords());
    while (a.ReadyWords()) CHECK(a.Read32(0) == b.Read32(0));
    CHECK(Save(a) == Save(b));
}

static void TestMemoryCardProtocol() {
    MemoryCard a, b;
    uint8_t rx = 0;
    a.Select();
    for (uint8_t t : std::vector<uint8_t>{0x81, 'R', 0, 0, 0x00}) a.Exchange(t, rx);  // read command, before the sector LSB
    std::vector<uint8_t> s;
    { StateIO io(s); a.SerializeProtocol(io); }
    { StateIO io(s.data(), s.size()); b.SerializeProtocol(io); CHECK(io.Ok() && io.Remaining() == 0); }
    for (uint8_t t : std::vector<uint8_t>{0x07, 0, 0, 0, 0}) {
        uint8_t ra = 0, rb = 0;
        CHECK(a.Exchange(t, ra) == b.Exchange(t, rb) && ra == rb);
    }
}

// A guest that keeps state in RAM (a counter) and reads timer 0 every pass.
static Engine& BootLoop() {
    auto& e = Engine::Instance();
    EngineBootConfig config;
    config.enable_bios_hle = true;
    e.Initialize(config);
    std::vector<uint8_t> exe(4096, 0);
    auto* hdr = reinterpret_cast<PsxExeHeader*>(exe.data());
    std::memcpy(hdr->magic, "PS-X EXE", 8);
    hdr->initial_pc = 0x80010000;
    hdr->destination_ram = 0x80010000;
    hdr->file_size = 2048;
    const uint32_t code[] = {
        0x3C048001,  // lui a0, 0x8001
        0x8C880100,  // loop: lw t0, 0x100(a0)
        0x00000000,
        0x25080001,  // addiu t0, t0, 1
        0xAC880100,  // sw t0, 0x100(a0)
        0x3C091F80,  // lui t1, 0x1F80
        0x8D2A1100,  // lw t2, 0x1100(t1)   timer 0
        0x00000000,
        0xAC8A0104,  // sw t2, 0x104(a0)
        0x08004001,  // j loop
        0x00000000,
    };
    std::memcpy(exe.data() + 2048, code, sizeof code);
    CHECK(e.BootExecutable(exe.data(), exe.size()));
    return e;
}

static void TestEngine() {
    Engine& e = BootLoop();
    for (int i = 0; i < 3; ++i) e.RunFrame();
    std::vector<uint8_t> state;
    e.SaveState(state);
    CHECK(state.size() > PS1_MAIN_RAM_SIZE && std::memcmp(state.data(), "PRMSTATE", 8) == 0);
    for (int i = 0; i < 4; ++i) e.RunFrame();
    const std::vector<uint8_t> ram1 = e.GetRam();
    const CpuRegisters regs1 = e.GetCpu().GetRegisters();
    const uint64_t cycles1 = e.GetCycles(), frame1 = e.GetFrameCount();
    std::vector<uint8_t> after;
    e.SaveState(after);

    std::string err;
    CHECK(e.LoadState(state, &err) && e.GetFrameCount() == 3);
    for (int i = 0; i < 4; ++i) e.RunFrame();
    CHECK(e.GetRam() == ram1 && e.GetCpu().GetRegisters().gpr == regs1.gpr && e.GetCycles() == cycles1 &&
          e.GetFrameCount() == frame1);
    std::vector<uint8_t> again;
    e.SaveState(again);
    CHECK(again == after);  // the whole machine, not just RAM

    // Rejections leave the machine untouched.
    auto rejected = [&](std::vector<uint8_t> bad) {
        std::string why;
        CHECK(!e.LoadState(bad, &why) && !why.empty());
        std::vector<uint8_t> now;
        e.SaveState(now);
        CHECK(now == after);
    };
    std::vector<uint8_t> bad = state;
    bad[0] ^= 1;  // magic
    rejected(bad);
    bad = state;
    bad[8] ^= 1;  // version
    rejected(bad);
    {  // another build of the same format version loads (warning only)
        std::vector<uint8_t> other = state;
        other[8 + 4 + 8 + e.DiscSerial().size() + 8] ^= 1;  // first byte of the build id
        std::string why;
        CHECK(e.LoadState(other, &why) && e.GetFrameCount() == 3);
        CHECK(e.LoadState(after, &why) && e.GetFrameCount() == frame1);
    }
    bad = state;
    bad[8 + 4] ^= 1;  // serial length: another disc (or garbage)
    rejected(bad);
    bad = state;
    bad.resize(bad.size() / 2);  // truncated body
    rejected(bad);
    bad = state;
    bad.push_back(0);  // trailing garbage
    rejected(bad);
    rejected({});
    e.Shutdown();
}

int main() {
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"StateIO", TestStateIO}, {"Cpu", TestCpu}, {"Gpu", TestGpu}, {"Spu", TestSpu}, {"CdRom", TestCdRom},
        {"Dma", TestDma}, {"Mdec", TestMdec}, {"MemoryCardProtocol", TestMemoryCardProtocol}, {"Engine", TestEngine},
    };
    for (const auto& t : tests) {
        std::fprintf(stderr, "%s\n", t.name);
        t.fn();
    }
    std::printf("save state tests passed\n");
    return 0;
}
