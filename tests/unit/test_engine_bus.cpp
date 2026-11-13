// Engine bus routing: RAM mirrors, DMA (ch 2/3/4/6), CD-ROM, SPU and GPU registers, Timer1 Hblank,
// post-BIOS CD state and SYSTEM.CNF STACK. Synthetic data only (psx-spx "Memory Map", "DMA Channels",
// "CDROM Controller I/O Ports", "SPU Control and Status Register", "Timers").
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/engine.h"
#include "loader/exe_loader.h"

using personarm::Engine;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// Boots a 2-instruction spin loop at 0x80010000 with the HLE BIOS (fresh devices, kernel installed).
static Engine& Boot() {
    auto& e = Engine::Instance();
    personarm::EngineBootConfig config;
    config.enable_bios_hle = true;
    e.Initialize(config);
    std::vector<uint8_t> exe(4096, 0);
    auto* hdr = reinterpret_cast<personarm::PsxExeHeader*>(exe.data());
    std::memcpy(hdr->magic, "PS-X EXE", 8);
    hdr->initial_pc = 0x80010000;
    hdr->destination_ram = 0x80010000;
    hdr->file_size = 2048;
    const uint32_t spin[2] = {0x08004000, 0};  // j 0x80010000; nop
    std::memcpy(exe.data() + 2048, spin, sizeof spin);
    CHECK(e.BootExecutable(exe.data(), exe.size()));
    return e;
}

static void TestRamMirrors(Engine& e) {
    e.Write(0x00001230, 0xCAFEBABE, 4);
    CHECK(e.Read(0x80001230, 4) == 0xCAFEBABE);
    CHECK(e.Read(0xA0201230, 4) == 0xCAFEBABE);  // 2 MB mirror in KSEG1
    CHECK(e.Read(0x80601230, 4) == 0xCAFEBABE);  // last mirror of the 8 MB window
    e.Write(0x807FFFF8, 0x11223344, 4);           // crt0 stack (MEMORY_MAP MM-014)
    CHECK(e.Read(0x801FFFF8, 4) == 0x11223344);
    CHECK(e.Read(0x801FFFFA, 2) == 0x1122);
    CHECK(e.Read(0x801FFFFB, 1) == 0x11);
    // ExeLoader without STACK/header SP: BIOS default 801FFFF0h.
    CHECK(e.GetCpu().GetGPR(29) == 0x801FFFF0);
}

static void TestDmaOtcAndRegs(Engine& e) {
    e.Write(0x1F8010F0, 0x08000000, 4);  // DPCR: enable DMA6
    CHECK(e.Read(0x1F8010F0, 4) == 0x08000000);
    CHECK(e.Read(0x1F8010F2, 2) == 0x0800);
    e.Write(0x1F8010E0, 0x80100000 + 4 * 3, 4);  // OT of 4 entries at 0x100000
    e.Write(0x1F8010E4, 4, 4);
    e.Write(0x1F8010E8, 0x11000002, 4);
    for (int i = 0; i < 100; ++i) e.GetDma().Tick(1);
    CHECK(e.Read(0x8010000C, 4) == 0x00100008);
    CHECK(e.Read(0x80100000, 4) == 0x00FFFFFF);
    CHECK((e.Read(0x1F8010E8, 4) & 0x01000000) == 0);
}

static void TestDmaGpuLinkedListAndIrq(Engine& e) {
    // One node: monochrome triangle (GP0 20h) -> end marker.
    e.Write(0x80100100, 0x04FFFFFF, 4);
    e.Write(0x80100104, 0x200000FF, 4);
    e.Write(0x80100108, 0x00000000, 4);
    e.Write(0x8010010C, 0x00000010, 4);
    e.Write(0x80100110, 0x00100000, 4);
    e.Write(0x1F801074, 0x8, 4);              // I_MASK: DMA
    e.Write(0x1F8010F4, 0x00840000, 4);       // DICR: master enable + ch2 enable
    e.Write(0x1F8010F0, 0x00000800, 4);       // DPCR: enable DMA2
    const uint64_t polys = e.GetGpu().GetPolygonCount();
    e.Write(0x1F8010A0, 0x00100100, 4);
    e.Write(0x1F8010A8, 0x01000401, 4);       // RAM->GPU, linked list, start
    e.GetDma().Tick(100);
    CHECK(e.GetGpu().GetPolygonCount() == polys + 1);
    CHECK(e.Read(0x1F8010A0, 4) == 0x00FFFFFF);
    CHECK(e.Read(0x1F801070, 4) & 0x8);       // IRQ3 raised into I_STAT
    CHECK(e.Read(0x1F8010F4, 4) & 0x84000000);
    e.Write(0x1F801070, ~0x8u, 4);
}

static void TestGpuPorts(Engine& e) {
    e.Write(0x1F801814, 0x00000000, 4);  // GP1(00h) reset
    CHECK(e.Read(0x1F801814, 4) & (1u << 26));
    // CPU->VRAM 1x1 pixel, then VRAM->CPU via GPUREAD.
    e.Write(0x1F801810, 0xA0000000, 4);
    e.Write(0x1F801810, 0x00000000, 4);
    e.Write(0x1F801810, 0x00010001, 4);
    e.Write(0x1F801810, 0x00001234, 4);
    e.Write(0x1F801810, 0xC0000000, 4);
    e.Write(0x1F801810, 0x00000000, 4);
    e.Write(0x1F801810, 0x00010001, 4);
    CHECK((e.Read(0x1F801810, 4) & 0xFFFF) == 0x1234);
}

