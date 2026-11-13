#pragma once
// PS1 'BS' (STR frame) bitstream v1/v2/v3 -> MDEC(1) run-length halfwords.
// Source: psx-spx "CDROM File Video BS Picture Compression" (BS header, AC Huffman table,
// v3 DC table) in https://psx-spx.consoledev.net/cdromfileformats/.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace personarm::bs {

struct Header {
    uint16_t mdec_words = 0;  // MDEC data size / 4 (rounded up to 80h bytes)
    uint16_t magic = 0;       // 3800h
    uint16_t qscale = 0;      // 0..3Fh, goes to bits 15-10 of every DC halfword
    uint16_t version = 0;     // 1, 2 or 3
};

bool ReadHeader(const uint8_t* bs, size_t size, Header& out);

/// Converts one BS frame (starting at its 8-byte header) to the MDEC run-length stream
/// (Cr,Cb,Y1..Y4 per macroblock, each block = DC halfword, AC halfwords, FE00h), padded with
/// FE00h to a multiple of 64 halfwords (80h bytes). Returns false on a bad header or code.
bool ToMdec(const uint8_t* bs, size_t size, uint16_t width, uint16_t height, std::vector<uint16_t>& out);

/// One AC Huffman code without its trailing sign bit (sign 1 = negative level).
struct AcCode {
    uint16_t bits;  // code, right-aligned
    uint8_t len;
    uint16_t mdec;  // run<<10 | level
};
/// The psx-spx AC table expanded (EOB "10" and escape "000001" not included).
const std::vector<AcCode>& AcCodes();

} // namespace personarm::bs
