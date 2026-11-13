// Recompiled execution inside the Engine (synthetic data only). The program of
// recomp_engine_program.h is booted with the HLE BIOS twice: interpreter only, and with the code
// tools/recomp_gen generated from it at build time (module "rengine") at quantum 1. Every frame
// must end at the same cycle, PC, registers and RAM: IRQs delivered while native code runs, frame
// ends inside native loops, syscalls, HLE calls from native code, dispatch misses and resumes.
// Both also run with devices ticked after every instruction (SetEagerDevices): lazy device time
// must not change a thing. The overlay program (recomp_overlay_program.h) swaps two generated
// overlays in one slot and writes into live code (CPU store, HLE memcpy, DMA) the same way.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/engine.h"
#include "loader/exe_loader.h"
#include "recomp_engine_program.h"
#include "recomp_overlay_program.h"

extern const personarm::recomp::Module kRecompModule_rengine;
extern const personarm::recomp::Module kRecompModule_rovl_main;
extern const personarm::recomp::Module kRecompModule_rovl_a;
extern const personarm::recomp::Module kRecompModule_rovl_b;

using namespace personarm;

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

constexpr int kFrames = 30;

struct Snap {
    uint64_t cycles;
    uint32_t pc;
    uint64_t regs, ram, scratch;
    bool operator==(const Snap& o) const {
        return cycles == o.cycles && pc == o.pc && regs == o.regs && ram == o.ram && scratch == o.scratch;
    }
};

struct Run {
    std::vector<Snap> frames;
    Engine::RecompStats stats;
    std::vector<uint8_t> ram;
};

std::vector<uint8_t> Exe(const g2::Program& p) {
    std::vector<uint8_t> exe(2048 + ((p.words.size() * 4 + 2047) & ~size_t{2047}), 0);
    auto* hdr = reinterpret_cast<PsxExeHeader*>(exe.data());
    std::memcpy(hdr->magic, "PS-X EXE", 8);
    hdr->initial_pc = p.fn.at("main");
    hdr->destination_ram = g2::kBase;
    hdr->file_size = static_cast<uint32_t>(exe.size() - 2048);
    std::memcpy(exe.data() + 2048, p.words.data(), p.words.size() * 4);
    return exe;
}

Run Boot(const std::vector<uint8_t>& exe, std::vector<const recomp::Module*> modules, uint32_t quantum,
         bool eager = false) {
    auto& e = Engine::Instance();
    EngineBootConfig config;
    config.enable_bios_hle = true;
    e.Initialize(config);
    e.SetRecompModules(std::move(modules), quantum);
    e.SetEagerDevices(eager);
    CHECK(e.BootExecutable(exe.data(), exe.size()));
    Run r;
    for (int f = 0; f < kFrames; ++f) {
        e.RunFrame();
        // A host RAM read between frames stalls like a guest load (the boot harness does this):
        // the devices then lag the CPU until the next instruction ends, in both modes; then an MMIO
        // read from the host sees them there.
        (void)e.Read(rengine::kData + 0x300, 4);
        (void)e.Read(0x1F801100, 4);
        const CpuRegisters& regs = e.GetCpu().GetRegisters();
        r.frames.push_back({e.GetCycles(), regs.pc, recomp::Fnv1a64(reinterpret_cast<const uint8_t*>(&regs), sizeof regs),
                            recomp::Fnv1a64(e.GetRam().data(), e.GetRam().size()),
                            recomp::Fnv1a64(e.GetScratchpad().data(), e.GetScratchpad().size())});
    }
    r.stats = e.GetRecompStats();
    r.ram = e.GetRam();
    e.SetRecompModules({});
    e.SetEagerDevices(false);
    e.Shutdown();
    return r;
}

uint32_t Word(const std::vector<uint8_t>& ram, uint32_t a) {
    a &= 0x1FFFFFu;
    return ram[a] | (ram[a + 1] << 8) | (ram[a + 2] << 16) | (uint32_t(ram[a + 3]) << 24);
}

int FirstDifference(const Run& a, const Run& b) {
    for (size_t i = 0; i < a.frames.size() && i < b.frames.size(); ++i)
        if (!(a.frames[i] == b.frames[i])) return static_cast<int>(i) + 1;
    return a.frames.size() == b.frames.size() ? 0 : -1;
}

} // namespace

