#pragma once

// p2text: dialogue tooling for Persona 2 (JP) event banks. Format notes: docs/research/TEXT_SYSTEM.md.
// Pure functions over byte buffers; never embeds game text or glyph images.
// Little-endian host assumed (same as asset_tools).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace personarm::tools::p2text {

// ── Event-bank LZ (game decompressor 0x80028DB4) ────────────────────────
// c<0x80: c+1 literals; c>=0x80: copy (c&0x7F)+3 bytes from out[-(next+1)].
/// Decodes until `out_size` bytes are produced. `consumed` = input bytes used. False on malformed input.
bool LzDecompress(const uint8_t* in, size_t n, size_t out_size, std::vector<uint8_t>& out, size_t* consumed = nullptr);
/// Greedy compressor, window 256, matches 3..130. By default never emits overlapping copies
/// (len <= distance) so a memcpy-based decoder is also safe; `allow_overlap` enables RLE-style runs.
std::vector<uint8_t> LzCompress(const uint8_t* in, size_t n, bool allow_overlap = false);

// ── Chunks / banks ──────────────────────────────────────────────────────
/// One LZ chunk (type low16 0x201 or 0x101): [u32 type][u32 size][u32 usize][lz stream], size incl. 8-byte header.
struct BankRef { size_t pos = 0; uint32_t type = 0; uint32_t size = 0; uint32_t usize = 0; };

/// Event bank = decompressed chunk: u32 0x18, 0x18, n (events), off[3]; section 1 = (u32 op, u32 ptr) pairs,
/// section 2 = operands, last section = messages (u16 codes).
struct Bank {
    std::vector<uint8_t> data;           // decompressed chunk
    std::vector<uint32_t> off;           // section offsets (header)
    size_t msg_off = 0;                  // start of message section
    std::vector<size_t> ref_ptrs;        // positions in `data` of op-19 operand words (the u32 there = message offset)
};
bool ParseBank(const std::vector<uint8_t>& dec, Bank& b);

struct Message {
    uint32_t id = 0;                     // index in section order
    uint32_t offset = 0;                 // offset inside the message section
    std::vector<uint16_t> codes;         // including the terminating 0x1103
    unsigned refs = 0;                   // number of distinct op-19 operands pointing here
};
struct Messages {
    std::vector<Message> list;
    std::vector<uint8_t> tail;           // bytes after the last terminator (kept verbatim)
    unsigned orphans = 0;                // messages no op 19 points at
    unsigned bad_refs = 0;               // op-19 operands not pointing at a message start
};
Messages SplitMessages(const Bank& b);

/// Finds all LZ chunks in a file that decode into a bank with messages.
std::vector<BankRef> FindBanks(const std::vector<uint8_t>& file, unsigned* lz_chunks_tried = nullptr);
bool DecodeBankChunk(const std::vector<uint8_t>& file, const BankRef& r, std::vector<uint8_t>& dec);

// ── Codes <-> text ──────────────────────────────────────────────────────
/// glyph id <-> single code point (UTF-8 string). Seeded with ASCII 0x20-0x7E.
struct GlyphMap {
    std::map<uint32_t, std::string> to_text;
    std::map<std::string, uint32_t> to_glyph;
    /// US release: text is packed as `0x1136, u16 = two ASCII bytes (low first)..., end at the first NUL byte`
    /// (odd length: last word has a zero high byte; even length: a 0x0000 word follows). Render/Encode then
    /// show/accept plain ASCII; a run that is not canonical stays as `<c:1136>` + raw words.
    bool packed = false;
    GlyphMap();                          // ASCII seed
    /// Lines `id<TAB>char` (id decimal or 0xHEX; '#' comments, blank lines ok). Returns false + error.
    bool Load(const std::string& path, std::string& error);
    void Set(uint32_t glyph, const std::string& utf8);
};

/// True if any message contains the packed-text control 0x1136 (US bank/file); JP never does.
bool UsesPackedText(const Messages& m);
/// Number of u16 a code occupies (controls: L of 0x1Lxx, min 1; glyphs 1).
size_t CodeLength(uint16_t c);
std::string Render(const std::vector<uint16_t>& codes, const GlyphMap& m);
/// Parses tagged text; appends to `out`. On failure returns false and fills `errors` (one per problem).
bool Encode(const std::string& text, const GlyphMap& m, std::vector<uint16_t>& out, std::vector<std::string>& errors);

// ── Rebuild ─────────────────────────────────────────────────────────────
struct RebuildReport {
    size_t old_msg_bytes = 0, new_msg_bytes = 0;
    size_t old_dec = 0, new_dec = 0;
    unsigned changed = 0, moved_orphans = 0;
    std::vector<std::string> warnings;
};
/// Replaces message section with `msgs` (same count/order as SplitMessages), fixes op-19 operands.
bool RebuildBank(const Bank& b, const Messages& orig, const std::vector<std::vector<uint16_t>>& msgs,
                 std::vector<uint8_t>& out_dec, RebuildReport& rep, std::string& error);
/// Writes `new_dec` as the chunk `r` of `file` (recompressed), re-padding the gap after it like the original.
/// With `keep_original_if_same`, an unchanged decompressed bank keeps the original chunk bytes.
bool PatchFile(const std::vector<uint8_t>& file, const BankRef& r, const std::vector<uint8_t>& new_dec,
               bool keep_original_if_same, std::vector<uint8_t>& out, std::string& error);

// ── Layout (overflow warnings) ──────────────────────────────────────────
/// Text-window metrics. JP: every glyph code advances a fixed 12 px. US: `adv[c - 0x20]` px per ASCII char, read at run
/// time from the user's own boot executable (the game's width table lives there; it is never embedded here).
struct Metrics {
    bool us = false;
    uint8_t adv[96] = {};                // US advance (ink width + 1 px) for 0x20..0x7F
    unsigned max_px = 0;                 // widest line, from the line origin (leading space included)
    unsigned max_lines = 0;              // lines per page, speaker-name line included
};
/// US widths from a PS-X EXE image (`SLUS_011.58`): 96-byte class index + 16 (left, width) pairs, see TRANSLATION_TOOLS.md.
bool UsMetricsFromExe(const std::vector<uint8_t>& exe, Metrics& m, std::string& error);
/// JP/US default window limits (measured on the game's own text window).
Metrics DefaultMetrics(bool us);
struct LineWidth { unsigned page = 0, line = 0, px = 0; };    // 1-based page/line within the message
/// Width of every line (split at <br>/<page>); the message's last line is closed by <end>.
std::vector<LineWidth> MeasureLines(const std::vector<uint16_t>& codes, const Metrics& m);
/// One warning per line wider than the window and one per page with too many lines.
std::vector<std::string> CheckLayout(const std::vector<uint16_t>& codes, const Metrics& m, const std::string& where);

// ── Font preview ────────────────────────────────────────────────────────
/// 8-bit grayscale BMP sheet of full-width glyphs [first, first+count) from the 1 bpp font
/// (full-width base +0x480, 18 B/glyph, 12x12, bit 0 = left pixel), 32 per row, scaled x2.
std::vector<uint8_t> FontSheetBmp(const std::vector<uint8_t>& font, unsigned first, unsigned count);

} // namespace personarm::tools::p2text
