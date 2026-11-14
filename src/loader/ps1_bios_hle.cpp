#include "loader/ps1_bios_hle.h"
#include "loader/disc_reader.h"
#include "core/logger.h"
#include "core/save_manager.h"
#include "trace/trace.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// Kernel layout in guest RAM. The game reads/patches these, so they live in emulated RAM.
// psx-spx "BIOS Memory Map" (from memory, verify): table of tables at 100h, A table at 200h,
// C table at 674h, B table at 874h; kernel control blocks (ExCB, PCB, TCB, EvCB) at A000E000h+.
// Everything below 10000h is masked out of trace ram_hash (TRACE_FORMAT.md section 2).
// The libapi kernel patchers write into B-table+594h..+898h (E08h..110Bh, GHIDRA GH-007),
// so nothing of ours lives there.
// ───────────────────────────────────────────────────────────────────────────
namespace {
constexpr uint32_t kATable = 0x200, kATableN = 0xB5;
constexpr uint32_t kCTable = 0x674, kCTableN = 0x1E;
constexpr uint32_t kBTable = 0x874, kBTableN = 0x5E;
// HLE entry points: table entry N of vector V holds kStubV + 4*N; the PC is intercepted there.
constexpr uint32_t kStubA = 0xC000, kStubB = 0xC400, kStubC = 0xC600;
constexpr uint32_t kKernelRet = 0xC700;   // ra of every guest function the kernel calls
constexpr uint32_t kRcntHandler = 0xC710; // +4*n: kernel's default RCnt n IRQ handler (3 = VBlank)
constexpr uint32_t kHleEnd = 0xC780;      // [C000h, C780h) is intercepted
constexpr uint32_t kWaitSpin = 0xC780;    // guest code: j WaitEvent stub (blocked WaitEvent)
constexpr uint32_t kRcntElems = 0xC800;   // 4 SysEnqIntRP elements x 10h for the RCnt handlers
constexpr uint32_t kKernelStackTop = 0xDFF0;
constexpr uint32_t kEvCB = 0xE000, kEvCBN = 16, kEvCBSize = 0x1C;
constexpr uint32_t kExCB = 0xE1C0;        // 4 priorities x 8: +0 chain head
constexpr uint32_t kPCB = 0xE1E0;         // +0 pointer to the current TCB
constexpr uint32_t kHookPtr = 0xE1E4;     // jmp_buf run after the chain (HookEntryInt)
constexpr uint32_t kRcntAck = 0xE1E8;     // 4 words: ChangeClearRCnt flags
constexpr uint32_t kPadAck = 0xE1F8;      // ChangeClearPad flag
constexpr uint32_t kDefaultHook = 0xE200; // jmp_buf 30h (ResetEntryInt)
constexpr uint32_t kTCB = 0xE240, kTCBSize = 0xC0;
constexpr uint32_t kDCB = 0xE400, kDCBSize = 0x50; // one device control block ("bu"), name at +50h
constexpr uint32_t kIStat = 0x1F801070, kIMask = 0x1F801074, kGp0 = 0x1F801810;

constexpr uint32_t Kseg0(uint32_t a) { return 0x80000000u | a; }

uint32_t Rd(const uint8_t* ram, size_t size, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFF;
    if (!ram || p + 4 > size) return 0;
    return ram[p] | (ram[p + 1] << 8) | (ram[p + 2] << 16) | (uint32_t(ram[p + 3]) << 24);
}
void Wr(uint8_t* ram, size_t size, uint32_t addr, uint32_t v) {
    uint32_t p = addr & 0x1FFFFF;
    if (!ram || p + 4 > size) return;
    for (int i = 0; i < 4; ++i) ram[p + i] = uint8_t(v >> (8 * i));
}
std::string Hex(uint32_t v) { std::ostringstream o; o << std::hex << v; return o.str(); }

// psx-spx "BIOS Event Functions": EvCB +00h class, +04h status, +08h spec, +0Ch mode, +10h func.
constexpr uint32_t kEvFree = 0, kEvDisabled = 0x1000, kEvBusy = 0x2000, kEvReady = 0x4000;
constexpr uint32_t kEvModeCallback = 0x1000, kEvModeReady = 0x2000;
constexpr uint32_t kHwCard = 0xF0000011, kSwCard = 0xF4000001, kEvSpTimeout = 0x0100, kEvSpIoe = 0x0004;
} // namespace

Ps1BiosHle& Ps1BiosHle::Instance() {
    static Ps1BiosHle instance;
    return instance;
}

void Ps1BiosHle::Initialize() {
    Reset();
    m_initialized = true;
    PRM_LOG_INFO("BIOS", "PlayStation 1 HLE BIOS Services initialized (Vectors A, B, C)");
}

void Ps1BiosHle::Reset() {
    m_intercept_count = 0;
    m_open_files.clear();
    m_card_files.clear();
    m_find = {};
    m_next_fd = 2; // shortcut: fd 0/1 are assumed to be the TTY (stdin/stdout) as after InstallDevices
    m_frames.clear();
    m_irq_frame_base = 0;
    m_in_irq = false;
    m_irq_bits = 0;
}

bool Ps1BiosHle::IsHleAddress(uint32_t p) {
    return p == 0x80 || p == 0xA0 || p == 0xB0 || p == 0xC0 || (p >= kStubA && p < kHleEnd);
}

