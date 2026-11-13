// HLE kernel: events, interrupt chain, kernel tables and trace wiring, driven by synthetic guest
// code only (psx-spx "BIOS Event Functions", "BIOS Interrupt/Exception Handling").
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "engine/engine.h"
#include "loader/exe_loader.h"
#include "loader/ps1_bios_hle.h"
#include "trace/trace.h"

using personarm::CpuContext;
using personarm::Engine;
using personarm::Ps1BiosHle;

static int g_failures = 0, g_checks = 0;
#define CHECK_EQ(name, got, want)                                                                   \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        unsigned long long g_ = (unsigned long long)(got), w_ = (unsigned long long)(want);         \
        if (g_ != w_) {                                                                             \
            ++g_failures;                                                                           \
            std::printf("  FAIL %s: got 0x%llx want 0x%llx (line %d)\n", name, g_, w_, __LINE__);  \
        }                                                                                           \
    } while (0)

// ── Tiny MIPS assembler ─────────────────────────────────────────────────────
enum : uint32_t { zero = 0, at = 1, v0 = 2, a0 = 4, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, t2 = 10,
                  s0 = 16, s7 = 23, ra = 31 };
static uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }
static uint32_t R(uint32_t fn, uint32_t rs, uint32_t rt, uint32_t rd) { return (rs << 21) | (rt << 16) | (rd << 11) | fn; }
static uint32_t ADDIU(uint32_t rt, uint32_t rs, uint32_t imm) { return I(0x09, rs, rt, imm); }
static uint32_t LUI(uint32_t rt, uint32_t imm) { return I(0x0F, 0, rt, imm); }
static uint32_t LW(uint32_t rt, uint32_t off, uint32_t base) { return I(0x23, base, rt, off); }
static uint32_t SW(uint32_t rt, uint32_t off, uint32_t base) { return I(0x2B, base, rt, off); }
static uint32_t MOVE(uint32_t rd, uint32_t rs) { return R(0x21, rs, zero, rd); }
static uint32_t JR(uint32_t rs) { return R(0x08, rs, 0, 0); }
static uint32_t J(uint32_t addr) { return (0x02u << 26) | ((addr >> 2) & 0x03FFFFFF); }
static const uint32_t kNop = 0, kSyscall = 0x0000000C;
static const uint32_t kBase = 0x80010000, kData = 0x80100000; // data words at phys 0x100000

struct Prog {
    std::vector<uint32_t> w = std::vector<uint32_t>(512, 0); // 2 KiB at kBase
    uint32_t pc = kBase;
    void Emit(std::initializer_list<uint32_t> ws) { for (uint32_t x : ws) { w[(pc - kBase) / 4] = x; pc += 4; } }
    void Org(uint32_t a) { pc = a; }
    void Li(uint32_t r, uint32_t v) { Emit({LUI(r, v >> 16), I(0x0D, r, r, v & 0xFFFF)}); }
    void Bios(uint32_t vec, uint32_t fn) { Emit({ADDIU(t2, zero, vec), R(0x09, t2, 0, ra), ADDIU(t1, zero, fn)}); }
    void Beq(uint32_t rs, uint32_t rt, uint32_t target) { Emit({I(0x04, rs, rt, (target - pc - 4) / 4), kNop}); }
    void Inc(uint32_t off) { Emit({LUI(t0, kData >> 16), LW(t1, off, t0), kNop, ADDIU(t1, t1, 1), SW(t1, off, t0)}); }
    void EnableVblank() { Emit({LUI(at, 0x1F80), ADDIU(t0, zero, 1), SW(t0, 0x1074, at), ADDIU(t0, zero, 0x401),
                                (0x10u << 26) | (4u << 21) | (t0 << 16) | (12u << 11)}); } // I_MASK=1; mtc0 t0,SR
    void Spin() { uint32_t here = pc; Emit({J(here), kNop}); }
};

