#include "rendering/vram_texture_hook.h"

#include "core/hash.h"
#include "core/json_lite.h"
#include "core/logger.h"
#include "loader/png_codec.h"
#include "rendering/ps1_gpu.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace personarm {
namespace fs = std::filesystem;

namespace {
constexpr uint32_t kW = PS1_VRAM_WIDTH, kH = PS1_VRAM_HEIGHT;

uint32_t PixelsPerWord(uint32_t mode) { return mode == 0 ? 4 : mode == 1 ? 2 : 1; }

// BGR555 -> RGBA bytes; 0000h is the transparent texel.
void Rgba(uint16_t t, uint8_t* o) {
    auto e = [](uint32_t c) { return uint8_t((c << 3) | (c >> 2)); };
    o[0] = e(t & 31); o[1] = e((t >> 5) & 31); o[2] = e((t >> 10) & 31); o[3] = t ? 255 : 0;
}
} // namespace

VramTextureHook::VramTextureHook() : m_tags(size_t(kW) * kH, 0) {}

uint32_t VramTextureHook::HashUpload(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint16_t head[4] = {uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h)};
    std::vector<uint8_t> bytes;
    bytes.reserve(8 + size_t(w) * h * 2);
    auto put = [&](uint16_t v) { bytes.push_back(uint8_t(v)); bytes.push_back(uint8_t(v >> 8)); };
    for (uint16_t v : head) put(v);
    for (uint32_t cy = 0; cy < h; ++cy)
        for (uint32_t cx = 0; cx < w; ++cx) put(vram[((y + cy) & (kH - 1)) * kW + ((x + cx) & (kW - 1))]);
    return Fnv1a32(bytes.data(), bytes.size());
}

void VramTextureHook::OnUpload(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!w || !h) return;
    const uint32_t hash = HashUpload(vram, x, y, w, h);
    size_t id = 0;
    for (size_t i = 0; i < m_uploads.size(); ++i)
        if (m_uploads[i].hash == hash) { id = i + 1; break; }  // shortcut: linear scan, fine for thousands of uploads
    if (!id) {
        m_uploads.push_back(Upload{x, y, w, h, hash});
        id = m_uploads.size();
    }
    for (uint32_t cy = 0; cy < h; ++cy)
        for (uint32_t cx = 0; cx < w; ++cx) m_tags[((y + cy) & (kH - 1)) * kW + ((x + cx) & (kW - 1))] = uint32_t(id);
}

void VramTextureHook::Invalidate(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    for (uint32_t cy = 0; cy < h; ++cy)
        for (uint32_t cx = 0; cx < w; ++cx) m_tags[((y + cy) & (kH - 1)) * kW + ((x + cx) & (kW - 1))] = 0;
}

void VramTextureHook::ClearTags() { std::fill(m_tags.begin(), m_tags.end(), 0u); }

size_t VramTextureHook::LoadMods(const std::string& dir) {
    m_mods.clear();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        PRM_LOG_WARN("TextureHook", "Mods directory not found: " + dir);
        return 0;
    }
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());
    size_t total = 0;
    for (const fs::path& f : folders) {
        auto mod = std::make_unique<Mod>();
        mod->name = f.filename().string();
        std::string sub = "textures";
        std::ifstream in(f / "manifest.json");
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::string err;
            auto json = JsonValue::Parse(ss.str(), &err);
            if (!json || !json->IsObject()) {
                PRM_LOG_WARN("TextureHook", "Ignoring invalid manifest of mod " + mod->name + ": " + err);
            } else {
                mod->priority = int(json->NumberOr("priority", 0));
                sub = json->StringOr("textures", sub);
                const JsonValue* serial = json->Find("serial");
                if (serial && serial->IsArray() && !m_serial.empty()) {
                    bool match = false;
                    for (const auto& s : serial->Items()) match = match || s.AsString() == m_serial;
                    if (!match) continue;  // mod is for other discs
                }
            }
        }
        const fs::path tex = f / sub;
        if (sub.find("..") != std::string::npos || !fs::is_directory(tex, ec)) continue;
        total += mod->textures.LoadDirectory(tex.string());
        m_mods.push_back(std::move(mod));
    }
    std::stable_sort(m_mods.begin(), m_mods.end(),
                     [](const auto& a, const auto& b) { return a->priority > b->priority; });
    return total;
}