void Ps1BiosHle::InstallKernel(uint8_t* ram, size_t n) {
    // Table of tables (psx-spx "BIOS Memory Map", from memory): ExCB, PCB, TCB, EvCB pointer/size pairs.
    Wr(ram, n, 0x100, 0xA0000000u | kExCB); Wr(ram, n, 0x104, 4 * 8);
    Wr(ram, n, 0x108, 0xA0000000u | kPCB);  Wr(ram, n, 0x10C, 4);
    Wr(ram, n, 0x110, 0xA0000000u | kTCB);  Wr(ram, n, 0x114, kTCBSize);
    Wr(ram, n, 0x120, 0xA0000000u | kEvCB); Wr(ram, n, 0x124, kEvCBN * kEvCBSize);
    // Device control blocks (psx-spx "BIOS File/Device Control Blocks", 50h each): the game's libcard
    // finds the "bu" device here and hooks its firstfile entry (+34h) before calling B(42h) firstfile;
    // without it, firstfile is skipped and the game waits for a card directory read forever.
    // The entries are never called: the HLE file functions do the work.
    Wr(ram, n, 0x150, 0xA0000000u | kDCB);  Wr(ram, n, 0x154, kDCBSize);
    Wr(ram, n, kDCB + 0x00, Kseg0(kDCB + kDCBSize));
    Wr(ram, n, kDCB + 0x04, 0x14);          // flags: block device
    Wr(ram, n, kDCB + 0x08, 0x80);          // sector size
    Wr(ram, n, kDCB + 0x34, Kseg0(kStubB + 4 * 0x42));
    Wr(ram, n, kDCB + 0x38, Kseg0(kStubB + 4 * 0x43));
    Wr(ram, n, kDCB + kDCBSize, 'b' | 'u' << 8);
    for (uint32_t i = 0; i < kATableN; ++i) Wr(ram, n, kATable + 4 * i, Kseg0(kStubA + 4 * i));
    for (uint32_t i = 0; i < kBTableN; ++i) Wr(ram, n, kBTable + 4 * i, Kseg0(kStubB + 4 * i));
    for (uint32_t i = 0; i < kCTableN; ++i) Wr(ram, n, kCTable + 4 * i, Kseg0(kStubC + 4 * i));
    Wr(ram, n, kPCB, 0xA0000000u | kTCB);
    // Default exit hook (ResetEntryInt): longjmp target = ReturnFromException on the kernel stack.
    Wr(ram, n, kDefaultHook + 0x00, Kseg0(kStubB + 4 * 0x17));
    Wr(ram, n, kDefaultHook + 0x04, Kseg0(kKernelStackTop));
    Wr(ram, n, kHookPtr, Kseg0(kDefaultHook));
    // shortcut (from memory): the BIOS boots with auto-acknowledge on for all four RCnt IRQs.
    for (uint32_t i = 0; i < 4; ++i) Wr(ram, n, kRcntAck + 4 * i, 1);
    Wr(ram, n, kPadAck, 1);
    // C(00h) EnqueueTimerAndVblankIrqs: priority 1 chain = VBlank, Timer2, Timer1, Timer0 (SVC-031).
    // Element layout (psx-spx "SysEnqIntRP", from memory): +0 next, +4 func2, +8 func1, +C 0.
    const uint32_t order[4] = {3, 2, 1, 0};
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t e = kRcntElems + 0x10 * i;
        Wr(ram, n, e + 0, i < 3 ? Kseg0(e + 0x10) : 0);
        Wr(ram, n, e + 8, Kseg0(kRcntHandler + 4 * order[i]));
    }
    Wr(ram, n, kExCB + 8 * 1, Kseg0(kRcntElems));
    // Blocked WaitEvent spins here with interrupts as the caller left them: j WaitEvent stub; nop.
    Wr(ram, n, kWaitSpin, (0x02u << 26) | (((kStubB + 4 * 0x0A) >> 2) & 0x03FFFFFF));
    Wr(ram, n, kWaitSpin + 4, 0);
    m_frames.clear();
    m_in_irq = false;
}

bool Ps1BiosHle::HandleBiosHook(CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    return HandleBiosHook(cpu, nullptr, 0);
}

bool Ps1BiosHle::HandleBiosHook(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    uint32_t pc = cpu.GetPC();
    uint32_t masked_pc = pc & 0x1FFFFFFF;

    if (masked_pc == 0xA0 || masked_pc == 0xB0 || masked_pc == 0xC0) {
        // Function code in $t1; the call goes through the RAM table so library patches take effect.
        BiosVector vec = static_cast<BiosVector>(masked_pc);
        uint32_t func = cpu.GetGPR(9);
        if (m_trace) {
            m_trace->OnBiosCall(masked_pc, func, {cpu.GetGPR(4), cpu.GetGPR(5), cpu.GetGPR(6), cpu.GetGPR(7)},
                                cpu.GetGPR(31));
        }
        uint32_t table = masked_pc == 0xA0 ? kATable : masked_pc == 0xB0 ? kBTable : kCTable;
        uint32_t count = masked_pc == 0xA0 ? kATableN : masked_pc == 0xB0 ? kBTableN : kCTableN;
        uint32_t stub = masked_pc == 0xA0 ? kStubA : masked_pc == 0xB0 ? kStubB : kStubC;
        uint32_t entry = func < count ? Rd(ram, ram_size, table + 4 * func) : 0;
        if (entry != 0 && (entry & 0x1FFFFFFF) != stub + 4 * func) {
            cpu.SetPC(entry); // patched entry: run the guest replacement, it returns to $ra itself
            return true;
        }
        return ExecuteCall(vec, func, cpu, ram, ram_size);
    }
    if (masked_pc >= kStubA && masked_pc < kStubA + 4 * kATableN)
        return ExecuteCall(BiosVector::VectorA, (masked_pc - kStubA) / 4, cpu, ram, ram_size);
    if (masked_pc >= kStubB && masked_pc < kStubB + 4 * kBTableN)
        return ExecuteCall(BiosVector::VectorB, (masked_pc - kStubB) / 4, cpu, ram, ram_size);
    if (masked_pc >= kStubC && masked_pc < kStubC + 4 * kCTableN)
        return ExecuteCall(BiosVector::VectorC, (masked_pc - kStubC) / 4, cpu, ram, ram_size);
    if (masked_pc == kKernelRet) {
        if (m_frames.empty()) {
            PRM_LOG_ERROR("HLE", "kernel return trampoline reached with no pending kernel call");
            return false;
        }
        Resume(cpu, ram, ram_size);
        return true;
    }
    if (masked_pc >= kRcntHandler && masked_pc < kRcntHandler + 16) {
        RcntIrq((masked_pc - kRcntHandler) / 4, cpu, ram, ram_size);
        return true;
    }
    if (masked_pc >= kStubA && masked_pc < kHleEnd) {
        // Unused stub slot: behave like an unknown function and return.
        cpu.SetGPR(2, 0);
        cpu.SetPC(cpu.GetGPR(31));
        return true;
    }

    if (masked_pc == 0x80) {
        CpuRegisters regs = cpu.GetRegisters();
        const uint32_t exc = (regs.cp0_cause >> 2) & 0x1Fu;
        if (exc == 0) {
            EnterInterrupt(cpu, ram, ram_size);
            return true;
        }
        // Exception vector with no BIOS in RAM: resume after the opcode and pop SR (rfe).
        regs.pc = regs.cp0_epc + 4;
        if (exc == 8) {
            // psx-spx "BIOS Misc Functions" (from memory): SYS(01h) EnterCriticalSection clears SR
            // bits 2 and 10 of the mode RFE restores (IEp, IM2) and returns 1 if interrupts were
            // enabled, else 0; SYS(02h) ExitCriticalSection sets them. Other SYS numbers: no-op.
            if (regs.gpr[4] == 1) {
                regs.gpr[2] = (regs.cp0_status & 0x404u) == 0x404u ? 1u : 0u;
                regs.cp0_status &= ~0x404u;
            } else if (regs.gpr[4] == 2) {
                regs.cp0_status |= 0x404u;
            }
        } else if (exc != 9) {
            // A fault (Ov, AdEL, AdES...) means a real bug, so make it visible instead of silently resuming.
            std::ostringstream m;
            m << "unhandled CPU exception code " << exc << " at EPC 0x" << std::hex << regs.cp0_epc;
            PRM_LOG_ERROR("HLE", m.str());
        }
        regs.cp0_status = (regs.cp0_status & ~0x0Fu) | ((regs.cp0_status >> 2) & 0x0Fu);
        cpu.SetRegisters(regs);
        return true;
    }

    return false;
}

