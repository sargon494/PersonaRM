#pragma once

// HD texture replacement: original PS1 textures (.TIM) are identified by a hash and, if an
// artist-provided PNG exists for that hash, the renderer receives the HD RGBA image instead.
//
// Pack layout (directory passed to LoadDirectory):
//   <dir>/1A2B3C4D.png            file name = 8 hex digits of the hash (optionally "_label" after them)
//   <dir>/overrides.json          optional manifest:
//       { "hash_algorithm": "fnv1a" | "crc32", "hash_scope": "full" | "pixels",
//         "textures": [ { "hash": "0x1A2B3C4D", "file": "sub/dir/hero.png" } ] }
//
// Hash scopes: "full" covers the whole TIM structure (palette + pixels, so palette swaps get
// different hashes); "pixels" covers pixel data and dimensions only (all palette variants of
// one texture share an override). Trailing sector padding after the TIM is ignored in both.
//
// Not thread-safe: call from the thread that owns the renderer / asset loading.

#include "loader/png_codec.h"

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace personarm {

enum class TextureHashAlgo { Fnv1a32, Crc32 };
enum class TextureHashScope { Full, PixelsOnly };

/// Byte layout of a TIM file (validated against the buffer length).
struct TimLayout {
    uint32_t bpp_mode = 0;          // 0=4bit 1=8bit 2=15bit 3=24bit
    bool has_clut = false;
    size_t clut_offset = 0;         // start of CLUT block (if any)
    size_t clut_size = 0;
    size_t pixel_block_offset = 0;
    size_t pixel_block_size = 0;
    size_t pixel_data_offset = 0;   // after the 12-byte block header
    size_t pixel_data_size = 0;
    uint32_t width_words = 0;       // 16-bit words per row in VRAM
    uint32_t width_pixels = 0;
    uint32_t height = 0;
    size_t end_offset = 0;          // first byte after the TIM (ignores trailing padding)
};

std::optional<TimLayout> ParseTimLayout(const uint8_t* data, size_t size);

struct HdTexture {
    uint32_t source_hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;      // width * height * 4
};

/// Result of looking up an original TIM: the shared HD image plus its scale relative to the original.
struct TextureOverrideResult {
    std::shared_ptr<const HdTexture> texture;
    float scale_x = 0.0f;           // HD width / original width in pixels
    float scale_y = 0.0f;
    explicit operator bool() const { return texture != nullptr; }
};

struct TextureOverrideStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t loads = 0;
    uint64_t load_failures = 0;
    uint64_t evictions = 0;
    size_t cached_bytes = 0;
};

class TextureOverrideManager {
public:
    static constexpr size_t kDefaultCacheBudget = 256ull * 1024 * 1024;

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    /// Changing the hash settings invalidates the index and the cache (hashes would not match).
    void SetHashAlgorithm(TextureHashAlgo algo);
    void SetHashScope(TextureHashScope scope);
    TextureHashAlgo GetHashAlgorithm() const { return m_algo; }
    TextureHashScope GetHashScope() const { return m_scope; }

    void SetCacheBudget(size_t bytes);

    /// Hash of a TIM using the current algorithm/scope. nullopt if the TIM is malformed.
    std::optional<uint32_t> ComputeHash(const uint8_t* tim, size_t size) const;

    /// Indexes every <hash>.png under `dir` (recursively, symlinks not followed) plus the optional
    /// overrides.json manifest. Replaces the previous index. Returns the number of indexed textures.
    size_t LoadDirectory(const std::string& dir);
    size_t IndexedCount() const { return m_index.size(); }
    bool HasOverride(uint32_t hash) const { return m_index.count(hash) != 0; }
    const std::string& GetDirectory() const { return m_directory; }

    /// Returns the HD texture for a hash (decoding and caching it on first use), or nullptr.
    std::shared_ptr<const HdTexture> FindOverride(uint32_t hash);

    /// Hash + lookup + scale computation for an original TIM. Empty result when disabled,
    /// malformed or when no override exists. The HD pixels are shared, never copied.
    TextureOverrideResult LookupForTim(const uint8_t* tim, size_t size);

    /// Writes <dir>/<hash>.png with the decoded original so artists can repaint it.
    bool ExportOriginal(const uint8_t* tim, size_t size, const std::string& dir, std::string* out_path = nullptr) const;

    void ClearCache();
    const TextureOverrideStats& Stats() const { return m_stats; }

    static std::string HashToHex(uint32_t hash);
    static std::optional<uint32_t> ParseHashString(const std::string& s);

private:
    struct CacheEntry {
        std::shared_ptr<const HdTexture> texture;
        std::list<uint32_t>::iterator lru_it;
    };

    void InvalidateIndex();
    void EvictIfNeeded(std::optional<uint32_t> keep_hash);

    bool m_enabled = true;
    TextureHashAlgo m_algo = TextureHashAlgo::Fnv1a32;
    TextureHashScope m_scope = TextureHashScope::Full;
    size_t m_cache_budget = kDefaultCacheBudget;

    std::string m_directory;
    std::unordered_map<uint32_t, std::string> m_index;   // hash -> PNG path
    std::unordered_map<uint32_t, CacheEntry> m_cache;
    std::list<uint32_t> m_lru;                           // front = most recently used
    std::unordered_set<uint32_t> m_failed;               // hashes whose PNG failed to decode
    TextureOverrideStats m_stats;
};

} // namespace personarm
