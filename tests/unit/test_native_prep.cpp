// personarm_native overlay discovery (tools/personarm_native/native_prep.h) on a synthetic overlay.
#undef NDEBUG
#include <cstdio>
#include <vector>

#include "native_prep.h"

using namespace personarm::native;

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

void Put(std::vector<uint8_t>& img, size_t off, uint32_t w) {
    for (int b = 0; b < 4; ++b) img[off + b] = static_cast<uint8_t>(w >> (8 * b));
}
uint32_t Jal(uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu); }

} // namespace

int main() {
    // Eight functions of 0x40 bytes (prologue, jal to the next function, jr ra; nop), meant for 0x80093000.
    constexpr uint32_t kBase = 0x80093000u;
    std::vector<uint8_t> img(8 * 0x40, 0);
    for (uint32_t f = 0; f < 8; ++f) {
        const size_t o = f * 0x40;
        Put(img, o, 0x27BDFFE8u);                                   // addiu sp,sp,-24
        Put(img, o + 4, Jal(kBase + ((f + 1) % 8) * 0x40));         // jal next
        Put(img, o + 16, Jal(kBase + ((f + 3) % 8) * 0x40));        // jal another
        Put(img, o + 0x38, 0x03E00008u);                            // jr ra
    }
    const std::vector<uint32_t> starts = FunctionStarts(img.data(), img.size(), kBase);
    CHECK(starts.size() == 8 && starts[0] == kBase && starts[7] == kBase + 7 * 0x40);

    const BaseGuess g = InferOverlayBase(img.data(), img.size(), 0x80020000u, 0x80200000u);
    CHECK(g.accepted && g.base == kBase && g.hits == 16 && g.in_range == 16);

    // Below the allowed range: never guessed there.
    const BaseGuess low = InferOverlayBase(img.data(), img.size(), 0x800A0000u, 0x80200000u);
    CHECK(!(low.accepted && low.base == kBase));

    // Data (no calls): not code.
    const std::vector<uint8_t> data(4096, 0x11);
    CHECK(!InferOverlayBase(data.data(), data.size(), 0x80020000u, 0x80200000u).accepted);

    if (g_fail) { std::fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    std::printf("native prep: ok\n");
    return 0;
}
