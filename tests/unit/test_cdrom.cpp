// CD-ROM controller tests (psx-spx "CDROM Controller I/O Ports" / "Command Summary").
// Synthetic sectors only, no game data.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hw/cdrom.h"
#include "hw/dma.h"

using namespace personarm::hw;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static uint8_t DataByte(uint32_t lba, uint32_t i) { return static_cast<uint8_t>(lba * 7 + i); }

class FakeSource : public SectorSource {
public:
    bool ReadSector(uint32_t lba, uint8_t* out) override {
        if (lba >= 100) return false;
        std::memset(out, 0, CdRom::kSectorBytes);
        std::memset(out + 1, 0xFF, 10);
        out[12] = 0x00; out[13] = 0x02; out[14] = static_cast<uint8_t>(lba);  // fake MSF (not BCD-checked)
        out[15] = 2;
        out[16] = 1; out[17] = static_cast<uint8_t>(lba & 1); out[18] = (lba >= 50) ? 0x64 : 0x08; out[19] = 0;
        for (uint32_t i = 0; i < 0x800; ++i) out[24 + i] = DataByte(lba, i);
        return true;
    }
};

static void Command(CdRom& cd, uint8_t cmd, std::vector<uint8_t> params = {}) {
    cd.Write8(0, 0);
    for (uint8_t p : params) cd.Write8(2, p);
    cd.Write8(1, cmd);
}

static uint8_t Flags(CdRom& cd) { cd.Write8(0, 1); return cd.Read8(3) & 0x1F; }

// Ticks until an interrupt is raised; returns its type (1..5) or 0 on timeout.
static uint8_t WaitIrq(CdRom& cd, uint32_t max_cycles = 4000000) {
    for (uint32_t t = 0; t < max_cycles; t += 1000) {
        cd.Tick(1000);
        if (uint8_t f = Flags(cd) & 7) return f;
    }
    return 0;
}

static std::vector<uint8_t> Response(CdRom& cd) {
    std::vector<uint8_t> r;
    while (cd.Read8(0) & 0x20) r.push_back(cd.Read8(1));
    return r;
}

static void Ack(CdRom& cd) { cd.Write8(0, 1); cd.Write8(3, 0x1F); }

static void EnableIrqs(CdRom& cd) { cd.Write8(0, 1); cd.Write8(2, 0x1F); }

static void TestReadSequence() {
    FakeSource src;
    CdRom cd(&src);
    int irqs = 0;
    cd.SetIrqCallback([&] { ++irqs; });
    EnableIrqs(cd);

    Command(cd, 0x01);  // Getstat
    CHECK(cd.Read8(0) & 0x80);  // busy until first response
    CHECK(WaitIrq(cd) == 3);
    CHECK(Response(cd) == std::vector<uint8_t>{0x02});
    CHECK(irqs == 1);
    Ack(cd);
    CHECK(Flags(cd) == 0);
    CHECK((cd.Read8(0) & 0x80) == 0);

    Command(cd, 0x02, {0x00, 0x02, 0x16});  // Setloc 00:02:16 -> LBA 16
    CHECK(WaitIrq(cd) == 3);
    Ack(cd);

    Command(cd, 0x06);  // ReadN
    CHECK(WaitIrq(cd) == 3);
    CHECK(Response(cd)[0] & 0x20);  // reading
    Ack(cd);

    // Sector 16 through the data register.
    CHECK(WaitIrq(cd) == 1);
    CHECK(Response(cd)[0] == 0x22);
    cd.Write8(0, 0); cd.Write8(3, 0x80);  // BFRD
    CHECK(cd.DataBytesRemaining() == 0x800);
    CHECK(cd.Read8(0) & 0x40);
    for (uint32_t i = 0; i < 0x800; ++i) CHECK(cd.Read8(2) == DataByte(16, i));
    CHECK((cd.Read8(0) & 0x40) == 0);
    Ack(cd);

    // Sector 17 through DMA3 (block transfer into RAM).
    CHECK(WaitIrq(cd) == 1);
    cd.Write8(0, 0); cd.Write8(3, 0x80);
    struct Ram : DmaMemory {
        std::vector<uint32_t> w = std::vector<uint32_t>(0x80000);
        uint32_t ReadWord(uint32_t a) override { return w[(a & 0x1FFFFC) >> 2]; }
        void WriteWord(uint32_t a, uint32_t v) override { w[(a & 0x1FFFFC) >> 2] = v; }
    } ram;
    Dma dma(ram);
    dma.SetPort(Dma::kCdrom, {[&] { return cd.DmaRead32(); }, nullptr});
    dma.Write32(0x70, 0x0000F000);  // enable DMA3
    dma.Write32(0x30, 0x1000);
    dma.Write32(0x34, 0x200);       // 512 words
    dma.Write32(0x38, 0x11000000);  // to RAM, burst, start+trigger
    dma.Tick(1000);
    CHECK(!dma.Busy(Dma::kCdrom));
    for (uint32_t i = 0; i < 0x800; ++i)
        CHECK(static_cast<uint8_t>(ram.w[(0x1000 + i) / 4] >> ((i & 3) * 8)) == DataByte(17, i));
    Ack(cd);

    // Pause stops further INT1s.
    Command(cd, 0x09);
    CHECK(WaitIrq(cd) == 3);
    Ack(cd);
    CHECK(WaitIrq(cd, 8000000) == 2);
    CHECK((Response(cd)[0] & 0x20) == 0);
    Ack(cd);
    CHECK(WaitIrq(cd, 2000000) == 0);
}