// ── Interrupt chain ─────────────────────────────────────────────────────────
// psx-spx "BIOS Interrupt/Exception Handling" (SVC-031): the kernel saves the registers to the
// current TCB, walks the SysEnqIntRP chains by priority 0..3 calling func1 of each element (and
// func2 with a0 = func1's result when that is nonzero; from memory), then longjmps (r2=1) to the
// HookEntryInt jmp_buf, which ends in ReturnFromException. A handler may call ReturnFromException
// itself to skip the rest. Guest functions run on the CPU and return through kKernelRet.

void Ps1BiosHle::EnterInterrupt(CpuContext& cpu, uint8_t* ram, size_t n) {
    if (m_in_irq) PRM_LOG_ERROR("HLE", "nested interrupt while the kernel IRQ chain is running");
    CpuRegisters regs = cpu.GetRegisters();
    // TCB layout (psx-spx "BIOS Thread Functions"): +08h r0..r31, +88h EPC, +8Ch HI, +90h LO, +94h SR, +98h CAUSE.
    for (uint32_t i = 0; i < 32; ++i) Wr(ram, n, kTCB + 8 + 4 * i, i == 0 ? 0 : regs.gpr[i]);
    Wr(ram, n, kTCB + 0x88, regs.cp0_epc);
    Wr(ram, n, kTCB + 0x8C, regs.hi);
    Wr(ram, n, kTCB + 0x90, regs.lo);
    Wr(ram, n, kTCB + 0x94, regs.cp0_status);
    Wr(ram, n, kTCB + 0x98, regs.cp0_cause);
    m_in_irq = true;
    m_irq_bits = (m_io_read ? m_io_read(kIStat, 4) & m_io_read(kIMask, 4) : 0);
    m_irq_frame_base = m_frames.size();
    cpu.SetGPR(29, Kseg0(kKernelStackTop));
    Frame walk;
    walk.irq_walk = true;
    m_frames.push_back(walk);
    Resume(cpu, ram, n);
}

void Ps1BiosHle::CallGuest(CpuContext& cpu, uint32_t func, uint32_t a0) {
    cpu.SetGPR(4, a0);
    cpu.SetGPR(31, Kseg0(kKernelRet));
    cpu.SetPC(func);
}

void Ps1BiosHle::Resume(CpuContext& cpu, uint8_t* ram, size_t n) {
    while (!m_frames.empty()) {
        Frame& f = m_frames.back();
        if (!f.irq_walk) {
            if (f.idx < f.funcs.size()) {
                CallGuest(cpu, f.funcs[f.idx++], 0);
                return;
            }
            uint32_t ret_pc = f.ret_pc, ret_v0 = f.ret_v0;
            m_frames.pop_back();
            cpu.SetGPR(2, ret_v0);
            cpu.SetPC(ret_pc); // kKernelRet when the caller was itself kernel-called: resumes below
            return;
        }
        if (f.stage == 1) {
            uint32_t v0 = cpu.GetGPR(2);
            f.stage = 0;
            if (v0 != 0 && f.func2 != 0) {
                f.stage = 2;
                CallGuest(cpu, f.func2, v0);
                return;
            }
        } else if (f.stage == 2) {
            f.stage = 0;
        }
        for (;;) {
            uint32_t e = f.next;
            while (e == 0 && f.prio < 3) e = Rd(ram, n, kExCB + 8 * uint32_t(++f.prio));
            if (e == 0) break;
            if (++f.visited > 256) {
                PRM_LOG_ERROR("HLE", "interrupt chain loop detected; skipping the rest");
                break;
            }
            f.next = Rd(ram, n, e); // read before the call: a handler may dequeue itself
            f.func2 = Rd(ram, n, e + 4);
            uint32_t func1 = Rd(ram, n, e + 8);
            if (func1 != 0) {
                f.stage = 1;
                CallGuest(cpu, func1, 0);
                return;
            }
        }
        m_frames.pop_back();
        RunExitHook(cpu, ram, n);
        return;
    }
}

void Ps1BiosHle::RunExitHook(CpuContext& cpu, uint8_t* ram, size_t n) {
    uint32_t hook = Rd(ram, n, kHookPtr);
    if (hook == 0) { // kernel tables not installed (bare unit-test RAM)
        ReturnFromException(cpu, ram, n);
        return;
    }
    // longjmp(hook, 1); jmp_buf (psx-spx "setjmp"): ra, sp, fp, s0..s7, gp.
    cpu.SetGPR(31, Rd(ram, n, hook));
    cpu.SetGPR(29, Rd(ram, n, hook + 4));
    cpu.SetGPR(30, Rd(ram, n, hook + 8));
    for (uint32_t i = 0; i < 8; ++i) cpu.SetGPR(16 + i, Rd(ram, n, hook + 0x0C + 4 * i));
    cpu.SetGPR(28, Rd(ram, n, hook + 0x2C));
    cpu.SetGPR(2, 1);
    cpu.SetPC(cpu.GetGPR(31));
}

void Ps1BiosHle::ReturnFromException(CpuContext& cpu, uint8_t* ram, size_t n) {
    CpuRegisters regs = cpu.GetRegisters();
    for (uint32_t i = 1; i < 32; ++i) regs.gpr[i] = Rd(ram, n, kTCB + 8 + 4 * i);
    regs.cp0_epc = Rd(ram, n, kTCB + 0x88);
    regs.hi = Rd(ram, n, kTCB + 0x8C);
    regs.lo = Rd(ram, n, kTCB + 0x90);
    uint32_t sr = Rd(ram, n, kTCB + 0x94);
    regs.cp0_status = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu); // rfe
    regs.pc = regs.cp0_epc;
    if (m_in_irq) {
        m_frames.resize(std::min(m_frames.size(), m_irq_frame_base));
        m_in_irq = false;
        // shortcut: real hardware re-enters forever if no handler acknowledged its IRQ; we drop the
        // sources that were pending at entry and are still unacknowledged so a game without a handler
        // for an enabled source keeps running. Remove once all used sources have handlers.
        if (m_io_read && m_io_write) {
            uint32_t left = m_io_read(kIStat, 4) & m_irq_bits;
            if (left) m_io_write(kIStat, ~left, 4);
        }
    }
    cpu.SetRegisters(regs);
}