int main() {
    const g2::Program p = rengine::Build();
    const std::vector<uint8_t> exe = Exe(p);

    const Run interp = Boot(exe, {}, 1);
    // The program did what the comparison relies on: IRQs, syscalls, memset, the uncompiled callee.
    const uint32_t d = rengine::kData;
    CHECK(Word(interp.ram, d + 32) > 2 * kFrames);   // IRQ handler runs (Timer2 + VBlank)
    CHECK(Word(interp.ram, d + 20) > 100);           // syscall pairs
    CHECK(Word(interp.ram, d + 24) > 300);           // unlisted
    CHECK(Word(interp.ram, d + 0x100) != 0);         // memset filled from a counter

    const Run native = Boot(exe, {&kRecompModule_rengine}, 1);
    const int diff = FirstDifference(interp, native);
    if (diff) std::fprintf(stderr, "recompiled run differs from the interpreter at frame %d\n", diff);
    CHECK(diff == 0);
    CHECK(native.ram == interp.ram);
    // Lazy device time vs ticking after every instruction (the reference), both modes.
    CHECK(FirstDifference(interp, Boot(exe, {}, 1, true)) == 0);
    CHECK(FirstDifference(interp, Boot(exe, {&kRecompModule_rengine}, 1, true)) == 0);
    const auto& st = native.stats;
    CHECK(st.active.size() == 1 && st.rejected.empty());
    CHECK(st.native_instructions > 4 * st.interp_resident);  // resume points bring the main loop back
    CHECK(st.stops > 0);                                     // IRQs due / frame ends inside native code
    CHECK(st.hle_calls > 0);                                 // memset and SysEnqIntRP stubs from native code
    CHECK(st.dispatch_misses > 0);                           // jal unlisted
    CHECK(st.exits.count(0x80000080u) && st.exits.at(0x80000080u) > 0);  // syscall exceptions
    CHECK(st.exits.count(p.fn.at("unlisted")));
    CHECK(st.interp_resident > 0);                           // hazard (refused) and the unlisted callee
    std::printf("recomp engine: native %llu, interpreted resident %llu, entries %llu, stops %llu, HLE %llu, misses %llu\n",
                (unsigned long long)st.native_instructions, (unsigned long long)st.interp_resident,
                (unsigned long long)st.entries, (unsigned long long)st.stops, (unsigned long long)st.hle_calls,
                (unsigned long long)st.dispatch_misses);

    // Hash guard: code generated from another image is never run.
    recomp::Module other = kRecompModule_rengine;
    other.code_hash ^= 1;
    const Run guarded = Boot(exe, {&other}, 1);
    CHECK(guarded.stats.rejected.size() == 1 && guarded.stats.native_instructions == 0);
    CHECK(FirstDifference(interp, guarded) == 0);

    // Larger quantum: IRQs may come late (not compared), but frames still end and the program runs.
    const Run coarse = Boot(exe, {&kRecompModule_rengine}, 256);
    CHECK(coarse.frames.size() == static_cast<size_t>(kFrames));
    CHECK(coarse.stats.native_instructions > 0);
    CHECK(Word(coarse.ram, d + 32) > kFrames);

    // Overlays: two modules for one slot, live only while their code is in RAM.
    const rovl::Set ov = rovl::Build();
    const std::vector<uint8_t> ovl_exe = Exe(ov.main);
    const std::vector<const recomp::Module*> ovl_mods = {&kRecompModule_rovl_main, &kRecompModule_rovl_a, &kRecompModule_rovl_b};
    const Run oi = Boot(ovl_exe, {}, 1);
    const uint32_t od = rovl::kData;
    CHECK(Word(oi.ram, od) > 20);                     // main loop iterations
    CHECK(Word(oi.ram, od + 20) > 40);                // resident helper called from both overlays
    CHECK(FirstDifference(oi, Boot(ovl_exe, {}, 1, true)) == 0);
    const Run on = Boot(ovl_exe, ovl_mods, 1);
    const int odiff = FirstDifference(oi, on);
    if (odiff) std::fprintf(stderr, "overlay run differs from the interpreter at frame %d\n", odiff);
    CHECK(odiff == 0);
    CHECK(on.ram == oi.ram);
    CHECK(FirstDifference(oi, Boot(ovl_exe, ovl_mods, 1, true)) == 0);
    const auto& os = on.stats;
    CHECK(os.active.size() == 1 && os.rejected.size() == 2);   // the overlays are not in RAM at boot
    CHECK(os.loads.count("rovl_a") && os.loads.at("rovl_a") > 10);
    CHECK(os.loads.count("rovl_b") && os.loads.at("rovl_b") > 10);
    CHECK(os.hash_mismatches > 0);                    // same first word: the hash tells A from B
    // Every load retires the one before (CPU copy, memcpy, DMA), plus the patches of A.
    CHECK(os.invalidations + 1 >= os.loads.at("rovl_a") + os.loads.at("rovl_b"));
    CHECK(os.native_instructions > 20 * (os.interp_resident + os.interp_overlay));
    std::printf("recomp overlays: native %llu, interpreted %llu, loads A %llu B %llu, mismatches %llu, retired %llu, stops %llu\n",
                (unsigned long long)os.native_instructions, (unsigned long long)(os.interp_resident + os.interp_overlay),
                (unsigned long long)os.loads.at("rovl_a"), (unsigned long long)os.loads.at("rovl_b"),
                (unsigned long long)os.hash_mismatches, (unsigned long long)os.invalidations, (unsigned long long)os.stops);
    // Code from another image at the slot is never run: a mismatching overlay stays interpreted.
    recomp::Module bad_a = kRecompModule_rovl_a;
    bad_a.code_hash ^= 1;
    const Run ob = Boot(ovl_exe, {&kRecompModule_rovl_main, &bad_a, &kRecompModule_rovl_b}, 1);
    CHECK(FirstDifference(oi, ob) == 0);
    CHECK(!ob.stats.loads.count("rovl_a") && ob.stats.loads.count("rovl_b"));

    std::printf("recomp engine integration: %d failures\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