static void TestCdRom(Engine& e) {
    // HLE boot leaves HINTMSK = 1Fh like the BIOS (libcd CD_init never writes it).
    e.Write(0x1F801800, 1, 1);
    CHECK((e.Read(0x1F801803, 1) & 0x1F) == 0);
    e.Write(0x1F801800, 0, 1);
    CHECK((e.Read(0x1F801800, 1) & 0x18) == 0x18);  // param FIFO empty + writable
    e.Write(0x1F801801, 0x01, 1);                   // Getstat
    CHECK(e.Read(0x1F801800, 1) & 0x80);             // busy
    e.GetCdRom().Tick(personarm::hw::CdRom::kFirstResponseCycles);
    CHECK(e.Read(0x1F801070, 4) & 0x4);              // IRQ2 in I_STAT
    CHECK(e.Read(0x1F801800, 1) & 0x20);             // response ready
    e.Read(0x1F801801, 1);
    e.Write(0x1F801800, 1, 1);
    CHECK((e.Read(0x1F801803, 1) & 7) == 3);         // INT3
    e.Write(0x1F801803, 0x1F, 1);
    e.Write(0x1F801800, 0, 1);
    e.Write(0x1F801070, ~0x4u, 4);
}

static void TestSpu(Engine& e) {
    e.Write(0x1F801D80, 0x3FFF, 2);                  // main volume L
    CHECK(e.Read(0x1F801D80, 2) == 0x3FFF);
    e.Write(0x1F801C00, 0x12345678, 4);              // voice 0 vol L/R as one word
    CHECK(e.Read(0x1F801C00, 4) == 0x12345678);
    CHECK(e.Read(0x1F801C03, 1) == 0x12);
    uint32_t kon = 0;
    e.SetSpuKeyOnHook([&](uint32_t m) { kon |= m; });
    e.Write(0x1F801D88, 0x00010003, 4);              // KON voices 0,1 and 16
    CHECK(kon == 0x00010003);
    e.SetSpuKeyOnHook(nullptr);
    // DMA4 RAM->SPU at transfer address 0x1000.
    e.Write(0x80100200, 0xDDCCBBAA, 4);
    e.Write(0x1F801DA6, 0x1000 >> 3, 2);
    e.Write(0x1F8010F0, 0x00080000, 4);
    e.Write(0x1F8010C0, 0x00100200, 4);
    e.Write(0x1F8010C4, 0x00010001, 4);
    e.Write(0x1F8010C8, 0x01000201, 4);
    CHECK(e.GetSpu().ReadRam(0x1000) == 0xAA && e.GetSpu().ReadRam(0x1003) == 0xDD);
}

static void TestTimer1Hblank(Engine& e) {
    e.Write(0x1F801114, 0x100, 4);  // Timer1 source = Hblank (counter reset by the MODE write)
    e.RunFrame();
    const uint32_t v = e.Read(0x1F801110, 4);
    CHECK(v >= 262 && v <= 264);  // 263 lines per frame, not sysclk (libetc VSync() needs this)
}

static void TestSystemCnfStack() {
    using personarm::ParseSystemCnfStack;
    CHECK(ParseSystemCnfStack("BOOT = cdrom:\\SLPS_027.85;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n", 1) == 0x801FFF00);
    CHECK(ParseSystemCnfStack("BOOT = cdrom:\\X;1\r\n", 0x801FFFF0) == 0x801FFFF0);
    CHECK(ParseSystemCnfStack("STACK = zz\r\n", 7) == 7);
}

// Scripted pad (boot harness --press): psx-spx "Controllers" digital pad, buttons active low.
static void TestPadScript(Engine& e) {
    Engine::PadPress p{};
    CHECK(Engine::ParsePadPress("start@10", p) && p.mask == 0x0008 && p.frame == 10 && p.frames == 5);
    CHECK(Engine::ParsePadPress("cross@3+2", p) && p.mask == 0x4000 && p.frame == 3 && p.frames == 2);
    CHECK(!Engine::ParsePadPress("start", p) && !Engine::ParsePadPress("jump@1", p) && !Engine::ParsePadPress("up@x", p));
    auto poll = [&] {
        uint32_t b[5];
        const uint32_t tx[5] = {0x01, 0x42, 0, 0, 0};
        for (int i = 0; i < 5; ++i) { e.Write(0x1F801040, tx[i], 1); b[i] = e.Read(0x1F801040, 1); }
        CHECK(b[1] == 0x41 && b[2] == 0x5A);
        return b[3] | b[4] << 8;
    };
    e.SetPadScript({p, {0x0010, 4, 1}});  // cross frames 3-4, up frame 4
    e.SetFrameCount(2);
    CHECK(poll() == 0xFFFF);
    e.SetFrameCount(4);
    CHECK(poll() == 0xBFEF);
    e.SetFrameCount(5);
    CHECK(poll() == 0xFFFF);
    e.SetPadScript({});
}

int main() {
    Engine& e = Boot();
    TestRamMirrors(e);
    TestDmaOtcAndRegs(e);
    TestDmaGpuLinkedListAndIrq(e);
    TestGpuPorts(e);
    TestCdRom(e);
    TestSpu(e);
    TestTimer1Hblank(e);
    TestSystemCnfStack();
    TestPadScript(e);
    e.Shutdown();
    std::puts("unit_test_engine_bus: all checks passed");
    return 0;
}