void Ps1BiosHle::RcntIrq(uint32_t t, CpuContext& cpu, uint8_t* ram, size_t n) {
    // Kernel default priority-1 handler for RCnt t (3 = VBlank, IRQ0; 0-2 = IRQ4-6): on a pending
    // enabled IRQ deliver event class F200000t spec 0002h (EvSpINT) and acknowledge it when
    // ChangeClearRCnt(t,1) (psx-spx "BIOS Event Functions"/"Interrupt Control"; handler details from memory).
    std::vector<uint32_t> callbacks;
    uint32_t bit = 1u << (t == 3 ? 0 : 4 + t);
    if (m_io_read && (m_io_read(kIStat, 4) & m_io_read(kIMask, 4) & bit)) {
        DeliverEvent(0xF2000000u + t, 0x0002, ram, n, callbacks);
        if (Rd(ram, n, kRcntAck + 4 * t) && m_io_write) m_io_write(kIStat, ~bit, 4);
    }
    cpu.SetGPR(2, 0);
    FinishCall(cpu, callbacks);
}

void Ps1BiosHle::FinishCall(CpuContext& cpu, std::vector<uint32_t>& callbacks) {
    uint32_t ra = cpu.GetGPR(31);
    if (callbacks.empty()) {
        if (ra != 0) cpu.SetPC(ra);
        return;
    }
    Frame f;
    f.funcs = std::move(callbacks);
    f.ret_pc = ra;
    f.ret_v0 = cpu.GetGPR(2);
    m_frames.push_back(std::move(f));
    CallGuest(cpu, m_frames.back().funcs[m_frames.back().idx++], 0);
}

// ── Events (psx-spx "BIOS Event Functions", SVC-030) ────────────────────────

uint32_t Ps1BiosHle::DeliverEvent(uint32_t cls, uint32_t spec, uint8_t* ram, size_t n,
                                  std::vector<uint32_t>& callbacks) {
    for (uint32_t i = 0; i < kEvCBN; ++i) {
        uint32_t e = kEvCB + i * kEvCBSize;
        if (Rd(ram, n, e + 4) != kEvBusy || Rd(ram, n, e) != cls || Rd(ram, n, e + 8) != spec) continue;
        uint32_t mode = Rd(ram, n, e + 0x0C);
        if (mode == kEvModeReady) {
            Wr(ram, n, e + 4, kEvReady);
        } else if (mode == kEvModeCallback && Rd(ram, n, e + 0x10) != 0) {
            callbacks.push_back(Rd(ram, n, e + 0x10)); // stays busy
        }
    }
    return 0;
}

// Memory card with no card inserted (psx-spx "Memory Card BIOS Functions"): every request ends with
// EvSpTIMOUT on SwCARD (_card_info/_card_load) or HwCARD (_card_read/_card_write).
// shortcut: delivered during the call instead of from the card IRQ some time later.
void Ps1BiosHle::CardNoCard(uint32_t cls, uint8_t* ram, size_t n, std::vector<uint32_t>& callbacks) {
    DeliverEvent(cls, kEvSpTimeout, ram, n, callbacks);
}

// ── Memory card (psx-spx "Memory Card BIOS Functions" / "Memory Card Data Format") ──────────────
// shortcut: every request completes during the call (events delivered before it returns) instead of
// from the card IRQ some milliseconds later, as the no-card path already did. Event choice follows
// common HLE BIOSes (unverified against a real BIOS): sector I/O ends with HwCARD EvSpIOE, info/load
// and async file I/O with HwCARD + SwCARD EvSpIOE. The "new card" flag is never reported.

void Ps1BiosHle::CardSector(bool write, CpuContext& cpu, uint8_t* ram, size_t n, std::vector<uint32_t>& callbacks) {
    MemoryCard* card = CardAtPort(cpu.GetGPR(4));
    const uint32_t sector = cpu.GetGPR(5), buf = cpu.GetGPR(6) & 0x1FFFFF;
    if (!card) { CardNoCard(kHwCard, ram, n, callbacks); cpu.SetGPR(2, 1); return; }
    if (sector >= MemoryCard::kFrames || !ram || buf + MemoryCard::kFrameSize > n) { cpu.SetGPR(2, 0); return; }
    if (write) card->WriteFrame(sector, ram + buf);
    else std::memcpy(ram + buf, card->Frame(sector), MemoryCard::kFrameSize);
    DeliverEvent(kHwCard, kEvSpIoe, ram, n, callbacks);
    cpu.SetGPR(2, 1);
}

namespace {
constexpr uint32_t kBlockSize = 0x2000, kFramesPerBlock = 64;
uint8_t Xor127(const uint8_t* f) { uint8_t x = 0; for (int i = 0; i < 127; ++i) x ^= f[i]; return x; }
std::string DirName(const uint8_t* dir) {  // directory frame +0Ah, 20 chars max
    std::string s;
    for (int i = 0; i < 20 && dir[0x0A + i]; ++i) s += char(dir[0x0A + i]);
    return s;
}
// firstfile wildcards: '?' any one character, '*' anything from here on.
bool NameMatches(const std::string& pat, const std::string& name) {
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] == '*') return true;
        if (i >= name.size() || (pat[i] != '?' && pat[i] != name[i])) return false;
    }
    return pat.size() == name.size();
}
// Directory frame (1..15) of the first block of `name`, 0 if none.
uint32_t FindFile(const MemoryCard& card, const std::string& name) {
    for (uint32_t f = 1; f < 16; ++f)
        if (card.Frame(f)[0] == 0x51 && DirName(card.Frame(f)) == name) return f;
    return 0;
}
} // namespace

