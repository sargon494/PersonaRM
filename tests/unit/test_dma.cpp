// DMA controller tests (psx-spx "DMA Channels"). Synthetic RAM only.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#include "hw/dma.h"

using namespace personarm::hw;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

struct Ram : DmaMemory {
    std::vector<uint32_t> w = std::vector<uint32_t>(0x80000);
    uint32_t ReadWord(uint32_t a) override { return w[(a & 0x1FFFFC) >> 2]; }
    void WriteWord(uint32_t a, uint32_t v) override { w[(a & 0x1FFFFC) >> 2] = v; }
    uint32_t& at(uint32_t a) { return w[a >> 2]; }
};

static uint32_t Reg(int ch, uint32_t r) { return 0x10u * ch + r; }

static void TestOtc() {
    Ram ram;
    Dma dma(ram);
    dma.Write32(0x70, 0x08000000);  // enable DMA6
    dma.Write32(Reg(6, 0), 0x80010000 + 4 * 7);  // OT of 8 entries, MADR = last entry
    dma.Write32(Reg(6, 4), 8);
    dma.Write32(Reg(6, 8), 0x11000002);
    CHECK(dma.Read32(Reg(6, 8)) == 0x01000002);  // trigger cleared, busy until Tick
    dma.Tick(100);
    CHECK(dma.Read32(Reg(6, 8)) == 0x00000002);
    for (uint32_t i = 7; i >= 1; --i) CHECK(ram.at(0x10000 + 4 * i) == 0x10000 + 4 * (i - 1));
    CHECK(ram.at(0x10000) == 0x00FFFFFF);
    // Walking from the top reaches the end marker after 8 nodes.
    uint32_t a = 0x1001C, n = 1;
    while (ram.at(a) != 0x00FFFFFF) { a = ram.at(a); ++n; }
    CHECK(n == 8);
}

static void TestGpuLinkedList() {
    Ram ram;
    Dma dma(ram);
    std::vector<uint32_t> gp0;
    dma.SetPort(Dma::kGpu, {nullptr, [&](uint32_t v) { gp0.push_back(v); }});
    // 0x100: 2 words -> 0x300; 0x300: 0 words -> 0x200; 0x200: 1 word -> end.
    ram.at(0x100) = 0x02000300; ram.at(0x104) = 0xA; ram.at(0x108) = 0xB;
    ram.at(0x300) = 0x00000200;
    ram.at(0x200) = 0x01FFFFFF; ram.at(0x204) = 0xC;
    ram.at(0x208) = 0xDEAD;  // never sent
    dma.Write32(0x70, 0x00000800);
    dma.Write32(Reg(2, 0), 0x100);
    dma.Write32(Reg(2, 8), 0x01000401);  // RAM->GPU, linked list, start
    dma.Tick(100);
    CHECK((gp0 == std::vector<uint32_t>{0xA, 0xB, 0xC}));
    CHECK(dma.Read32(Reg(2, 0)) == 0x00FFFFFF);
    CHECK(!dma.Busy(Dma::kGpu));
}

static void TestBlockToDevice() {
    Ram ram;
    Dma dma(ram);
    std::vector<uint32_t> spu;
    dma.SetPort(Dma::kSpu, {nullptr, [&](uint32_t v) { spu.push_back(v); }});
    for (uint32_t i = 0; i < 32; ++i) ram.at(0x4000 + 4 * i) = i;
    dma.Write32(0x70, 0x00080000);
    dma.Write32(Reg(4, 0), 0x4000);
    dma.Write32(Reg(4, 4), 0x00020010);  // 2 blocks of 16 words
    dma.Write32(Reg(4, 8), 0x01000201);  // slice mode, start (no trigger needed)
    CHECK(spu.size() == 32 && spu[31] == 31);
    CHECK(dma.Read32(Reg(4, 0)) == 0x4080);
    CHECK((dma.Read32(Reg(4, 4)) >> 16) == 0);
}

static void TestBurstNeedsTriggerAndDpcr() {
    Ram ram;
    Dma dma(ram);
    int reads = 0;
    dma.SetPort(Dma::kCdrom, {[&] { return static_cast<uint32_t>(++reads); }, nullptr});
    dma.Write32(Reg(3, 4), 4);
    dma.Write32(Reg(3, 8), 0x01000000);  // start without trigger: waits
    CHECK(reads == 0);
    dma.Write32(Reg(3, 8), 0x11000000);  // DPCR reset 0x07654321: channel 3 not enabled
    CHECK(reads == 0);
    dma.Write32(0x70, 0x0000F000);       // enabling DPCR starts it
    CHECK(reads == 4 && ram.at(0) == 1 && ram.at(12) == 4);
}

