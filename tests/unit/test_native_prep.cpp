// personarm_native overlay discovery (tools/personarm_native/native_prep.h) on a synthetic overlay.
#undef NDEBUG
#include <algorithm>
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

    // AddGnuStackHeader on TinyCC's layout: ELF header, 2 program headers, then 3 section headers,
    // then section data at 0x200.
    {
        auto put = [](std::vector<uint8_t>& v, size_t off, int n, uint64_t x) { for (int i = 0; i < n; ++i) v[off + i] = uint8_t(x >> (8 * i)); };
        auto get = [](const std::vector<uint8_t>& v, size_t off, int n) { uint64_t x = 0; for (int i = n - 1; i >= 0; --i) x = x << 8 | v[off + i]; return x; };
        std::vector<uint8_t> elf(0x300, 0);
        elf[0] = 0x7F, elf[1] = 'E', elf[2] = 'L', elf[3] = 'F', elf[4] = 2, elf[5] = 1;
        put(elf, 32, 8, 64);                    // e_phoff
        put(elf, 40, 8, 64 + 2 * 56);           // e_shoff, right after the program headers
        put(elf, 54, 2, 56), put(elf, 56, 2, 2), put(elf, 58, 2, 64), put(elf, 60, 2, 3);
        put(elf, 64, 4, 1), put(elf, 64 + 56, 4, 1);              // two PT_LOAD
        const size_t sh = 64 + 2 * 56;
        put(elf, sh + 64 + 4, 4, 1), put(elf, sh + 64 + 24, 8, 0x200), put(elf, sh + 64 + 32, 8, 0x80);   // PROGBITS at 0x200
        put(elf, sh + 128 + 4, 4, 1), put(elf, sh + 128 + 24, 8, 0x280), put(elf, sh + 128 + 32, 8, 0x80);
        const std::vector<uint8_t> old_sh(elf.begin() + sh, elf.begin() + sh + 3 * 64);
        CHECK(AddGnuStackHeader(elf));
        CHECK(get(elf, 56, 2) == 3);
        CHECK(get(elf, 64 + 2 * 56, 4) == 0x6474E551u && get(elf, 64 + 2 * 56 + 4, 4) == 6);   // RW, not X
        const size_t new_sh = get(elf, 40, 8);
        CHECK(new_sh >= 0x300 && new_sh % 8 == 0 && elf.size() == new_sh + 3 * 64);
        CHECK(std::equal(old_sh.begin(), old_sh.end(), elf.begin() + new_sh));
        const size_t size = elf.size();
        CHECK(AddGnuStackHeader(elf) && elf.size() == size && get(elf, 56, 2) == 3);   // already there: unchanged
        // No room (section data right after the program headers): refused.
        std::vector<uint8_t> tight(old_sh.size() + 0x300, 0);
        std::copy(elf.begin(), elf.begin() + 64, tight.begin());
        put(tight, 56, 2, 2), put(tight, 40, 8, 0x200);
        std::copy(old_sh.begin(), old_sh.end(), tight.begin() + 0x200);
        put(tight, 0x200 + 64 + 24, 8, 64 + 2 * 56);   // section 1 data starts where a 3rd entry would go
        CHECK(!AddGnuStackHeader(tight) && get(tight, 56, 2) == 2);
        std::vector<uint8_t> junk(100, 0);
        CHECK(!AddGnuStackHeader(junk));
    }

    if (g_fail) { std::fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    std::printf("native prep: ok\n");
    return 0;
}
