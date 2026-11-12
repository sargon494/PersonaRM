// PRMTRACE v1 tests (TRC-FMT-001, DET-001, TRC-DIFF-001 comparator side). Synthetic data only.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "trace/trace.h"

using namespace personarm;
using namespace personarm::trace;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static TraceHeader Hdr(const char* src) {
    TraceHeader h;
    h.src = src; h.build = "test"; h.disc = "SLPS-00000"; h.exe_sha256 = "00";
    return h;
}

// Deterministic synthetic scenario; `ram` lets tests vary memory contents.
static std::string Scenario(const char* src, const std::vector<uint8_t>& ram, bool with_gp0 = true, bool irq = false) {
    std::ostringstream os;
    TraceWriter w(os, Hdr(src));
    w.OnBiosCall(0xA0, 0x3F, {1, 2, 3, 4}, 0x80010010);
    if (irq) w.OnIrq("vblank", 0x80010020);
    w.OnLoadFile(789, 0x80100000, 0x80010030);
    w.OnCdRead(1234, 8, 0x80100000);
    w.OnVsync(1, with_gp0 ? "00000000deadbeef" : "", 12);
    CpuRegisters r; r.pc = 0x80010040; r.gpr[4] = 7; r.hi = 1; r.lo = 2;
    std::vector<uint8_t> vram(64, 9);
    w.Checkpoint("overlay_loaded", r, ram.data(), ram.size(), nullptr, 0, vram.data(), vram.size());
    w.OnEnd("quit", 1);
    return os.str();
}

static TraceFile Parse(const std::string& s) {
    std::istringstream is(s); TraceFile f; std::string err;
    CHECK(ReadTrace(is, &f, &err));
    return f;
}