// MDEC-style DRQ pacing: DMA1 (out) is started before DMA0 (in) and must wait for data.
static void TestRequestPacedOutBeforeIn() {
    Ram ram;
    Dma dma(ram);
    std::deque<uint32_t> fifo;
    bool in_open = true;
    dma.SetPort(Dma::kMdecIn, {nullptr, [&](uint32_t v) { fifo.push_back(v * 10); }, [&] { return in_open; }});
    dma.SetPort(Dma::kMdecOut, {[&] { uint32_t v = fifo.front(); fifo.pop_front(); return v; }, nullptr,
                                [&] { return fifo.size() >= 4; }});
    for (uint32_t i = 0; i < 8; ++i) ram.at(0x1000 + 4 * i) = i + 1;
    dma.Write32(0x70, 0x000000BB);  // enable DMA0 and DMA1
    dma.Write32(Reg(1, 0), 0x2000);
    dma.Write32(Reg(1, 4), 0x00020004);  // 2 blocks of 4 words
    dma.Write32(Reg(1, 8), 0x01000200);  // MDEC -> RAM, slice mode
    dma.Tick(100);
    CHECK(dma.Busy(Dma::kMdecOut) && ram.at(0x2000) == 0);  // no data yet: nothing moved
    in_open = false;
    dma.Write32(Reg(0, 0), 0x1000);
    dma.Write32(Reg(0, 4), 0x00020004);
    dma.Write32(Reg(0, 8), 0x01000201);  // RAM -> MDEC
    dma.Tick(1);
    CHECK(fifo.empty() && dma.Busy(Dma::kMdecIn));  // DRQ low: waits
    in_open = true;
    dma.Tick(1);  // DMA0 moves both blocks, then DMA1 drains them
    dma.Tick(1);
    CHECK(!dma.Busy(Dma::kMdecIn) && !dma.Busy(Dma::kMdecOut));
    for (uint32_t i = 0; i < 8; ++i) CHECK(ram.at(0x2000 + 4 * i) == (i + 1) * 10);
    CHECK(dma.Read32(Reg(1, 0)) == 0x2020 && (dma.Read32(Reg(1, 4)) >> 16) == 0);
}

static void TestDicr() {
    Ram ram;
    Dma dma(ram);
    int irqs = 0;
    dma.SetIrqCallback([&] { ++irqs; });
    dma.Write32(0x70, 0x08000000);
    auto run_otc = [&] {
        dma.Write32(Reg(6, 0), 0x100);
        dma.Write32(Reg(6, 4), 1);
        dma.Write32(Reg(6, 8), 0x11000002);
        dma.Tick(10);
    };
    // Channel enable off: no flag, no IRQ.
    run_otc();
    CHECK(dma.Read32(0x74) == 0 && irqs == 0);
    // Enable ch6 but master enable off: flag set, no master flag.
    dma.Write32(0x74, 1u << 22);
    run_otc();
    CHECK(dma.Read32(0x74) & (1u << 30));
    CHECK(!dma.IrqLine() && irqs == 0);
    // Master enable on with a pending enabled flag raises bit 31 and IRQ3 once.
    dma.Write32(0x74, (1u << 22) | (1u << 23));
    CHECK(dma.IrqLine() && irqs == 1);
    // Writing 0 to the flag keeps it; writing 1 acknowledges it and drops the master flag.
    dma.Write32(0x74, (1u << 22) | (1u << 23));
    CHECK(dma.IrqLine());
    dma.Write32(0x74, (1u << 22) | (1u << 23) | (1u << 30));
    CHECK(!dma.IrqLine() && !(dma.Read32(0x74) & (1u << 30)));
    // Next completion raises IRQ again (edge).
    run_otc();
    CHECK(dma.IrqLine() && irqs == 2);
    // Byte write to the enable byte does not acknowledge flags.
    dma.Write8(0x76, 0xC0);
    CHECK(dma.IrqLine());
    // Force bit 15 sets the master flag regardless of enables.
    dma.Write32(0x74, 1u << 30);
    CHECK(!dma.IrqLine());
    dma.Write32(0x74, 0x8000);
    CHECK(dma.IrqLine() && irqs == 3);
    CHECK(dma.Read16(0x74) == 0x8000);
}

int main() {
    TestOtc();
    TestGpuLinkedList();
    TestBlockToDevice();
    TestBurstNeedsTriggerAndDpcr();
    TestDicr();
    TestRequestPacedOutBeforeIn();
    std::printf("test_dma: OK\n");
    return 0;
}