static Engine& Boot(const Prog& p) {
    auto& e = Engine::Instance();
    personarm::EngineBootConfig config;
    config.enable_bios_hle = true;
    e.Initialize(config);
    std::vector<uint8_t> exe(4096, 0);
    auto* hdr = reinterpret_cast<personarm::PsxExeHeader*>(exe.data());
    std::memcpy(hdr->magic, "PS-X EXE", 8);
    hdr->initial_pc = kBase;
    hdr->destination_ram = kBase;
    hdr->file_size = 2048;
    hdr->initial_sp_base = 0x801FFF00;
    std::memcpy(exe.data() + 2048, p.w.data(), 2048);
    e.BootExecutable(exe.data(), exe.size());
    return e;
}
static uint32_t Data(Engine& e, uint32_t off) {
    const auto& r = e.GetRam();
    uint32_t a = (kData & 0x1FFFFF) + off;
    return r[a] | (r[a + 1] << 8) | (r[a + 2] << 16) | (uint32_t(r[a + 3]) << 24);
}

// Counters: +0 chain handler, +4 event callback, +8 TestEvent/WaitEvent hits.
static const uint32_t kHandlerA = kBase + 0x400, kCallbackB = kBase + 0x440;
static void Handlers(Prog& p) {
    p.Org(kHandlerA); // SysEnqIntRP func1: count, return 0 (func2 not called), leave I_STAT alone
    p.Inc(0);
    p.Emit({JR(ra), ADDIU(v0, zero, 0)});
    p.Org(kCallbackB); // event callback
    p.Inc(4);
    p.Emit({JR(ra), kNop});
}

// VBlank -> priority-0 chain handler + kernel RCnt3 event (callback mode and ready mode/TestEvent).
static Prog VsyncProgram(bool poll = true) {
    Prog p;
    p.Emit({LUI(s7, kData >> 16)});
    p.Li(t0, kHandlerA);
    p.Emit({SW(t0, 0x48, s7), ADDIU(a0, zero, 0), ADDIU(a1, s7, 0x40)});
    p.Bios(0xC0, 0x02); // SysEnqIntRP(0, elem)
    p.Li(a0, 0xF2000003);
    p.Emit({ADDIU(a1, zero, 2), ADDIU(a2, zero, 0x1000)});
    p.Li(a3, kCallbackB);
    p.Bios(0xB0, 0x08); // OpenEvent(RCnt3, EvSpINT, EvMdINTR, cb)
    p.Emit({MOVE(a0, v0)});
    p.Bios(0xB0, 0x0C);
    p.Li(a0, 0xF2000003);
    p.Emit({ADDIU(a1, zero, 2), ADDIU(a2, zero, 0x2000), MOVE(a3, zero)});
    p.Bios(0xB0, 0x08); // OpenEvent(RCnt3, EvSpINT, EvMdNOINTR, 0)
    p.Emit({MOVE(s0, v0), MOVE(a0, v0)});
    p.Bios(0xB0, 0x0C);
    p.EnableVblank();
    if (!poll) p.Spin(); // keeps the trace small: only setup calls and IRQs
    uint32_t loop = p.pc;
    p.Emit({MOVE(a0, s0)});
    p.Bios(0xB0, 0x0B); // TestEvent
    p.Beq(v0, zero, loop);
    p.Inc(8);
    p.Emit({J(loop), kNop});
    Handlers(p);
    return p;
}

static void TestVsyncChain() {
    auto& e = Boot(VsyncProgram());
    for (int i = 0; i < 5; ++i) e.RunFrame();
    // VBlank is raised at the end of frames 1..5 and delivered during frames 2..5.
    CHECK_EQ("chain handler once per frame", Data(e, 0), 4);
    CHECK_EQ("event callback once per frame", Data(e, 4), 4);
    CHECK_EQ("TestEvent ready once per frame", Data(e, 8), 4);
    CHECK_EQ("SR restored after IRQ", e.GetCpu().GetRegisters().cp0_status & 0xFFFF, 0x401);
}

