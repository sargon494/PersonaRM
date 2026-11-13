// Gate G2 (synthetic): interpreter vs code generated at build time by tools/recomp_gen from the
// synthetic program in g2_synth_program.h. Zero divergence required.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>

#include "g2_diff.h"
#include "g2_synth_program.h"
#include "recomp/codegen.h"

extern const personarm::recomp::FunctionEntry kRecomp_g2[];
extern const size_t kRecomp_g2_count;

using namespace personarm;

namespace {

constexpr uint32_t kBuffer = 0x80100000u;

int g_fail = 0;
uint64_t g_stop_at = 0;  // service-point test: stop after this many instructions
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

std::vector<uint8_t> InitialRam(const g2::Program& p, std::mt19937& rng) {
    std::vector<uint8_t> ram(g2::kRamSize, 0);
    for (size_t i = 0; i < p.words.size(); ++i) g2::RamWrite(ram, g2::kBase + 4 * static_cast<uint32_t>(i), p.words[i], 4);
    for (uint32_t i = 0; i < 64; ++i) ram[(kBuffer & (g2::kRamSize - 1)) + i] = static_cast<uint8_t>(rng());
    return ram;
}

} // namespace

int main() {
    const g2::Program p = g2::Build();
    recomp::Dispatcher disp;
    disp.Add(kRecomp_g2, kRecomp_g2_count);

    // Generator decisions: every entry but the hazard one is compiled.
    CHECK(disp.Size() == p.entries.size() - 1);
    CHECK(disp.Find(p.fn.at("hazard")) == nullptr);
    CHECK(disp.Find(p.fn.at("unlisted")) == nullptr);
    CHECK(disp.Find(p.fn.at("chain_a")) != nullptr);

    // The verifier refuses a load whose delay slot reads the register (generator called directly).
    {
        std::vector<uint8_t> img;
        for (uint32_t w : {0x8C880000u, 0x01001021u, 0x03E00008u, 0u})   // lw t0,0(a0); addu v0,t0,zero; jr ra; nop
            for (int i = 0; i < 4; ++i) img.push_back(static_cast<uint8_t>(w >> (8 * i)));
        const auto r = recomp::GenerateCpp(img.data(), img.size(), g2::kBase, {g2::kBase}, "t");
        CHECK(r.functions.size() == 1 && !r.functions[0].generated && r.functions[0].reason.find("load-delay") != std::string::npos);
    }

    std::mt19937 rng(0x6232u);
    const uint32_t edge[] = {0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x8000u, 0xFFFF8000u, 3u};
    auto rnd = [&]() -> uint32_t {
        if (rng() % 4 == 0) return edge[rng() % 8];
        return static_cast<uint32_t>(rng());
    };

    // name -> fixups on the random initial registers
    const std::vector<std::pair<std::string, std::function<void(CpuRegisters&)>>> cases = {
        {"arith", nullptr}, {"divs", nullptr}, {"loop", nullptr}, {"branches", nullptr},
        {"mem", [](CpuRegisters& r) { r.gpr[g2::a0] = kBuffer; }},
        {"ovf", nullptr}, {"ovf_ds", nullptr}, {"chain_a", nullptr}, {"switch", [&](CpuRegisters& r) { r.gpr[g2::a0] = rng() % 6; }},
        {"jalr", nullptr}, {"jalr_unk", nullptr}, {"gte", nullptr}, {"cop0", nullptr}, {"sys", nullptr}, {"brk", nullptr},
        {"adel", nullptr}, {"below", nullptr}, {"hazard", [](CpuRegisters& r) { r.gpr[g2::a0] = kBuffer; }},
    };

    uint64_t runs = 0, fallback_runs = 0;
    for (const auto& c : cases) {
        for (int iter = 0; iter < 300; ++iter) {
            CpuRegisters init;
            for (int i = 1; i < 32; ++i) init.gpr[i] = rnd();
            init.gpr[g2::sp] = g2::kStack;
            init.gpr[g2::ra] = g2::kSentinel;
            init.hi = rnd();
            init.lo = rnd();
            init.pc = p.fn.at(c.first);
            if (c.second) c.second(init);
            const std::vector<uint8_t> ram = InitialRam(p, rng);
            Gte gte;
            for (uint32_t i = 0; i < 32; ++i) { gte.WriteDataRegister(i, rnd()); gte.WriteControlRegister(i, rnd()); }

            const g2::Final a = g2::RunInterpreter(init, ram, gte);
            const g2::Final b = g2::RunRecompiled(init, ram, gte, disp);
            const std::string d = g2::Diff(a, b);
            if (!d.empty()) {
                std::fprintf(stderr, "DIVERGENCE %s iter %d: %s\n", c.first.c_str(), iter, d.c_str());
                ++g_fail;
                break;
            }
            ++runs;
            fallback_runs += b.fallbacks ? 1 : 0;
        }
    }
    CHECK(fallback_runs == 600);   // jalr_unk (dispatch miss) and hazard (never compiled), 300 each

    // Service points: stopped after any k instructions (delay slots, in-flight loads, LWL/LWR
    // chains, nested calls), generated code must hand the interpreter the exact state it would
    // hold after k steps, and both must then finish identically.
    uint64_t stop_checks = 0;
    for (const char* name : {"arith", "loop", "branches", "mem", "chain_a", "switch", "jalr", "gte", "cop0", "below"}) {
        for (int iter = 0; iter < 20; ++iter) {
            CpuRegisters init;
            for (int i = 1; i < 32; ++i) init.gpr[i] = rnd();
            init.gpr[g2::sp] = g2::kStack;
            init.gpr[g2::ra] = g2::kSentinel;
            init.pc = p.fn.at(name);
            if (std::string(name) == "mem") init.gpr[g2::a0] = kBuffer;
            if (std::string(name) == "switch") init.gpr[g2::a0] = rng() % 6;
            if (std::string(name) == "loop") init.gpr[g2::a0] = rng() % 8;
            const std::vector<uint8_t> ram0 = InitialRam(p, rng);
            Gte gte0;
            for (uint32_t i = 0; i < 32; ++i) { gte0.WriteDataRegister(i, rnd()); gte0.WriteControlRegister(i, rnd()); }
            for (uint64_t k = 1; k < 400; ++k) {
                std::vector<uint8_t> ram_ref = ram0, ram_nat = ram0;
                CpuContext ref;
                g2::Hook(ref, ram_ref);
                ref.SetRegisters(init);
                ref.GetGTE() = gte0;
                bool ended = false;
                for (uint64_t i = 0; i < k && !ended; ++i) {
                    ended = ref.GetPC() == g2::kSentinel || ref.GetPC() == g2::kVector;
                    if (!ended) ref.Step();
                }
                if (ended) break;

                recomp::CpuState s;
                Gte gte = gte0;
                std::memcpy(s.gpr, init.gpr.data(), sizeof s.gpr);
                s.hi = init.hi; s.lo = init.lo; s.sr = init.cp0_status; s.cause = init.cp0_cause; s.epc = init.cp0_epc;
                s.gte = &gte;
                s.mem = &ram_nat;
                s.read = [](void* m, uint32_t a, uint32_t sz) { return g2::RamRead(*static_cast<std::vector<uint8_t>*>(m), a, sz); };
                s.write = [](void* m, uint32_t a, uint32_t v, uint32_t sz) { g2::RamWrite(*static_cast<std::vector<uint8_t>*>(m), a, v, sz); };
                s.dispatcher = &disp;
                s.deadline = k;
                g_stop_at = k;  // a forced service point (mtc0/rfe set deadline 0) before k continues
                s.service = [](recomp::CpuState& st) { st.deadline = g_stop_at; return st.cycles >= g_stop_at; };
                recomp::Dispatch(s, init.pc);
                if (!s.stopped) break;   // ended (return, exception, dispatch miss) before k instructions
                CHECK(s.cycles == k);

                CpuContext nat;
                g2::Hook(nat, ram_nat);
                CpuRegisters r = init;
                std::memcpy(r.gpr.data(), s.gpr, sizeof s.gpr);
                r.hi = s.hi; r.lo = s.lo; r.pc = s.pc; r.cp0_status = s.sr; r.cp0_cause = s.cause; r.cp0_epc = s.epc;
                nat.SetRegisters(r);
                nat.SetBadVaddr(s.badvaddr);
                if (s.next_pc) nat.SetPCInDelaySlot(s.pc, s.next_pc);
                if (s.load_reg) nat.SetPendingLoad(s.load_reg, s.load_val);
                nat.GetGTE() = gte;
                const bool same_regs = std::memcmp(&nat.GetRegisters(), &ref.GetRegisters(), sizeof(CpuRegisters)) == 0;
                if (!same_regs) {
                    const CpuRegisters &x = ref.GetRegisters(), &y = nat.GetRegisters();
                    std::fprintf(stderr, "STOP %s iter %d k %llu: registers differ at the stop (pc %08X/%08X sr %08X/%08X cause %08X/%08X epc %08X/%08X)\n",
                                 name, iter, (unsigned long long)k, x.pc, y.pc, x.cp0_status, y.cp0_status, x.cp0_cause, y.cp0_cause,
                                 x.cp0_epc, y.cp0_epc);
                    for (int i = 0; i < 32; ++i)
                        if (x.gpr[i] != y.gpr[i]) std::fprintf(stderr, "  r%d %08X/%08X\n", i, x.gpr[i], y.gpr[i]);
                }
                CHECK(same_regs);
                g2::Final a, b;
                a.timeout = !g2::InterpretToEnd(ref, a.end_pc);
                b.timeout = !g2::InterpretToEnd(nat, b.end_pc);
                a.regs = ref.GetRegisters(); b.regs = nat.GetRegisters();
                a.ram = ram_ref; b.ram = ram_nat;
                g2::Snapshot(ref.GetGTE(), a); g2::Snapshot(nat.GetGTE(), b);
                const std::string d = g2::Diff(a, b);
                if (!d.empty()) std::fprintf(stderr, "STOP %s iter %d k %llu: %s\n", name, iter, (unsigned long long)k, d.c_str());
                CHECK(d.empty());
                ++stop_checks;
                if (!same_regs || !d.empty()) break;
            }
        }
    }
    CHECK(stop_checks > 2000);

    std::printf("G2 synthetic: %zu functions compiled, %llu differential runs, %llu with interpreter fallback, "
                "%llu service-point stops, %d failures\n", disp.Size(), static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(fallback_runs), static_cast<unsigned long long>(stop_checks), g_fail);
    return g_fail == 0 ? 0 : 1;
}
