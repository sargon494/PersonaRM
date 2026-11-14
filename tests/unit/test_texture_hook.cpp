// VRAM texture dump / replacement hook: synthetic uploads + textured rectangles through the real GPU.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <vector>
#include "loader/png_codec.h"
#include "rendering/ps1_gpu.h"
#include "rendering/vram_texture_hook.h"

using namespace personarm;
namespace fs = std::filesystem;

static int g_failures = 0, g_checks = 0;
#define CHECK(name, cond)                                         \
    do {                                                          \
        ++g_checks;                                               \
        if (!(cond)) { ++g_failures; std::printf("  FAIL %s\n", name); } \
    } while (0)
#define CHECK_EQ(name, got, want)                                                         \
    do {                                                                                  \
        ++g_checks;                                                                       \
        uint32_t g_ = (got), w_ = (want);                                                 \
        if (g_ != w_) { ++g_failures; std::printf("  FAIL %s: 0x%08X want 0x%08X\n", name, g_, w_); } \
    } while (0)

static void Send(Ps1Gpu& gpu, std::initializer_list<uint32_t> words) { for (uint32_t w : words) gpu.WriteGP0(w); }
static uint32_t XY(int x, int y) { return (uint32_t(x) & 0xFFFF) | (uint32_t(y) << 16); }
static uint16_t Gray(int v5) { return uint16_t(v5 * 0x421); }

static void Upload(Ps1Gpu& gpu, int x, int y, int w, int h, const std::vector<uint16_t>& px) {
    Send(gpu, {0xA0000000, XY(x, y), XY(w, h)});
    for (size_t i = 0; i < px.size(); i += 2) gpu.WriteGP0(px[i] | (i + 1 < px.size() ? uint32_t(px[i + 1]) << 16 : 0));
}

// Draws the 4-bit (8x1), 8-bit (4x1) and 15-bit (2x1) textures at y=100/110/120, x=100.
struct Scene { uint32_t hash4 = 0, hash8 = 0, hash15 = 0; };
static Scene Run(Ps1Gpu& gpu) {
    Scene s;
    Send(gpu, {0xE3000000, 0xE4000000 | 1023 | (511 << 10), 0xE5000000, 0xE1000000});
    for (int i = 0; i < 16; ++i) gpu.WriteVram(i, 100, i ? Gray(i) : 0);            // 4-bit CLUT row
    for (int i = 0; i < 256; ++i) gpu.WriteVram(i, 101, i ? Gray(i % 31 + 1) : 0);  // 8-bit CLUT row
    Upload(gpu, 640, 0, 2, 1, {0x3210, 0x7654});
    Upload(gpu, 704, 0, 2, 1, {0x0201, 0x0403});
    Upload(gpu, 768, 0, 2, 1, {0x7C00, 0x03E0});
    s.hash4 = VramTextureHook::HashUpload(gpu.GetVram(), 640, 0, 2, 1);
    s.hash8 = VramTextureHook::HashUpload(gpu.GetVram(), 704, 0, 2, 1);
    s.hash15 = VramTextureHook::HashUpload(gpu.GetVram(), 768, 0, 2, 1);
    Send(gpu, {0xE100000A, 0x65000000, XY(100, 100), uint32_t(100 << 6) << 16, XY(8, 1)});
    Send(gpu, {0xE100008B, 0x65000000, XY(100, 110), uint32_t(101 << 6) << 16, XY(4, 1)});
    Send(gpu, {0xE100010C, 0x65000000, XY(100, 120), 0, XY(2, 1)});
    return s;
}

static std::string Hex(uint32_t h) { char b[16]; std::snprintf(b, sizeof b, "%08X", h); return b; }

static void Expect(const std::string& file, uint32_t w, uint32_t h, const std::vector<uint16_t>& texels) {
    auto png = PngCodec::DecodeFile(file);
    CHECK(("dump exists " + file).c_str(), png.has_value());
    if (!png) return;
    CHECK_EQ("dump w", png->width, w);
    CHECK_EQ("dump h", png->height, h);
    for (size_t i = 0; i < texels.size() && i < w * h; ++i) {
        const uint8_t* p = &png->rgba[i * 4];
        auto e = [](uint32_t c) { return uint32_t((c << 3) | (c >> 2)); };
        const uint16_t t = texels[i];
        CHECK_EQ("dump alpha", p[3], t ? 255 : 0);
        if (t) {
            CHECK_EQ("dump r", p[0], e(t & 31));
            CHECK_EQ("dump g", p[1], e((t >> 5) & 31));
            CHECK_EQ("dump b", p[2], e((t >> 10) & 31));
        }
    }
}

