#pragma once

// Asset/format helpers for psx_analyze: FILEPOS.DAT, F*.BIN chunk streams, RLE, load-delay hazards.
// Format notes: docs/research/ASSET_FORMATS.md (AF-001..005). Pure functions over byte buffers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace personarm::tools {

struct FileposEntry { uint32_t lba = 0; uint32_t size = 0; };

/// FILEPOS.DAT: array of little-endian {u32 lba, u32 size}; trailing partial entry ignored.
std::vector<FileposEntry> ParseFilepos(const uint8_t* data, size_t size);

struct Chunk { uint32_t type = 0; uint32_t size = 0; size_t offset = 0; };

/// Walks [u32 type][u32 size incl. header] chunks, 4-byte aligned. Zero padding up to the next
/// 0x800 boundary (or to the end) is skipped. Returns false and sets `error` on a malformed chunk.
bool WalkChunks(const uint8_t* data, size_t size, std::vector<Chunk>& out, std::string& error);

/// PackBits-style RLE: c<0x80 copies c+1 literals, c>=0x80 repeats the next byte (c&0x7F)+3 times.
/// Returns false when the input ends in the middle of a run.
bool RleDecode(const uint8_t* in, size_t size, std::vector<uint8_t>& out);

struct Hazard { uint32_t addr = 0; uint32_t load = 0; uint32_t next = 0; };

/// Load-delay hazards (LB/LH/LW/LBU/LHU whose rt is read by the next instruction; LWL/LWR exempt).
/// With `use_entry`, only code reachable from `entry` through direct control flow is scanned;
/// otherwise every word is scanned linearly (may include data). Delay-slot loads are checked
/// against the next word in memory, not the branch target (known limit).
std::vector<Hazard> ScanHazards(const uint8_t* data, size_t size, uint32_t base,
                                bool use_entry, uint32_t entry);

/// Commands behind psx_analyze --filepos / --extract-file. Return a process exit code.
int RunFilepos(const std::string& disc);
int RunExtractFile(const std::string& disc, uint32_t index, const std::string& out_path);

} // namespace personarm::tools
