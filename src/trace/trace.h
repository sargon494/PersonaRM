#pragma once

// PRMTRACE v1: event-aligned JSONL execution trace (see docs/TRACE_FORMAT.md).
// Pure C++17. No singletons: the caller owns the stream and the writer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/json_lite.h"
#include "cpu/cpu_context.h" // CpuRegisters only (read-only use)

namespace personarm::trace {

constexpr int kTraceVersion = 1;

/// Half-open physical address range [begin, end).
using AddrRange = std::pair<uint32_t, uint32_t>;

/// Default RAM mask: kernel/BIOS area 0x00000000-0x0000FFFF (HLE BIOS vs real BIOS).
std::vector<AddrRange> DefaultRamMasks();

/// FNV-1a 64-bit.
uint64_t Fnv1a64(const uint8_t* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

/// "%016llx" (lower-case, no prefix).
std::string Hex64(uint64_t v);
/// "0x%08x" (lower-case).
std::string Hex32(uint32_t v);

/// FNV-1a 64 over RAM where every byte inside a mask range is replaced by 0.
std::string HashMaskedRam(const uint8_t* ram, size_t size, const std::vector<AddrRange>& masks);
/// FNV-1a 64 over r0..r31, hi, lo (34 x u32, little endian; r0 forced to 0).
std::string HashGpr(const CpuRegisters& regs);
/// FNV-1a 64 over raw bytes (scratchpad, VRAM).
std::string HashBytes(const uint8_t* data, size_t size);

struct TraceHeader {
    std::string src;             // "prm" | "redux"
    std::string build;           // git sha / redux version
    std::string disc;            // disc serial
    std::string exe_sha256;      // hex
    std::string mode = "events"; // "events" | "verbose"
    std::vector<AddrRange> ram_masks = DefaultRamMasks();
};

/// What the engine/CPU/BIOS-HLE call. TraceWriter implements it; a null sink means tracing off.
class ITraceSink {
public:
    virtual ~ITraceSink() = default;
    virtual void OnBiosCall(uint32_t vec, uint32_t fn, const std::array<uint32_t, 4>& a, uint32_t ra) = 0;
    virtual void OnLoadFile(uint32_t file_id, uint32_t dest, uint32_t ra) = 0;
    virtual void OnCdRead(uint32_t lba, uint32_t sectors, uint32_t dest) = 0;
    /// Empty gp0_hash => gp0 fields omitted (Redux cannot supply them).
    virtual void OnVsync(uint32_t frame, const std::string& gp0_hash, uint32_t gp0_words) = 0;
    /// Empty hash => field omitted.
    virtual void OnCheckpoint(const std::string& tag, uint32_t pc, const std::string& gpr_hash,
                              const std::string& ram_hash, const std::string& scratch_hash,
                              const std::string& vram_hash) = 0;
    virtual void OnIrq(const std::string& src, uint32_t pc) = 0;
    virtual void OnMmio(uint32_t addr, uint32_t width, bool write, uint32_t value) = 0;
    virtual void OnEnd(const std::string& reason, uint32_t frames) = 0;
};

class TraceWriter final : public ITraceSink {
public:
    /// Writes the header line immediately. `out` must outlive the writer.
    TraceWriter(std::ostream& out, const TraceHeader& header);
    /// Flushes a pending collapsed `call` (TRACE_FORMAT.md section 3.1).
    ~TraceWriter() override;

    void OnBiosCall(uint32_t vec, uint32_t fn, const std::array<uint32_t, 4>& a, uint32_t ra) override;
    void OnLoadFile(uint32_t file_id, uint32_t dest, uint32_t ra) override;
    void OnCdRead(uint32_t lba, uint32_t sectors, uint32_t dest) override;
    void OnVsync(uint32_t frame, const std::string& gp0_hash, uint32_t gp0_words) override;
    void OnCheckpoint(const std::string& tag, uint32_t pc, const std::string& gpr_hash,
                      const std::string& ram_hash, const std::string& scratch_hash,
                      const std::string& vram_hash) override;
    void OnIrq(const std::string& src, uint32_t pc) override;
    void OnMmio(uint32_t addr, uint32_t width, bool write, uint32_t value) override;
    void OnEnd(const std::string& reason, uint32_t frames) override;

    /// Convenience: checkpoint from live state. Null scratch/vram => hash omitted.
    void Checkpoint(const std::string& tag, const CpuRegisters& regs, const uint8_t* ram, size_t ram_size,
                    const uint8_t* scratch, size_t scratch_size, const uint8_t* vram, size_t vram_size);

private:
    void Emit(JsonValue rec);
    void FlushCall();
    std::ostream& m_out;
    // Consecutive identical `call` events collapse into one with "n" = repeat count (section 3.1).
    JsonValue m_call;
    uint32_t m_call_n = 0;
    std::vector<AddrRange> m_masks;
    uint64_t m_seq = 0;
};

struct TraceFile {
    JsonValue header;
    std::vector<JsonValue> events; // each: object with "seq" and "ev"
};

/// Parse a whole trace. Returns false and fills `error` ("line N: ...") on the first malformed line.
/// Rules: line 1 is a header with v==1; every other non-empty line is an object with string "ev" and
/// integer "seq" strictly greater than the previous one. Unknown "ev" values are accepted.
bool ReadTrace(std::istream& in, TraceFile* out, std::string* error);

struct CompareOptions {
    /// Event types dropped from both sides before alignment (timing-dependent by default).
    std::set<std::string> ignore_events = {"irq", "mmio"};
    size_t context = 5; // events of context shown around the divergence
    /// Compare the "n" repeat count of collapsed `call` runs. Off by default: polling counts
    /// (TestEvent loops) depend on emulation timing.
    bool compare_repeat_counts = false;
    /// Event fields never compared and not part of a `call`'s identity when merging runs (e.g. "a3":
    /// a register the callee does not read, left over from the real vs HLE BIOS).
    std::set<std::string> ignore_fields;
};

struct Divergence {
    bool found = false;
    size_t index = 0;   // position in the filtered event streams (0-based)
    std::string reason; // human-readable, names the differing field
    std::vector<std::string> context_a, context_b; // compact JSON lines around index
};

/// First divergence between two traces, aligned by event order (seq is ignored; see TRACE_FORMAT.md).
Divergence CompareTraces(const TraceFile& a, const TraceFile& b, const CompareOptions& opt = {});

/// Re-synchronising comparison: each trace is cut before every `sync_ev` event (e.g. "load_file"), and
/// segment i of A is compared with segment i of B. One result per segment (max of both counts; a missing
/// segment compares as empty). Indices are relative to the segment.
std::vector<Divergence> CompareTracesBySegment(const TraceFile& a, const TraceFile& b, const std::string& sync_ev,
                                               const CompareOptions& opt = {});

} // namespace personarm::trace