static void TestWaitEvent() {
    Prog p;
    p.Emit({LUI(s7, kData >> 16)});
    p.Li(a0, 0xF2000003);
    p.Emit({ADDIU(a1, zero, 2), ADDIU(a2, zero, 0x2000), MOVE(a3, zero)});
    p.Bios(0xB0, 0x08);
    p.Emit({MOVE(s0, v0), MOVE(a0, v0)});
    p.Bios(0xB0, 0x0A); // WaitEvent on a disabled event returns 0 at once
    p.Emit({ADDIU(t0, v0, 1), SW(t0, 0x10, s7), MOVE(a0, s0)});
    p.Bios(0xB0, 0x0C);
    p.EnableVblank();
    uint32_t loop = p.pc;
    p.Emit({MOVE(a0, s0)});
    p.Bios(0xB0, 0x0A); // blocks until the VBlank event is ready
    p.Emit({SW(v0, 0x14, s7)});
    p.Inc(8);
    p.Emit({J(loop), kNop});
    auto& e = Boot(p);
    for (int i = 0; i < 5; ++i) e.RunFrame();
    CHECK_EQ("WaitEvent disabled -> 0", Data(e, 0x10), 1);
    CHECK_EQ("WaitEvent ready -> 1", Data(e, 0x14), 1);
    CHECK_EQ("WaitEvent wakes once per frame", Data(e, 8), 4);
}

static void TestCriticalSection() {
    Prog p;
    p.Emit({LUI(s7, kData >> 16)});
    p.Li(a0, 0xF2000003);
    p.Emit({ADDIU(a1, zero, 2), ADDIU(a2, zero, 0x1000)});
    p.Li(a3, kCallbackB);
    p.Bios(0xB0, 0x08);
    p.Emit({MOVE(a0, v0)});
    p.Bios(0xB0, 0x0C);
    p.EnableVblank();
    p.Emit({ADDIU(a0, zero, 1), kSyscall}); // EnterCriticalSection
    uint32_t wait = p.pc;
    p.Emit({LW(t0, 0x0C, s7), kNop});
    p.Beq(t0, zero, wait); // until the host sets the flag
    p.Emit({ADDIU(a0, zero, 2), kSyscall}); // ExitCriticalSection
    p.Spin();
    Handlers(p);
    auto& e = Boot(p);
    for (int i = 0; i < 3; ++i) e.RunFrame();
    CHECK_EQ("no delivery inside critical section", Data(e, 4), 0);
    std::vector<uint8_t> ram = e.GetRam();
    ram[(kData & 0x1FFFFF) + 0x0C] = 1;
    e.SetRam(ram);
    e.RunFrame();
    CHECK_EQ("pending VBlank delivered after ExitCriticalSection", Data(e, 4), 1);
    e.RunFrame();
    CHECK_EQ("next VBlank delivered", Data(e, 4), 2);
}

static void TestTablePatch() {
    const uint32_t kMyFunc = kBase + 0x400;
    Prog p;
    p.Emit({LUI(s7, kData >> 16)});
    p.Bios(0xB0, 0x57); // GetB0Table
    p.Emit({SW(v0, 0x20, s7)});
    p.Li(t0, kMyFunc);
    p.Emit({SW(t0, 0x4D * 4, v0)}); // patch B(4Dh)
    p.Bios(0xA0, 0x44);             // FlushCache
    p.Emit({ADDIU(a0, zero, 0x55)});
    p.Bios(0xB0, 0x4D);
    p.Emit({SW(v0, 0x24, s7)});
    p.Bios(0xB0, 0x56); // GetC0Table
    p.Emit({SW(v0, 0x28, s7)});
    p.Spin();
    p.Org(kMyFunc);
    p.Emit({LUI(t0, kData >> 16), ADDIU(t1, zero, 0x1234), SW(t1, 0x2C, t0), JR(ra), ADDIU(v0, a0, 1)});
    auto& e = Boot(p);
    e.RunFrame();
    CHECK_EQ("GetB0Table", Data(e, 0x20), 0x874);
    CHECK_EQ("patched B entry ran", Data(e, 0x2C), 0x1234);
    CHECK_EQ("patched B entry returned to caller", Data(e, 0x24), 0x56);
    CHECK_EQ("GetC0Table", Data(e, 0x28), 0x674);
}