// "buXY:NAME" (X = slot 0/1, Y = multitap port, only 0 here). Returns false for a slot without a card
// so the caller answers -1 as before.
bool Ps1BiosHle::CardFileOpen(const std::string& path, uint32_t mode, CpuContext& cpu) {
    MemoryCard* card = (path[2] == '0' || path[2] == '1') && path[3] == '0' ? m_cards[path[2] - '0'] : nullptr;
    if (!card) return false;
    const std::string name = path.substr(5, 20);
    int32_t result = -1;
    CardFile file{card, {}, 0, (mode & 0x8000) != 0};
    const uint32_t f = name.empty() ? 0 : FindFile(*card, name);
    if (mode & 0x200) {  // create, size in blocks in bits 16-31; fails if the name exists or space is short
        const uint32_t blocks = std::max<uint32_t>(1, mode >> 16);
        std::vector<uint32_t> free_frames;
        for (uint32_t d = 1; d < 16 && free_frames.size() < blocks; ++d)
            if ((card->Frame(d)[0] & 0xF0) == 0xA0) free_frames.push_back(d);
        if (!name.empty() && !f && free_frames.size() == blocks) {
            for (size_t i = 0; i < blocks; ++i) {
                uint8_t dir[MemoryCard::kFrameSize] = {};
                dir[0] = i == 0 ? 0x51 : (i + 1 == blocks ? 0x53 : 0x52);
                if (i == 0) {
                    const uint32_t size = blocks * kBlockSize;
                    for (int b = 0; b < 4; ++b) dir[4 + b] = uint8_t(size >> (8 * b));
                    std::memcpy(dir + 0x0A, name.data(), name.size());
                }
                const uint16_t next = i + 1 < blocks ? uint16_t(free_frames[i + 1] - 1) : 0xFFFF;
                dir[8] = uint8_t(next);
                dir[9] = uint8_t(next >> 8);
                dir[127] = Xor127(dir);
                card->WriteFrame(free_frames[i], dir);
            }
            file.blocks = free_frames;
        }
    } else if (f) {
        for (uint32_t d = f, guard = 0; d >= 1 && d < 16 && guard < 15; ++guard) {
            file.blocks.push_back(d);
            const uint32_t next = card->Frame(d)[8] | (card->Frame(d)[9] << 8);
            if (next == 0xFFFF) break;
            d = next + 1;
        }
    }
    if (!file.blocks.empty()) {
        result = m_next_fd++;
        m_card_files[result] = std::move(file);
    }
    PRM_LOG_INFO("BIOS", "open('" + path + "', " + Hex(mode) + "h) -> " + std::to_string(result));
    cpu.SetGPR(2, static_cast<uint32_t>(result));
    return true;
}

// read/write on a card fd: whole 128-byte frames at a 128-byte aligned position inside the file.
bool Ps1BiosHle::CardFileIo(bool write, CpuContext& cpu, uint8_t* ram, size_t n, std::vector<uint32_t>& callbacks) {
    auto it = m_card_files.find(static_cast<int>(cpu.GetGPR(4)));
    if (it == m_card_files.end()) return false;
    CardFile& f = it->second;
    const uint32_t buf = cpu.GetGPR(5) & 0x1FFFFF, len = cpu.GetGPR(6);
    const uint32_t size = uint32_t(f.blocks.size()) * kBlockSize;
    if (!ram || len % MemoryCard::kFrameSize || f.pos % MemoryCard::kFrameSize || len > size - f.pos ||
        buf + size_t(len) > n) {
        cpu.SetGPR(2, 0xFFFFFFFFu);
        return true;
    }
    for (uint32_t done = 0; done < len; done += MemoryCard::kFrameSize, f.pos += MemoryCard::kFrameSize) {
        const uint32_t frame = f.blocks[f.pos / kBlockSize] * kFramesPerBlock + (f.pos % kBlockSize) / MemoryCard::kFrameSize;
        if (write) f.card->WriteFrame(frame, ram + buf + done);
        else std::memcpy(ram + buf + done, f.card->Frame(frame), MemoryCard::kFrameSize);
    }
    if (f.async) {  // completion through events; v0 = 0 like common HLE BIOSes
        DeliverEvent(kHwCard, kEvSpIoe, ram, n, callbacks);
        DeliverEvent(kSwCard, kEvSpIoe, ram, n, callbacks);
        cpu.SetGPR(2, 0);
    } else {
        cpu.SetGPR(2, len);
    }
    return true;
}

// firstfile(pattern, direntry) / nextfile(direntry) -> direntry, or 0 when no (more) file matches.
// DIRENTRY: name[20], +14h attr, +18h size, +1Ch next, +20h head (first block), +24h system[4].
void Ps1BiosHle::CardFind(bool first, CpuContext& cpu, uint8_t* ram, size_t n, std::vector<uint32_t>& callbacks) {
    const uint32_t entry = cpu.GetGPR(first ? 5 : 4);
    if (first) {
        std::string pat;
        for (uint32_t p = cpu.GetGPR(4) & 0x1FFFFF; ram && p < n && ram[p] && pat.size() < 64; ++p) pat += char(ram[p]);
        m_find = {};
        if (pat.size() >= 5 && pat.compare(0, 2, "bu") == 0 && pat[4] == ':' && pat[3] == '0' && (pat[2] == '0' || pat[2] == '1'))
            m_find.card = m_cards[pat[2] - '0'];
        m_find.pattern = pat.size() > 5 ? pat.substr(5) : "*";
        // firstfile re-reads the card directory (libcard forces it, see the "bu" DCB): the game waits
        // for that read's HwCARD event. A "buXY:" path on an empty slot times out.
        if (pat.compare(0, 2, "bu") == 0)
            DeliverEvent(kHwCard, m_find.card ? kEvSpIoe : kEvSpTimeout, ram, n, callbacks);
    }
    cpu.SetGPR(2, 0);
    if (!m_find.card || !ram || (entry & 0x1FFFFF) + 0x28 > n) return;
    while (m_find.next < 16) {
        const uint8_t* dir = m_find.card->Frame(m_find.next++);
        const std::string name = DirName(dir);
        if (dir[0] != 0x51 || name.empty() || !NameMatches(m_find.pattern, name)) continue;
        uint8_t* e = ram + (entry & 0x1FFFFF);
        std::memset(e, 0, 0x28);
        std::memcpy(e, name.data(), name.size());
        Wr(ram, n, entry + 0x14, 0x51);
        Wr(ram, n, entry + 0x18, Rd(dir, MemoryCard::kFrameSize, 4));
        Wr(ram, n, entry + 0x20, m_find.next - 1);
        cpu.SetGPR(2, entry);
        return;
    }
}

