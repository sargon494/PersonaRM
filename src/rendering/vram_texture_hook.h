#pragma once

// Texture dump / replacement keyed on the game's own GP0 A0h (CPU->VRAM) uploads, opt-in via
// Ps1Gpu::SetTextureHook (null = off, zero effect). Design: docs/research/MODDING.md sections 4.1, 4.6, 5.
//
//  * Every completed upload gets an id = Fnv1a32(x, y, w, h, pixel words) and tags the VRAM words it wrote.
//    Later GPU writes (fills, draws, VRAM->VRAM copies) clear the tags, so a tag means "this word still
//    holds that upload's data".
//  * When a textured primitive samples a tagged word, the upload counts as "used as a texture":
//    dump mode writes <HASH8>_<4|8|15>bit_<W>x<H>.png once (decoded with the texpage depth and CLUT in
//    effect at that first use), replace mode swaps the texel for the HD PNG of the same hash.
//
// Limits (first version): the hash ignores the CLUT, so every palette variant of an upload shares one
// PNG (dumped with the first CLUT seen); the replacement is full colour and ignores the CLUT. Sampling is
// nearest at native output resolution (more texture detail per texel is not visible yet; higher output
// resolution is a separate step). Textures split over several uploads get one PNG per upload.

#include "rendering/texture_override.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace personarm {

class VramTextureHook {
public:
    VramTextureHook();

    /// Dump mode: write each used upload once as PNG into `dir` (created on demand).
    void SetDumpDir(const std::string& dir) { m_dump_dir = dir; }
    /// Replace mode: index <dir>/<mod>/textures/*.png (+ optional <mod>/manifest.json: "priority" higher wins,
    /// ties by folder name; "serial" list restricts the mod to those discs when SetDiscSerial was given;
    /// "textures" sub-folder name). Returns the number of indexed textures over all active mods.
    size_t LoadMods(const std::string& dir);
    void SetDiscSerial(const std::string& serial) { m_serial = serial; }

    // --- called by Ps1Gpu ---
    void OnUpload(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void Invalidate(uint32_t x, uint32_t y, uint32_t w, uint32_t h);   // rect overwritten by a non-upload write
    void ClearTags();
    /// `orig` is the texel the rasterizer would use. (wx,wy) = VRAM word holding the texel, mode = texpage
    /// depth (0=4bit, 1=8bit, else 15bit), sub = pixel index inside that word, clut_x/y = CLUT origin.
    uint16_t Texel(const std::vector<uint16_t>& vram, uint32_t wx, uint32_t wy, uint32_t mode, uint32_t sub,
                   uint32_t clut_x, uint32_t clut_y, uint16_t orig);

    static uint32_t HashUpload(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    size_t UploadCount() const { return m_uploads.size(); }
    size_t DumpedCount() const { return m_dumped; }
    size_t ReplacedUploadCount() const { return m_replaced; }

private:
    struct Upload {
        uint32_t x, y, w, h, hash;
        bool dumped = false, resolved = false;
        std::shared_ptr<const HdTexture> hd;
    };
    struct Mod {
        std::string name;
        int priority = 0;
        TextureOverrideManager textures;
    };

    void Dump(const std::vector<uint16_t>& vram, const Upload& u, uint32_t mode, uint32_t clut_x, uint32_t clut_y);
    void Resolve(Upload& u);

    std::string m_dump_dir, m_serial;
    std::vector<std::unique_ptr<Mod>> m_mods;
    std::vector<uint32_t> m_tags;        // per VRAM word: upload index + 1, 0 = none
    std::vector<Upload> m_uploads;       // deduplicated by hash
    size_t m_dumped = 0, m_replaced = 0;
};

} // namespace personarm