void VramTextureHook::Resolve(Upload& u) {
    u.resolved = true;
    const Mod* first = nullptr;
    for (auto& m : m_mods) {
        if (!m->textures.HasOverride(u.hash)) continue;
        if (!first) {
            u.hd = m->textures.FindOverride(u.hash);
            if (u.hd) { first = m.get(); ++m_replaced; }
        } else {
            PRM_LOG_WARN("TextureHook", "Texture " + TextureOverrideManager::HashToHex(u.hash) + " in mod " +
                                            m->name + " ignored (lower priority than " + first->name + ")");
        }
    }
}

void VramTextureHook::Dump(const std::vector<uint16_t>& vram, const Upload& u, uint32_t mode, uint32_t clut_x,
                           uint32_t clut_y) {
    const uint32_t ppw = PixelsPerWord(mode), W = u.w * ppw, H = u.h;
    std::vector<uint8_t> rgba(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint16_t word = vram[((u.y + y) & (kH - 1)) * kW + ((u.x + x / ppw) & (kW - 1))];
            uint16_t t = word;
            if (mode == 0) t = vram[(clut_y & (kH - 1)) * kW + ((clut_x + ((word >> ((x & 3) * 4)) & 0xF)) & (kW - 1))];
            else if (mode == 1) t = vram[(clut_y & (kH - 1)) * kW + ((clut_x + ((word >> ((x & 1) * 8)) & 0xFF)) & (kW - 1))];
            Rgba(t, &rgba[(size_t(y) * W + x) * 4]);
        }
    std::error_code ec;
    fs::create_directories(m_dump_dir, ec);
    char name[64];
    std::snprintf(name, sizeof name, "%08X_%ubit_%ux%u.png", u.hash, mode == 0 ? 4u : mode == 1 ? 8u : 15u, W, H);
    if (PngCodec::EncodeToFile((fs::path(m_dump_dir) / name).string(), W, H, rgba.data())) ++m_dumped;
}

uint16_t VramTextureHook::Texel(const std::vector<uint16_t>& vram, uint32_t wx, uint32_t wy, uint32_t mode,
                                uint32_t sub, uint32_t clut_x, uint32_t clut_y, uint16_t orig) {
    const uint32_t id = m_tags[(wy & (kH - 1)) * kW + (wx & (kW - 1))];
    if (!id) return orig;
    Upload& u = m_uploads[id - 1];
    if (!m_dump_dir.empty() && !u.dumped) {
        u.dumped = true;
        Dump(vram, u, mode, clut_x, clut_y);
    }
    if (!u.resolved) Resolve(u);
    if (!u.hd) return orig;

    const uint32_t ppw = PixelsPerWord(mode), W = u.w * ppw, H = u.h;
    const uint32_t px = ((wx - u.x) & (kW - 1)) * ppw + sub, py = (wy - u.y) & (kH - 1);
    if (px >= W || py >= H) return orig;
    const HdTexture& hd = *u.hd;
    const uint32_t hx = uint32_t(std::min<uint64_t>(hd.width - 1, (uint64_t(2 * px + 1) * hd.width) / (2 * W)));
    const uint32_t hy = uint32_t(std::min<uint64_t>(hd.height - 1, (uint64_t(2 * py + 1) * hd.height) / (2 * H)));
    const uint8_t* p = &hd.rgba[(size_t(hy) * hd.width + hx) * 4];
    if (p[3] < 128) return 0;  // transparent texel
    uint16_t t = uint16_t((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10)) | (orig & 0x8000);
    return t ? t : uint16_t(0x0421);  // opaque black must not collapse into the transparent 0000h
}

} // namespace personarm