uint32_t Ps1BiosHle::EventCall(uint32_t fn, CpuContext& cpu, uint8_t* ram, size_t n, bool& blocked) {
    uint32_t a0 = cpu.GetGPR(4);
    if (fn == 0x08) { // OpenEvent(class, spec, mode, func) -> F10000xxh, or -1 when the table is full
        for (uint32_t i = 0; i < kEvCBN; ++i) {
            uint32_t e = kEvCB + i * kEvCBSize;
            if (Rd(ram, n, e + 4) != kEvFree) continue;
            Wr(ram, n, e, a0);
            Wr(ram, n, e + 4, kEvDisabled);
            Wr(ram, n, e + 8, cpu.GetGPR(5));
            Wr(ram, n, e + 0x0C, cpu.GetGPR(6));
            Wr(ram, n, e + 0x10, cpu.GetGPR(7));
            return 0xF1000000u | i;
        }
        return 0xFFFFFFFFu;
    }
    uint32_t idx = a0 & 0xFFFF;
    if ((a0 & 0xFFFF0000u) != 0xF1000000u || idx >= kEvCBN || !ram) return 0;
    uint32_t e = kEvCB + idx * kEvCBSize;
    uint32_t st = Rd(ram, n, e + 4);
    switch (fn) {
        case 0x09: Wr(ram, n, e + 4, kEvFree); return 1;                       // CloseEvent
        case 0x0C: if (st != kEvFree) Wr(ram, n, e + 4, kEvBusy); return 1;    // EnableEvent
        case 0x0D: if (st != kEvFree) Wr(ram, n, e + 4, kEvDisabled); return 1; // DisableEvent
        case 0x0B:                                                              // TestEvent
        case 0x0A:                                                              // WaitEvent
            if (st == kEvReady) { Wr(ram, n, e + 4, kEvBusy); return 1; }
            // WaitEvent on a busy event blocks until a DeliverEvent makes it ready (forever for
            // callback-mode events, as on hardware); disabled/free returns 0.
            if (fn == 0x0A && st == kEvBusy) blocked = true;
            return 0;
    }
    return 0;
}

void Ps1BiosHle::LogUnimplemented(BiosVector vec, uint32_t fn) {
    uint32_t key = (static_cast<uint32_t>(vec) << 8) | fn;
    if (m_logged_unimpl.insert(key).second) {
        const char v = vec == BiosVector::VectorA ? 'A' : vec == BiosVector::VectorB ? 'B' : 'C';
        PRM_LOG_WARN("BIOS", std::string("unimplemented ") + v + "(" + Hex(fn) + "h) -> v0=0 (logged once)");
    }
}

bool Ps1BiosHle::ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, MemoryManager& memory) {
    (void)memory;
    return ExecuteCall(vec, func_code, cpu, nullptr, 0);
}

// Function numbers per psx-spx "BIOS Function Summary". Implemented: what Persona 2 EP calls
// (FUNCTION_CATALOG "Stubs BIOS": 7 A0, 26 B0, 3 C0 + B(56h)/B(57h)) for events, interrupts,
// tables, heap and files, plus the existing memcpy/memset/puts. Memory card (A70/AB/AC, B4A-50,
// "buXY:" files, B42/43 firstfile/nextfile) works on the inserted MemoryCard, else "no card inserted".
bool Ps1BiosHle::ExecuteCall(BiosVector vec, uint32_t func_code, CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    m_intercept_count++;
    std::vector<uint32_t> callbacks;
    bool ret = true;

    if (vec == BiosVector::VectorA) {
        switch (func_code) {
            case 0x00: HandleFileOpen(cpu, ram, ram_size); break;   // open(name, mode)
            case 0x01: HandleFileSeek(cpu); break;                  // lseek(fd, off, whence)
            case 0x02:                                              // read(fd, dst, len)
                if (!CardFileIo(false, cpu, ram, ram_size, callbacks)) HandleFileRead(cpu, ram, ram_size);
                break;
            case 0x03:                                              // write(fd, src, len)
                if (!CardFileIo(true, cpu, ram, ram_size, callbacks)) HandleFileWrite(cpu, ram, ram_size);
                break;
            case 0x04: HandleFileClose(cpu); break;                 // close(fd)
            case 0x2A: HandleMemCopy(cpu, ram, ram_size); break;
            case 0x2B: HandleMemSet(cpu, ram, ram_size); break;
            case 0x39: HandleHeapInit(cpu); break;                  // InitHeap(addr, size)
            case 0x44: cpu.SetGPR(2, 0); break;                     // FlushCache: no I-cache modelled
            case 0x49:                                              // GPU_cw(gp0cmd)
                if (m_io_write) m_io_write(kGp0, cpu.GetGPR(4), 4);
                cpu.SetGPR(2, 0);
                break;
            case 0x72: cpu.SetGPR(2, 0); break;                     // _96_remove: no BIOS CD driver to remove
            case 0x70: cpu.SetGPR(2, 0); break;                     // _bu_init
            case 0xAB: case 0xAC:                                   // _card_info(port) / _card_load(port)
                if (CardAtPort(cpu.GetGPR(4))) {
                    // Card present, directory readable: low and high level "end of I/O".
                    DeliverEvent(kHwCard, kEvSpIoe, ram, ram_size, callbacks);
                    DeliverEvent(kSwCard, kEvSpIoe, ram, ram_size, callbacks);
                } else {
                    CardNoCard(kSwCard, ram, ram_size, callbacks);
                }
                cpu.SetGPR(2, 1);
                break;
            default: LogUnimplemented(vec, func_code); cpu.SetGPR(2, 0); break;
        }
    } else if (vec == BiosVector::VectorB) {
        switch (func_code) {
            case 0x07: // DeliverEvent(class, spec)
                cpu.SetGPR(2, DeliverEvent(cpu.GetGPR(4), cpu.GetGPR(5), ram, ram_size, callbacks));
                break;
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
                bool blocked = false;
                uint32_t v0 = EventCall(func_code, cpu, ram, ram_size, blocked);
                if (blocked) { // re-enter WaitEvent from guest code so time passes and IRQs fire
                    cpu.SetPC(Kseg0(kWaitSpin));
                    ret = false;
                } else {
                    cpu.SetGPR(2, v0);
                }
                break;
            }
            case 0x12: HandlePadInit(cpu); break; // InitPAD(buf1, siz1, buf2, siz2) (InitPAD2)
            case 0x13: cpu.SetGPR(2, 1); break;   // StartPAD: shortcut, libpad drives SIO0 directly
            case 0x17: ReturnFromException(cpu, ram, ram_size); ret = false; break;
            case 0x18: Wr(ram, ram_size, kHookPtr, Kseg0(kDefaultHook)); cpu.SetGPR(2, 0); break; // ResetEntryInt
            case 0x19: Wr(ram, ram_size, kHookPtr, cpu.GetGPR(4)); cpu.SetGPR(2, 0); break;       // HookEntryInt(jmp_buf)
            case 0x20: { // UnDeliverEvent(class, spec): ready -> busy for matching ready-mode events
                for (uint32_t i = 0; i < kEvCBN; ++i) {
                    uint32_t e = kEvCB + i * kEvCBSize;
                    if (Rd(ram, ram_size, e + 4) == kEvReady && Rd(ram, ram_size, e + 0x0C) == kEvModeReady &&
                        Rd(ram, ram_size, e) == cpu.GetGPR(4) && Rd(ram, ram_size, e + 8) == cpu.GetGPR(5))
                        Wr(ram, ram_size, e + 4, kEvBusy);
                }
                cpu.SetGPR(2, 0);
                break;
            }
            case 0x32: HandleFileOpen(cpu, ram, ram_size); break;
            case 0x33: HandleFileSeek(cpu); break;
            case 0x34: if (!CardFileIo(false, cpu, ram, ram_size, callbacks)) HandleFileRead(cpu, ram, ram_size); break;
            case 0x35: if (!CardFileIo(true, cpu, ram, ram_size, callbacks)) HandleFileWrite(cpu, ram, ram_size); break;
            case 0x36: HandleFileClose(cpu); break;
            case 0x3F: HandleStdPuts(cpu, ram, ram_size); break;
            case 0x42: case 0x43:                     // firstfile / nextfile
                CardFind(func_code == 0x42, cpu, ram, ram_size, callbacks);
                break;
            case 0x4A: case 0x4B: case 0x4C: cpu.SetGPR(2, 0); break; // InitCARD2 / StartCARD / StopCARD
            case 0x4E: case 0x4F:                     // _card_write / _card_read(port, sector, buf)
                CardSector(func_code == 0x4E, cpu, ram, ram_size, callbacks);
                break;
            case 0x50: cpu.SetGPR(2, 0); break;       // _new_card
            case 0x56: cpu.SetGPR(2, kCTable); break; // GetC0Table
            case 0x57: cpu.SetGPR(2, kBTable); break; // GetB0Table
            case 0x5B: // ChangeClearPad(flag)
                Wr(ram, ram_size, kPadAck, cpu.GetGPR(4));
                cpu.SetGPR(2, 0);
                break;
            default: LogUnimplemented(vec, func_code); cpu.SetGPR(2, 0); break;
        }
    } else if (vec == BiosVector::VectorC) {
        uint32_t a0 = cpu.GetGPR(4), a1 = cpu.GetGPR(5);
        switch (func_code) {
            case 0x02: { // SysEnqIntRP(priority, struc): insert at the head of the chain
                uint32_t head = kExCB + 8 * (a0 & 3);
                Wr(ram, ram_size, a1, Rd(ram, ram_size, head));
                Wr(ram, ram_size, head, a1);
                cpu.SetGPR(2, 0);
                break;
            }
            case 0x03: { // SysDeqIntRP(priority, struc)
                // Deviation: psx-spx documents a BIOS bug here (only the first element can be removed);
                // we unlink any element, which is what callers intend.
                uint32_t link = kExCB + 8 * (a0 & 3);
                for (int guard = 0; guard < 256; ++guard) {
                    uint32_t e = Rd(ram, ram_size, link);
                    if (e == 0) break;
                    if ((e & 0x1FFFFF) == (a1 & 0x1FFFFF)) { Wr(ram, ram_size, link, Rd(ram, ram_size, e)); break; }
                    link = e;
                }
                cpu.SetGPR(2, 0);
                break;
            }
            case 0x0A: { // ChangeClearRCnt(t, flag) -> old flag
                uint32_t slot = kRcntAck + 4 * (a0 & 3);
                cpu.SetGPR(2, Rd(ram, ram_size, slot));
                Wr(ram, ram_size, slot, a1);
                break;
            }
            case 0x12: cpu.SetGPR(2, 0); break; // InstallDevices(ttyflag): devices are host-side
            default: LogUnimplemented(vec, func_code); cpu.SetGPR(2, 0); break;
        }
    }

    // Standard MIPS function return: JR $ra ($31), after any event callbacks ran.
    if (ret) FinishCall(cpu, callbacks);
    return true;
}