static void TestSetmodeSectorSize() {
    FakeSource src;
    CdRom cd(&src);
    Command(cd, 0x0E, {0xA0});  // 2x, whole sector
    CHECK(WaitIrq(cd) == 3);
    Ack(cd);
    CHECK(cd.Mode() == 0xA0);
    Command(cd, 0x02, {0x00, 0x02, 0x20});  // LBA 20
    WaitIrq(cd); Ack(cd);
    Command(cd, 0x1B);  // ReadS
    WaitIrq(cd); Ack(cd);
    CHECK(WaitIrq(cd, 2 * CdRom::kCyclesPerSector2x + 20000) == 1);  // 2x pacing
    cd.Write8(0, 0); cd.Write8(3, 0x80);
    CHECK(cd.DataBytesRemaining() == 0x924);
    CHECK(cd.Read8(2) == 0x00);  // header MSF begins the 0x924 payload
    CHECK(cd.Read8(2) == 0x02);
    CHECK(cd.Read8(2) == 20);
    CHECK(cd.Read8(2) == 2);     // mode
    for (int i = 0; i < 8; ++i) cd.Read8(2);  // subheader x2
    CHECK(cd.Read8(2) == DataByte(20, 0));
    Ack(cd);

    // GetlocL returns header + subheader of the delivered sector.
    Command(cd, 0x10);
    CHECK(WaitIrq(cd) == 3);
    auto r = Response(cd);
    CHECK(r.size() == 8 && r[2] == 20 && r[3] == 2 && r[6] == 0x08);
}

static void TestXaFilter() {
    FakeSource src;
    CdRom cd(&src);
    int xa = 0;
    cd.SetXaCallback([&](const uint8_t* s) { CHECK(s[17] == 1); ++xa; });
    Command(cd, 0x0D, {1, 1}); WaitIrq(cd); Ack(cd);       // Setfilter file 1 ch 1
    Command(cd, 0x0E, {0xC8}); WaitIrq(cd); Ack(cd);       // 2x, XA-ADPCM, filter
    Command(cd, 0x02, {0x00, 0x02, 0x50}); WaitIrq(cd); Ack(cd);  // LBA 50.. (audio sectors)
    Command(cd, 0x06); WaitIrq(cd); Ack(cd);
    CHECK(WaitIrq(cd, 10 * CdRom::kCyclesPerSector2x) == 0);  // audio never reaches the host
    CHECK(xa >= 3);
}

static void TestGetIdTestAndErrors() {
    FakeSource src;
    CdRom cd(&src);
    Command(cd, 0x1A);
    CHECK(WaitIrq(cd) == 3);
    Ack(cd);
    CHECK(WaitIrq(cd) == 2);
    CHECK((Response(cd) == std::vector<uint8_t>{0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I'}));
    Ack(cd);

    Command(cd, 0x19, {0x20});
    CHECK(WaitIrq(cd) == 3);
    CHECK(Response(cd).size() == 4);
    Ack(cd);

    Command(cd, 0x02, {0x00});  // Setloc with wrong param count
    CHECK(WaitIrq(cd) == 5);
    CHECK((Response(cd) == std::vector<uint8_t>{0x03, 0x20}));
    Ack(cd);

    Command(cd, 0x5F);  // unknown
    CHECK(WaitIrq(cd) == 5);
    CHECK(Response(cd)[1] == 0x40);
    Ack(cd);

    CdRom empty;
    Command(empty, 0x1A);
    CHECK(WaitIrq(empty) == 3);
    Ack(empty);
    CHECK(WaitIrq(empty) == 5);
    CHECK(Response(empty)[1] == 0x40);
}

static void TestIrqEnableMask() {
    FakeSource src;
    CdRom cd(&src);
    int irqs = 0;
    cd.SetIrqCallback([&] { ++irqs; });
    Command(cd, 0x01);
    CHECK(WaitIrq(cd) == 3);   // flag set even when masked
    CHECK(!cd.IrqPending() && irqs == 0);
    EnableIrqs(cd);
    CHECK(cd.IrqPending());
    cd.Write8(0, 1);
    CHECK(cd.Read8(3) == (0xE0 | 3));
    Ack(cd);
    CHECK(!cd.IrqPending());
    cd.Write8(0, 0);
    CHECK(cd.Read8(3) == (0xE0 | 0x1F));
}

int main() {
    TestReadSequence();
    TestSetmodeSectorSize();
    TestXaFilter();
    TestGetIdTestAndErrors();
    TestIrqEnableMask();
    std::printf("test_cdrom: OK\n");
    return 0;
}
