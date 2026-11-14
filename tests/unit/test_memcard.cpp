// Memory card: raw 128 KB image format, SIO0 card protocol (psx-spx "Memory Card Read/Write Commands",
// "Memory Card Data Format"), atomic host writes, and the card on the SIO0 port next to the pad.
// Synthetic data only.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/save_manager.h"
#include "engine/engine.h"
#include "loader/exe_loader.h"

using personarm::Engine;
using personarm::MemoryCard;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static uint8_t Xor(const uint8_t* p, size_t n) { uint8_t x = 0; while (n--) x ^= *p++; return x; }

static void TestFormat() {
    MemoryCard c;
    const auto& d = c.Data();
    CHECK(d.size() == 131072);
    CHECK(d[0] == 'M' && d[1] == 'C' && d[127] == 0x0E);
    for (uint32_t f = 0; f < 36; ++f) CHECK(Xor(c.Frame(f), 127) == c.Frame(f)[127]);  // header, dir, broken list
    for (uint32_t f = 1; f < 16; ++f) CHECK(c.Frame(f)[0] == 0xA0 && c.Frame(f)[127] == 0xA0);
    for (uint32_t f = 16; f < 36; ++f) CHECK(std::memcmp(c.Frame(f), "\xFF\xFF\xFF\xFF", 4) == 0);
    CHECK(std::memcmp(c.Frame(63), c.Frame(0), 128) == 0);
    CHECK(c.Flag() == 0x08);
}

// One command through MemoryCard::Exchange: replies and /ACK per byte.
struct Xfer { std::vector<uint8_t> rx; std::vector<bool> ack; };
static Xfer Run(MemoryCard& c, const std::vector<uint8_t>& tx) {
    Xfer x;
    c.Select();
    for (uint8_t b : tx) { uint8_t r = 0; x.ack.push_back(c.Exchange(b, r)); x.rx.push_back(r); }
    return x;
}

static void TestProtocol() {
    MemoryCard c;
    // Get ID
    Xfer id = Run(c, {0x81, 'S', 0, 0, 0, 0, 0, 0, 0, 0});
    const uint8_t want_id[] = {0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
    CHECK(std::memcmp(id.rx.data(), want_id, sizeof want_id) == 0);
    for (size_t i = 0; i + 1 < id.ack.size(); ++i) CHECK(id.ack[i]);
    CHECK(!id.ack.back());  // last byte: no /ACK

    // Write sector 0x123, then read it back
    uint8_t data[128];
    for (int i = 0; i < 128; ++i) data[i] = uint8_t(i * 7 + 3);
    std::vector<uint8_t> w = {0x81, 'W', 0, 0, 0x01, 0x23};
    w.insert(w.end(), data, data + 128);
    w.push_back(uint8_t(0x01 ^ 0x23 ^ Xor(data, 128)));
    w.insert(w.end(), {0, 0, 0});
    Xfer wr = Run(c, w);
    CHECK(wr.rx[1] == 0x08 && wr.rx[2] == 0x5A && wr.rx[3] == 0x5D);
    CHECK(wr.rx[6 + 128 + 1] == 0x5C && wr.rx[6 + 128 + 2] == 0x5D && wr.rx.back() == 0x47);
    CHECK(!wr.ack.back() && wr.ack[wr.ack.size() - 2]);
    CHECK(std::memcmp(c.Frame(0x123), data, 128) == 0);
    CHECK(c.Flag() == 0x00 && c.Dirty());  // a write clears "new card"

    std::vector<uint8_t> r = {0x81, 'R', 0, 0, 0x01, 0x23};
    r.resize(r.size() + 4 + 128 + 2, 0);
    Xfer rd = Run(c, r);
    CHECK(rd.rx[1] == 0x00 && rd.rx[2] == 0x5A && rd.rx[3] == 0x5D && rd.rx[5] == 0x01);
    CHECK(rd.rx[6] == 0x5C && rd.rx[7] == 0x5D && rd.rx[8] == 0x01 && rd.rx[9] == 0x23);
    CHECK(std::memcmp(&rd.rx[10], data, 128) == 0);
    CHECK(rd.rx[138] == uint8_t(0x01 ^ 0x23 ^ Xor(data, 128)) && rd.rx[139] == 0x47);
    CHECK(rd.ack[138] && !rd.ack[139]);

    // Bad checksum: 4Eh, sector unchanged
    uint8_t other[128];
    std::memset(other, 0x55, 128);
    std::vector<uint8_t> bad = {0x81, 'W', 0, 0, 0x01, 0x23};
    bad.insert(bad.end(), other, other + 128);
    bad.push_back(0x00);
    bad.insert(bad.end(), {0, 0, 0});
    CHECK(Run(c, bad).rx.back() == 0x4E);
    CHECK(std::memcmp(c.Frame(0x123), data, 128) == 0);

    // Bad sector: read confirms FFFFh and aborts (no /ACK, no data); write ends with FFh
    Xfer bs = Run(c, {0x81, 'R', 0, 0, 0x04, 0x00, 0, 0, 0, 0, 0});
    CHECK(bs.rx[8] == 0xFF && bs.rx[9] == 0xFF && !bs.ack[9] && bs.rx[10] == 0xFF);
    std::vector<uint8_t> bw = {0x81, 'W', 0, 0, 0x04, 0x00};
    bw.insert(bw.end(), data, data + 128);
    bw.push_back(uint8_t(0x04 ^ Xor(data, 128)));
    bw.insert(bw.end(), {0, 0, 0});
    CHECK(Run(c, bw).rx.back() == 0xFF);

    // Not addressed (pad address) and unknown command: silent
    Xfer pad = Run(c, {0x01, 0x42});
    CHECK(!pad.ack[0] && pad.rx[1] == 0xFF);
    Xfer unk = Run(c, {0x81, 'X', 0});
    CHECK(unk.ack[0] && !unk.ack[1] && unk.rx[2] == 0xFF);
}

static void TestAtomicFile() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "prm_memcard_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "card.mcd").string();
    {
        MemoryCard c;
        CHECK(c.Open(path));  // missing: created and formatted
        CHECK(fs::file_size(path) == 131072 && !c.Dirty() && !fs::exists(path + ".tmp"));
        uint8_t f[128];
        std::memset(f, 0xAB, 128);
        c.WriteFrame(200, f);
        CHECK(c.Dirty() && c.Flush() && !c.Dirty() && !fs::exists(path + ".tmp"));
        const auto t = fs::last_write_time(path);
        CHECK(c.Flush() && fs::last_write_time(path) == t);  // unchanged: not rewritten
        c.WriteFrame(200, f);                                  // same bytes: still clean
        CHECK(!c.Dirty());
    }
    {
        MemoryCard c;
        CHECK(c.Open(path) && c.Frame(200)[5] == 0xAB && c.Frame(0)[0] == 'M');
    }
    // A wrong-size file is refused and left alone
    const std::string bad = (dir / "bad.mcd").string();
    std::ofstream(bad, std::ios::binary) << "not a card";
    std::string err;
    MemoryCard c;
    CHECK(!c.Open(bad, &err) && !err.empty() && fs::file_size(bad) == 10);
    // An interrupted first write (only .tmp exists) is not formatted over
    const std::string lost = (dir / "lost.mcd").string();
    fs::copy_file(path, lost + ".tmp");
    CHECK(!c.Open(lost, &err) && !fs::exists(lost) && fs::exists(lost + ".tmp"));
    fs::remove_all(dir);
}

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

