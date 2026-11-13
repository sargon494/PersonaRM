// Build step for unit_test_recomp_g2: writes the synthetic program as a raw image plus its entry
// list, which tools/recomp_gen then turns into C++.
#include <cstdio>
#include <fstream>
#include <string>

#include "g2_synth_program.h"

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: g2_synth_writer <out.bin> <out.entries>\n"); return 2; }
    const g2::Program p = g2::Build();
    std::ofstream bin(argv[1], std::ios::binary);
    for (uint32_t w : p.words) {
        const char b[4] = {static_cast<char>(w), static_cast<char>(w >> 8), static_cast<char>(w >> 16), static_cast<char>(w >> 24)};
        bin.write(b, 4);
    }
    std::ofstream ent(argv[2]);
    for (uint32_t e : p.entries) ent << "0x" << std::hex << e << "\n";
    return (bin && ent) ? 0 : 1;
}