int main() {
    const fs::path root = fs::temp_directory_path() / "prm_texhook_test";
    fs::remove_all(root);

    // Hash: stable, depends on position and contents.
    {
        Ps1Gpu a, b;
        Upload(a, 10, 20, 2, 1, {1, 2});
        Upload(b, 10, 20, 2, 1, {1, 2});
        CHECK_EQ("hash stable", VramTextureHook::HashUpload(a.GetVram(), 10, 20, 2, 1),
                 VramTextureHook::HashUpload(b.GetVram(), 10, 20, 2, 1));
        CHECK("hash pos", VramTextureHook::HashUpload(a.GetVram(), 10, 20, 2, 1) !=
                              VramTextureHook::HashUpload(a.GetVram(), 11, 20, 2, 1));
        Upload(b, 10, 20, 2, 1, {1, 3});
        CHECK("hash data", VramTextureHook::HashUpload(a.GetVram(), 10, 20, 2, 1) !=
                               VramTextureHook::HashUpload(b.GetVram(), 10, 20, 2, 1));
        // The same upload twice is one entry.
        VramTextureHook hook;
        a.SetTextureHook(&hook);
        Upload(a, 10, 20, 2, 1, {1, 2});
        Upload(a, 10, 20, 2, 1, {1, 2});
        CHECK_EQ("dedup", hook.UploadCount(), 1);
    }

    // Baseline VRAM, no hook.
    Ps1Gpu base;
    const Scene sc = Run(base);

    // Hook installed but nothing configured: VRAM identical.
    {
        Ps1Gpu g;
        VramTextureHook hook;
        g.SetTextureHook(&hook);
        Run(g);
        CHECK("idle hook identical", g.GetVram() == base.GetVram());
        CHECK_EQ("idle dumped none", hook.DumpedCount(), 0);
    }

    // Dump: one PNG per used upload, correct decoded pixels, VRAM unchanged.
    {
        Ps1Gpu g;
        VramTextureHook hook;
        hook.SetDumpDir((root / "dump").string());
        g.SetTextureHook(&hook);
        Run(g);
        Run(g);  // second run re-uploads the same data: still one PNG per hash
        CHECK("dump identical", g.GetVram() == base.GetVram());
        CHECK_EQ("dumped count", hook.DumpedCount(), 3);
        size_t files = 0;
        for (auto& e : fs::directory_iterator(root / "dump")) { (void)e; ++files; }
        CHECK_EQ("dump files", files, 3);
        Expect((root / "dump" / (Hex(sc.hash4) + "_4bit_8x1.png")).string(), 8, 1,
               {0, Gray(1), Gray(2), Gray(3), Gray(4), Gray(5), Gray(6), Gray(7)});
        Expect((root / "dump" / (Hex(sc.hash8) + "_8bit_4x1.png")).string(), 4, 1,
               {Gray(2), Gray(3), Gray(4), Gray(5)});
        Expect((root / "dump" / (Hex(sc.hash15) + "_15bit_2x1.png")).string(), 2, 1, {0x7C00, 0x03E0});
    }

    // Replace: 8x1 4-bit texture -> 16x2 HD PNG, left half opaque red, right half transparent.
    {
        const fs::path tex = root / "mods" / "m" / "textures";
        fs::create_directories(tex);
        std::vector<uint8_t> hd(16 * 2 * 4, 0);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 8; ++x) { uint8_t* p = &hd[(y * 16 + x) * 4]; p[0] = 255; p[3] = 255; }
        PngCodec::EncodeToFile((tex / (Hex(sc.hash4) + ".png")).string(), 16, 2, hd.data());

        Ps1Gpu g;
        VramTextureHook hook;
        CHECK_EQ("indexed", hook.LoadMods((root / "mods").string()), 1);
        g.SetTextureHook(&hook);
        Run(g);
        for (int x = 0; x < 4; ++x) CHECK_EQ("replaced red", g.ReadVram(100 + x, 100), 0x001F);
        for (int x = 4; x < 8; ++x) CHECK_EQ("transparent keeps bg", g.ReadVram(100 + x, 100), 0);
        for (int y : {110, 120})
            for (int x = 0; x < 4; ++x)
                CHECK_EQ("other textures untouched", g.ReadVram(100 + x, y), base.ReadVram(100 + x, y));
        CHECK_EQ("replaced uploads", hook.ReplacedUploadCount(), 1);

        // Higher-priority mod wins: green PNG for the same hash.
        const fs::path tex2 = root / "mods" / "z" / "textures";
        fs::create_directories(tex2);
        std::vector<uint8_t> green(2 * 1 * 4, 0);
        green[1] = green[3] = green[5] = green[7] = 255;
        PngCodec::EncodeToFile((tex2 / (Hex(sc.hash4) + ".png")).string(), 2, 1, green.data());
        std::FILE* f = std::fopen((root / "mods" / "z" / "manifest.json").string().c_str(), "wb");
        std::fputs("{\"priority\": 10}", f);
        std::fclose(f);
        Ps1Gpu g2;
        VramTextureHook hook2;
        hook2.LoadMods((root / "mods").string());
        g2.SetTextureHook(&hook2);
        Run(g2);
        CHECK_EQ("priority wins", g2.ReadVram(100, 100), 0x03E0);

        // Overwriting the uploaded texels (quick fill) detaches the tag: no replacement afterwards.
        Ps1Gpu g3;
        VramTextureHook hook3;
        hook3.LoadMods((root / "mods").string());
        g3.SetTextureHook(&hook3);
        Run(g3);
        g3.FillVramRect(640, 0, 2, 1, 0x1111);
        Send(g3, {0xE100000A, 0x65000000, XY(100, 200), uint32_t(100 << 6) << 16, XY(8, 1)});
        CHECK_EQ("fill drops replacement", g3.ReadVram(100, 200), Gray(1));  // index 1 of 0x1111 = CLUT[1]
    }

    fs::remove_all(root);
    std::printf("Texture hook: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
