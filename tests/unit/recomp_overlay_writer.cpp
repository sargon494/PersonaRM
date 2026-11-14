// Build step for unit_test_recomp_engine: writes the synthetic overlay program (resident main and the
// two overlays of one slot) as raw images plus entry lists, which tools/recomp_gen then turns into
// C++ (modules "rovl_main", "rovl_a", "rovl_b").
#include <cstdio>
#include <fstream>
#include <string>

#include "recomp_overlay_program.h"

static bool Write(const g2::Program& p, const std::string& stem) {
    std::ofstream bin(stem + ".bin", std::ios::binary);
    for (uint32_t w : p.words) {
        const char b[4] = {static_cast<char>(w), static_cast<char>(w >> 8), static_cast<char>(w >> 16), static_cast<char>(w >> 24)};
        bin.write(b, 4);
    }
    std::ofstream ent(stem + ".entries");
    for (uint32_t e : p.entries) ent << "0x" << std::hex << e << "\n";
    return bin && ent;
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: recomp_overlay_writer <out-stem>\n"); return 2; }
    const rovl::Set s = rovl::Build();
    const std::string stem = argv[1];
    return Write(s.main, stem + "_main") && Write(s.a, stem + "_a") && Write(s.b, stem + "_b") ? 0 : 1;
}