// Event semantics through direct HLE calls (no CPU).
static void TestEventSemantics() {
    auto& bios = Ps1BiosHle::Instance();
    bios.Initialize();
    bios.SetIoHooks(nullptr, nullptr);
    std::vector<uint8_t> ram(2 * 1024 * 1024, 0);
    bios.InstallKernel(ram.data(), ram.size());
    CpuContext cpu;
    auto call = [&](uint32_t vec, uint32_t fn, uint32_t x0 = 0, uint32_t x1 = 0, uint32_t x2 = 0, uint32_t x3 = 0) {
        cpu.SetGPR(9, fn);
        cpu.SetGPR(4, x0); cpu.SetGPR(5, x1); cpu.SetGPR(6, x2); cpu.SetGPR(7, x3);
        cpu.SetGPR(31, 0x80001000);
        cpu.SetPC(vec);
        bios.HandleBiosHook(cpu, ram.data(), ram.size());
        return cpu.GetGPR(2);
    };
    const uint32_t cls = 0xF2000001, spec = 2;
    uint32_t ev = call(0xB0, 0x08, cls, spec, 0x2000, 0);
    CHECK_EQ("OpenEvent descriptor", ev, 0xF1000000);
    CHECK_EQ("TestEvent disabled", call(0xB0, 0x0B, ev), 0);
    call(0xB0, 0x07, cls, spec);
    CHECK_EQ("Deliver to disabled is ignored", call(0xB0, 0x0B, ev), 0);
    call(0xB0, 0x0C, ev);
    call(0xB0, 0x07, cls, 4);
    CHECK_EQ("Deliver other spec", call(0xB0, 0x0B, ev), 0);
    call(0xB0, 0x07, cls, spec);
    CHECK_EQ("TestEvent ready", call(0xB0, 0x0B, ev), 1);
    CHECK_EQ("TestEvent rearmed", call(0xB0, 0x0B, ev), 0);
    call(0xB0, 0x07, cls, spec);
    call(0xB0, 0x20, cls, spec); // UnDeliverEvent
    CHECK_EQ("UnDeliverEvent", call(0xB0, 0x0B, ev), 0);
    call(0xB0, 0x07, cls, spec);
    CHECK_EQ("WaitEvent ready", call(0xB0, 0x0A, ev), 1);
    CHECK_EQ("WaitEvent returns to ra", cpu.GetPC(), 0x80001000);
    call(0xB0, 0x0A, ev);
    CHECK_EQ("WaitEvent busy blocks", cpu.GetPC() & 0x1FFFFFFF, 0xC780);
    call(0xB0, 0x0D, ev);
    CHECK_EQ("WaitEvent disabled", call(0xB0, 0x0A, ev), 0);
    CHECK_EQ("CloseEvent", call(0xB0, 0x09, ev), 1);
    uint32_t last = 0;
    for (int i = 0; i < 17; ++i) last = call(0xB0, 0x08, cls, spec, 0x2000, 0);
    CHECK_EQ("OpenEvent table full", last, 0xFFFFFFFF);
    CHECK_EQ("ChangeClearRCnt old flag", call(0xC0, 0x0A, 3, 0), 1);
    CHECK_EQ("ChangeClearRCnt new flag", call(0xC0, 0x0A, 3, 1), 0);
    CHECK_EQ("open missing file", call(0xA0, 0x00), 0xFFFFFFFF);
    CHECK_EQ("read bad fd", call(0xA0, 0x02, 7, 0x80100000, 4), 0xFFFFFFFF);
    CHECK_EQ("lseek bad fd", call(0xA0, 0x01, 7, 0, 0), 0xFFFFFFFF);
}