int main() {
    std::vector<uint8_t> ram(0x20000, 0x5A);

    // Round trip + known hash vector (FNV-1a 64 of "a").
    CHECK(Hex64(Fnv1a64(reinterpret_cast<const uint8_t*>("a"), 1)) == "af63dc4c8601ec8c");
    std::string t1 = Scenario("prm", ram);
    TraceFile f = Parse(t1);
    CHECK(f.header.NumberOr("v", 0) == 1);
    CHECK(f.events.size() == 6);
    CHECK(f.events[0].StringOr("vec", "") == "0x000000a0");
    CHECK(f.events[1].NumberOr("id", 0) == 789);
    CHECK(f.events[2].StringOr("ev", "") == "cd_read" && f.events[2].NumberOr("lba", 0) == 1234);
    CHECK(f.events[5].NumberOr("seq", -1) == 5);

    // Determinism: same input, identical bytes.
    CHECK(Scenario("prm", ram) == t1);

    // Equal traces from different sources, extra irq events and a missing gp0_hash: no divergence.
    CHECK(!CompareTraces(f, Parse(Scenario("redux", ram, false, true))).found);

    // Masked RAM (kernel area) is ignored; unmasked RAM is not.
    std::vector<uint8_t> kern = ram; kern[0x100] ^= 0xFF;
    CHECK(!CompareTraces(f, Parse(Scenario("redux", kern))).found);
    std::vector<uint8_t> user = ram; user[0x10000] ^= 0xFF;
    Divergence d = CompareTraces(f, Parse(Scenario("redux", user)));
    CHECK(d.found && d.index == 4);
    CHECK(d.reason.find("ram_hash") != std::string::npos);
    CHECK(!d.context_a.empty() && d.context_a.size() == d.context_b.size());

    // First divergence wins: tweak an earlier event as well.
    std::string t3 = Scenario("redux", user);
    size_t p = t3.find("\"lba\":1234");
    CHECK(p != std::string::npos);
    t3.replace(p, 10, "\"lba\":1235");
    d = CompareTraces(f, Parse(t3));
    CHECK(d.found && d.index == 2 && d.reason.find("lba") != std::string::npos);

    // Truncated trace.
    std::string t4 = t1.substr(0, t1.rfind("{\"ev\":\"end\""));
    d = CompareTraces(f, Parse(t4));
    CHECK(d.found && d.index == 5 && d.reason.find("ends early") != std::string::npos);

    // Header mismatch (disc).
    TraceHeader h2 = Hdr("redux"); h2.disc = "OTHER";
    std::ostringstream os; TraceWriter(os, h2);
    CHECK(CompareTraces(f, Parse(os.str())).reason.find("disc") != std::string::npos);

    // Malformed input.
    const char* bad[] = {
        "",                                                         // no header
        "{\"v\":2}\n",                                              // wrong version
        "not json\n",                                               // invalid JSON
        "{\"v\":1}\n[1]\n",                                         // non-object record
        "{\"v\":1}\n{\"seq\":0}\n",                                 // missing ev
        "{\"v\":1}\n{\"ev\":\"x\"}\n",                              // missing seq
        "{\"v\":1}\n{\"ev\":\"x\",\"seq\":1}\n{\"ev\":\"y\",\"seq\":1}\n", // seq not increasing
    };
    for (const char* b : bad) {
        std::istringstream is(b); TraceFile tf; std::string err;
        CHECK(!ReadTrace(is, &tf, &err));
        CHECK(!err.empty());
    }
    std::istringstream is("{\"v\":1}\n{\"ev\":\"x\",\"seq\":0}\n{oops\n"); TraceFile tf; std::string err;
    CHECK(!ReadTrace(is, &tf, &err) && err.rfind("line 3:", 0) == 0);
    // Unknown event type and CRLF are tolerated.
    std::istringstream ok("{\"v\":1}\r\n{\"ev\":\"future\",\"seq\":0}\r\n\r\n");
    CHECK(ReadTrace(ok, &tf, &err) && tf.events.size() == 1);

    // Collapsed polling calls (TRACE_FORMAT.md 3.1): 5 identical TestEvent polls -> one event, n=5.
    auto polls = [](int n, bool collapse_via_writer, int vsync_after) {
        std::ostringstream os;
        if (collapse_via_writer) {
            TraceWriter w(os, Hdr("prm"));
            for (int i = 0; i < n; ++i) w.OnBiosCall(0xB0, 0x0B, {0xF1000000, 0, 0, 0}, 0x80010000);
            if (vsync_after) w.OnVsync(1, "", 0);
            w.OnBiosCall(0xB0, 0x0B, {0xF1000001, 0, 0, 0}, 0x80010000);
            // destructor flushes the pending call
        } else {  // an uncollapsed producer (e.g. Redux Lua): one line per call, no "n"
            os << "{\"v\":1,\"disc\":\"SLPS-00000\",\"exe_sha256\":\"00\",\"mode\":\"events\",\"ram_masks\":[[\"0x00000000\",\"0x00010000\"]],\"src\":\"redux\"}\n";
            int seq = 0;
            auto call = [&](const char* a0) {
                os << "{\"a0\":\"" << a0 << "\",\"a1\":\"0x00000000\",\"a2\":\"0x00000000\",\"a3\":\"0x00000000\",\"ev\":\"call\","
                   << "\"fn\":11,\"kind\":\"bios\",\"ra\":\"0x80010000\",\"seq\":" << seq++ << ",\"vec\":\"0x000000b0\"}\n";
            };
            for (int i = 0; i < n; ++i) call("0xf1000000");
            if (vsync_after) os << "{\"ev\":\"vsync\",\"frame\":1,\"seq\":" << seq++ << "}\n";
            call("0xf1000001");
        }
        return os.str();
    };
    TraceFile c5 = Parse(polls(5, true, 0));
    CHECK(c5.events.size() == 2);
    CHECK(c5.events[0].NumberOr("n", 0) == 5 && c5.events[0].NumberOr("seq", -1) == 0);
    CHECK(c5.events[1].Find("n") == nullptr && c5.events[1].NumberOr("seq", -1) == 1);
    CHECK(Parse(polls(2, true, 1)).events.size() == 3);  // another event ends a run
    // Collapsed vs uncollapsed producer, and different poll counts: equal by default.
    CHECK(!CompareTraces(c5, Parse(polls(5, false, 0))).found);
    CHECK(!CompareTraces(c5, Parse(polls(9, false, 0))).found);
    CompareOptions strict;
    strict.compare_repeat_counts = true;
    CHECK(!CompareTraces(c5, Parse(polls(5, false, 0)), strict).found);
    Divergence dn = CompareTraces(c5, Parse(polls(9, true, 0)), strict);
    CHECK(dn.found && dn.index == 0 && dn.reason.find("repeat count") != std::string::npos);
    // A different call after the run still diverges.
    CHECK(CompareTraces(c5, Parse(polls(5, false, 1))).found);

    std::puts("unit_test_trace: all checks passed");
    return 0;
}