// ── Files (SVC-020): fd -> host copy of the disc file with a read position ───

void Ps1BiosHle::HandleFileOpen(CpuContext& cpu, const uint8_t* ram, size_t ram_size) {
    uint32_t name_ptr = cpu.GetGPR(4); // $a0
    std::string filename;

    if (ram && name_ptr != 0) {
        uint32_t phys = name_ptr & 0x1FFFFFFF;
        while (phys < ram_size && ram[phys] != 0 && filename.size() < 128) {
            filename += static_cast<char>(ram[phys++]);
        }
    }

    if (filename.size() >= 5 && filename.compare(0, 2, "bu") == 0 && filename[4] == ':' &&
        CardFileOpen(filename, cpu.GetGPR(5), cpu))
        return;
    if (m_disc_reader && !filename.empty()) {
        std::vector<uint8_t> file_bytes;
        // Strip cdrom: prefix if present
        std::string lookup_name = filename;
        if (lookup_name.rfind("cdrom:", 0) == 0) {
            lookup_name = lookup_name.substr(6);
        }
        if (lookup_name.rfind("cdrom\\", 0) == 0) {
            lookup_name = lookup_name.substr(6);
        }
        // Remove leading slashes
        while (!lookup_name.empty() && (lookup_name.front() == '/' || lookup_name.front() == '\\')) {
            lookup_name.erase(0, 1);
        }

        if (m_disc_reader->ReadFile(lookup_name, file_bytes)) {
            int fd = m_next_fd++;
            m_open_files[fd] = OpenFile{std::move(file_bytes), 0};
            PRM_LOG_INFO("BIOS", "open('" + filename + "') -> fd=" + std::to_string(fd));
            cpu.SetGPR(2, static_cast<uint32_t>(fd));
            return;
        }
    }

    // psx-spx "BIOS File Functions": open returns -1 when the file cannot be opened.
    PRM_LOG_INFO("BIOS", "open('" + filename + "') failed -> -1");
    cpu.SetGPR(2, 0xFFFFFFFFu);
}

void Ps1BiosHle::HandleFileSeek(CpuContext& cpu) {
    // lseek(fd, offset, seektype): 0 = from start, 1 = from current (psx-spx); 2 = from end (from memory).
    int64_t off = static_cast<int32_t>(cpu.GetGPR(5));
    uint32_t whence = cpu.GetGPR(6);
    if (auto c = m_card_files.find(static_cast<int>(cpu.GetGPR(4))); c != m_card_files.end()) {
        const int64_t size = int64_t(c->second.blocks.size()) * 0x2000;
        int64_t pos = (whence == 0 ? 0 : whence == 1 ? int64_t(c->second.pos) : size) + off;
        if (whence > 2 || pos < 0 || pos > size) { cpu.SetGPR(2, 0xFFFFFFFFu); return; }
        c->second.pos = static_cast<uint32_t>(pos);
        cpu.SetGPR(2, c->second.pos);
        return;
    }
    auto it = m_open_files.find(static_cast<int>(cpu.GetGPR(4)));
    if (it == m_open_files.end() || whence > 2) { cpu.SetGPR(2, 0xFFFFFFFFu); return; }
    int64_t base = whence == 0 ? 0 : whence == 1 ? int64_t(it->second.pos) : int64_t(it->second.data.size());
    int64_t pos = base + off;
    if (pos < 0) { cpu.SetGPR(2, 0xFFFFFFFFu); return; }
    it->second.pos = static_cast<size_t>(pos);
    cpu.SetGPR(2, static_cast<uint32_t>(pos));
}