// SIO0 bus: the pad keeps answering on /JOY1 while a card sits on /JOY2 (and on /JOY1 via 81h).
static void TestSioPort() {
    Engine& e = Boot();
    MemoryCard card1, card2;
    uint8_t mark[128];
    std::memset(mark, 0x3C, 128);
    card2.WriteFrame(5, mark);
    e.SetMemoryCard(1, &card2);
    auto xfer = [&](uint16_t ctrl, const std::vector<uint8_t>& tx) {
        std::vector<uint8_t> rx;
        e.Write(0x1F80104A, ctrl, 2);
        for (uint8_t b : tx) { e.Write(0x1F801040, b, 1); rx.push_back(uint8_t(e.Read(0x1F801040, 1))); }
        e.Write(0x1F80104A, 0, 2);
        return rx;
    };
    e.SetPadButtons(0x4000);  // cross
    auto pad = xfer(0x0003, {0x01, 0x42, 0, 0, 0});
    CHECK(pad[1] == 0x41 && pad[2] == 0x5A && pad[3] == 0xFF && pad[4] == 0xBF);
    CHECK(xfer(0x0003, {0x81, 'S'})[1] == 0xFF);  // no card in slot 1
    std::vector<uint8_t> rd = {0x81, 'R', 0, 0, 0x00, 0x05};
    rd.resize(rd.size() + 4 + 128 + 2, 0);
    auto r = xfer(0x2003, rd);  // /JOY2
    CHECK(r[1] == 0x00 && r[2] == 0x5A && r[3] == 0x5D && r[10] == 0x3C && r[137] == 0x3C && r[139] == 0x47);
    CHECK(xfer(0x2003, {0x01, 0x42})[1] == 0xFF);  // no pad on /JOY2
    pad = xfer(0x0003, {0x01, 0x42, 0, 0, 0});     // pad unaffected
    CHECK(pad[1] == 0x41 && pad[4] == 0xBF);
    // /ACK -> IRQ7 for a card byte
    e.Write(0x1F801070, 0, 4);
    e.Write(0x1F80104A, 0x3003, 2);  // /JOY2, /ACK IRQ enabled
    e.Write(0x1F801040, 0x81, 1);
    e.RunFrame();
    CHECK(e.Read(0x1F801044, 2) & 0x280);
    CHECK(e.Read(0x1F801070, 4) & 0x80);
    e.Write(0x1F80104A, 0x0040, 2);
    // Slot 1 card on /JOY1 with the pad
    e.SetMemoryCard(0, &card1);
    CHECK(xfer(0x0003, {0x81, 'S', 0, 0})[3] == 0x5D);
    pad = xfer(0x0003, {0x01, 0x42, 0, 0, 0});
    CHECK(pad[1] == 0x41 && pad[4] == 0xBF);
    e.SetPadButtons(0);
    e.SetMemoryCard(0, nullptr);
    e.SetMemoryCard(1, nullptr);
    e.Shutdown();
}

int main() {
    TestFormat();
    TestProtocol();
    TestAtomicFile();
    TestSioPort();
    std::puts("unit_test_memcard: all checks passed");
    return 0;
}