// Memory card "no card inserted": requests end with EvSpTIMOUT (psx-spx "Memory Card BIOS Functions").
static void TestMemoryCardNoCard() {
    auto& bios = Ps1BiosHle::Instance();
    bios.Initialize();
    bios.SetIoHooks(nullptr, nullptr);
    std::vector<uint8_t> ram(2 * 1024 * 1024, 0);
    bios.InstallKernel(ram.data(), ram.size());
    CpuContext cpu;
    auto call = [&](uint32_t vec, uint32_t fn, uint32_t x0 = 0, uint32_t x1 = 0, uint32_t x2 = 0, uint32_t x3 = 0) {
        cpu.SetGPR(9, fn);
        cpu.SetGPR(4, x0); cpu.SetGPR(5, x1); cpu.SetGPR(6, x2); cpu.SetGPR(7, x3);
        cpu.SetGPR(31, 0x80001000);
        cpu.SetPC(vec);
        bios.HandleBiosHook(cpu, ram.data(), ram.size());
        return cpu.GetGPR(2);
    };
    const uint32_t sw = 0xF4000001, hw = 0xF0000011, ioe = 0x0004, timeout = 0x0100;
    const uint32_t sw_to = call(0xB0, 0x08, sw, timeout, 0x2000, 0), sw_ok = call(0xB0, 0x08, sw, ioe, 0x2000, 0);
    const uint32_t hw_to = call(0xB0, 0x08, hw, timeout, 0x2000, 0);
    for (uint32_t ev : {sw_to, sw_ok, hw_to}) call(0xB0, 0x0C, ev);
    CHECK_EQ("_bu_init", call(0xA0, 0x70), 0);
    CHECK_EQ("InitCARD2", call(0xB0, 0x4A, 1), 0);
    CHECK_EQ("StartCARD", call(0xB0, 0x4B), 0);
    CHECK_EQ("_card_info accepted", call(0xA0, 0xAB, 0x00), 1);
    CHECK_EQ("_card_info -> SwCARD timeout", call(0xB0, 0x0B, sw_to), 1);
    CHECK_EQ("_card_info no IOE", call(0xB0, 0x0B, sw_ok), 0);
    CHECK_EQ("_card_read accepted", call(0xB0, 0x4F, 0x00, 0, 0x80100000), 1);
    CHECK_EQ("_card_read -> HwCARD timeout", call(0xB0, 0x0B, hw_to), 1);
    // libmcrd registers callback-mode events: the callback runs before returning to the caller.
    const uint32_t cb = call(0xB0, 0x08, sw, timeout, 0x1000, 0x80012340);
    call(0xB0, 0x0C, cb);
    call(0xA0, 0xAB, 0x10);
    CHECK_EQ("_card_info runs the SwCARD timeout callback", cpu.GetPC(), 0x80012340);
}

static std::string TraceRun() {
    std::ostringstream out;
    {
        personarm::trace::TraceHeader h;
        h.src = "prm";
        personarm::trace::TraceWriter w(out, h);
        auto& e = Engine::Instance();
        e.SetTraceSink(&w);
        Boot(VsyncProgram(false));
        for (int i = 0; i < 3; ++i) e.RunFrame();
        e.Shutdown(); // emits `end` and detaches the sink
    }
    return out.str();
}

static void TestTraceDeterminism() {
    std::string a = TraceRun(), b = TraceRun();
    CHECK_EQ("trace byte-identical across runs", a == b, 1);
    CHECK_EQ("trace has bios calls", a.find("\"ev\":\"call\"") != std::string::npos, 1);
    CHECK_EQ("trace has vsync", a.find("\"ev\":\"vsync\"") != std::string::npos, 1);
    CHECK_EQ("trace has vsync checkpoint", a.find("\"tag\":\"vsync_3\"") != std::string::npos, 1);
    CHECK_EQ("trace has irq", a.find("\"src\":\"vblank\"") != std::string::npos, 1);
    std::istringstream in(a);
    personarm::trace::TraceFile tf;
    std::string err;
    CHECK_EQ("trace parses", personarm::trace::ReadTrace(in, &tf, &err), 1);
}

int main() {
    std::printf("[UNIT TEST] HLE kernel events / IRQ chain / tables / trace\n");
    TestEventSemantics();
    TestMemoryCardNoCard();
    TestVsyncChain();
    TestWaitEvent();
    TestCriticalSection();
    TestTablePatch();
    TestTraceDeterminism();
    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