void Ps1BiosHle::HandleFileRead(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    int fd = static_cast<int>(cpu.GetGPR(4));     // $a0 = fd
    uint32_t dst_ptr = cpu.GetGPR(5);           // $a1 = dst_buf
    uint32_t count = cpu.GetGPR(6);             // $a2 = count

    auto it = m_open_files.find(fd);
    if (it == m_open_files.end()) {
        cpu.SetGPR(2, 0xFFFFFFFFu);
        return;
    }
    OpenFile& f = it->second;
    size_t avail = f.pos < f.data.size() ? f.data.size() - f.pos : 0;
    size_t to_copy = std::min(static_cast<size_t>(count), avail);
    uint32_t phys = dst_ptr & 0x1FFFFFFF;
    if (!ram || phys > ram_size) to_copy = 0;
    else to_copy = std::min(to_copy, ram_size - phys);
    if (to_copy) std::memcpy(&ram[phys], f.data.data() + f.pos, to_copy);
    f.pos += to_copy;
    cpu.SetGPR(2, static_cast<uint32_t>(to_copy)); // $v0 = bytes read
    PRM_LOG_DEBUG("BIOS", "read fd=" + std::to_string(fd) + ", bytes=" + std::to_string(to_copy));
}

void Ps1BiosHle::HandleFileWrite(CpuContext& cpu, const uint8_t* ram, size_t ram_size) {
    // Only the TTY (fd 0/1, printf through libc) is writable; files on CD are read-only.
    uint32_t fd = cpu.GetGPR(4), src = cpu.GetGPR(5) & 0x1FFFFFFF, len = cpu.GetGPR(6);
    if (fd > 1) { PRM_LOG_WARN("BIOS", "write to fd " + std::to_string(fd) + " unsupported -> -1"); cpu.SetGPR(2, 0xFFFFFFFFu); return; }
    std::string text;
    for (uint32_t i = 0; ram && i < len && src + i < ram_size && text.size() < 1024; ++i) text += char(ram[src + i]);
    PRM_LOG_INFO("BIOS_TTY", text);
    cpu.SetGPR(2, len);
}

void Ps1BiosHle::HandleFileClose(CpuContext& cpu) {
    int fd = static_cast<int>(cpu.GetGPR(4));
    m_open_files.erase(fd);
    m_card_files.erase(fd);
    cpu.SetGPR(2, 0); // 0 = success
    PRM_LOG_DEBUG("BIOS", "FileClose intercepted for fd=" + std::to_string(fd));
}

void Ps1BiosHle::HandleMemCopy(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    uint32_t dst = cpu.GetGPR(4);   // $a0
    uint32_t src = cpu.GetGPR(5);   // $a1
    uint32_t count = cpu.GetGPR(6); // $a2

    if (ram && count > 0) {
        uint32_t dst_phys = dst & 0x1FFFFFFF;
        uint32_t src_phys = src & 0x1FFFFFFF;
        if (dst_phys < ram_size && src_phys < ram_size) {
            size_t copy_len = std::min(static_cast<size_t>(count),
                                       std::min(ram_size - dst_phys, ram_size - src_phys));
            std::memmove(&ram[dst_phys], &ram[src_phys], copy_len);
        }
    }

    cpu.SetGPR(2, dst); // memcpy returns dst pointer in $v0
    PRM_LOG_DEBUG("BIOS", "memcpy intercepted: dst=0x" + std::to_string(dst) +
                          " src=0x" + std::to_string(src) + " count=" + std::to_string(count));
}

void Ps1BiosHle::HandleMemSet(CpuContext& cpu, uint8_t* ram, size_t ram_size) {
    uint32_t dst = cpu.GetGPR(4);   // $a0
    uint32_t val = cpu.GetGPR(5);   // $a1
    uint32_t count = cpu.GetGPR(6); // $a2

    if (ram && count > 0) {
        uint32_t dst_phys = dst & 0x1FFFFFFF;
        if (dst_phys < ram_size) {
            size_t set_len = std::min(static_cast<size_t>(count), ram_size - dst_phys);
            std::memset(&ram[dst_phys], static_cast<int>(val & 0xFF), set_len);
        }
    }

    cpu.SetGPR(2, dst); // memset returns dst pointer in $v0
    PRM_LOG_DEBUG("BIOS", "memset intercepted: dst=0x" + std::to_string(dst) +
                          " val=" + std::to_string(val & 0xFF) + " count=" + std::to_string(count));
}

void Ps1BiosHle::HandleHeapInit(CpuContext& cpu) {
    // A(39h) InitHeap(addr, size). shortcut: the game never calls the kernel malloc (A(33h) absent,
    // FUNCTION_CATALOG), so the heap is only recorded, not built.
    uint32_t heap_base = cpu.GetGPR(4); // $a0
    uint32_t heap_size = cpu.GetGPR(5); // $a1
    cpu.SetGPR(2, 1);
    PRM_LOG_INFO("BIOS", "InitHeap intercepted: base=0x" + Hex(heap_base) +
                         " size=" + std::to_string(heap_size) + " bytes");
}

void Ps1BiosHle::HandleStdPuts(CpuContext& cpu, const uint8_t* ram, size_t ram_size) {
    uint32_t str_ptr = cpu.GetGPR(4);
    std::string msg;

    if (ram && str_ptr != 0) {
        uint32_t phys = str_ptr & 0x1FFFFFFF;
        while (phys < ram_size && ram[phys] != 0 && msg.size() < 1024) {
            msg += static_cast<char>(ram[phys++]);
        }
    }

    cpu.SetGPR(2, 0);
    PRM_LOG_INFO("BIOS_TTY", msg.empty() ? "(empty)" : msg);
}

void Ps1BiosHle::HandlePadInit(CpuContext& cpu) {
    // B(12h) InitPAD(buf1, siz1, buf2, siz2). shortcut: buffers are not filled by a BIOS pad
    // handler; the game's libpad drives SIO0 directly (FUNCTION_CATALOG "libpad").
    cpu.SetGPR(2, 1);
    PRM_LOG_INFO("BIOS", "InitPAD intercepted -> controllers registered via InputManager");
}

} // namespace personarm
