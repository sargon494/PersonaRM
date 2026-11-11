#include "texture_override.h"

#include "core/hash.h"
#include "core/json_lite.h"
#include "core/logger.h"
#include "loader/tim_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace personarm {

namespace fs = std::filesystem;

namespace {

uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::string ToLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// "1A2B3C4D.png" or "1a2b3c4d_hero.png" -> hash
std::optional<uint32_t> HashFromFileName(const fs::path& p) {
    if (ToLower(p.extension().string()) != ".png") return std::nullopt;
    const std::string stem = p.stem().string();
    if (stem.size() < 8) return std::nullopt;
    for (size_t i = 0; i < 8; ++i) if (!IsHex(stem[i])) return std::nullopt;
    if (stem.size() > 8 && stem[8] != '_') return std::nullopt;
    return static_cast<uint32_t>(std::stoul(stem.substr(0, 8), nullptr, 16));
}

// The manifest may only reference files inside the pack directory.
bool IsSafeRelativePath(const std::string& rel) {
    if (rel.empty()) return false;
    fs::path p(rel);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p) if (part == "..") return false;
    return true;
}

} // namespace

std::optional<TimLayout> ParseTimLayout(const uint8_t* data, size_t size) {
    if (!data || size < 8 || Rd32(data) != 0x10) return std::nullopt;
    const uint32_t flags = Rd32(data + 4);
    TimLayout l;
    l.bpp_mode = flags & 0x07;
    if (l.bpp_mode > 3) return std::nullopt;
    l.has_clut = (flags & 0x08) != 0;

    size_t offset = 8;
    if (l.has_clut) {
        if (size - offset < 12) return std::nullopt;
        const uint32_t bnum = Rd32(data + offset);
        if (bnum < 12 || bnum > size - offset) return std::nullopt;
        l.clut_offset = offset;
        l.clut_size = bnum;
        offset += bnum;
    }
    if (size - offset < 12) return std::nullopt;
    const uint32_t bnum = Rd32(data + offset);
    if (bnum < 12 || bnum > size - offset) return std::nullopt;
    l.pixel_block_offset = offset;
    l.pixel_block_size = bnum;
    l.pixel_data_offset = offset + 12;
    l.width_words = Rd16(data + offset + 8);
    l.height = Rd16(data + offset + 10);
    l.pixel_data_size = bnum - 12;
    if (static_cast<uint64_t>(l.width_words) * l.height * 2 > l.pixel_data_size) return std::nullopt;
    switch (l.bpp_mode) {
        case 0: l.width_pixels = l.width_words * 4; break;
        case 1: l.width_pixels = l.width_words * 2; break;
        case 2: l.width_pixels = l.width_words; break;
        default: l.width_pixels = (l.width_words * 2) / 3; break;
    }
    l.end_offset = offset + bnum;
    return l;
}

void TextureOverrideManager::SetHashAlgorithm(TextureHashAlgo algo) {
    if (algo == m_algo) return;
    m_algo = algo;
    InvalidateIndex();
}

void TextureOverrideManager::SetHashScope(TextureHashScope scope) {
    if (scope == m_scope) return;
    m_scope = scope;
    InvalidateIndex();
}

void TextureOverrideManager::InvalidateIndex() {
    m_index.clear();
    m_failed.clear();
    ClearCache();
}

void TextureOverrideManager::SetCacheBudget(size_t bytes) {
    m_cache_budget = bytes;
    EvictIfNeeded(std::nullopt);
}

void TextureOverrideManager::ClearCache() {
    m_cache.clear();
    m_lru.clear();
    m_stats.cached_bytes = 0;
}

std::optional<uint32_t> TextureOverrideManager::ComputeHash(const uint8_t* tim, size_t size) const {
    auto layout = ParseTimLayout(tim, size);
    if (!layout) return std::nullopt;

    auto hash_bytes = [&](const uint8_t* p, size_t n, uint32_t seed) {
        return m_algo == TextureHashAlgo::Fnv1a32 ? Fnv1a32(p, n, seed) : Crc32(p, n, seed);
    };
    const uint32_t seed = m_algo == TextureHashAlgo::Fnv1a32 ? 0x811C9DC5u : 0u;

    if (m_scope == TextureHashScope::Full) {
        return hash_bytes(tim, layout->end_offset, seed);
    }
    // Pixel data + the properties that make two pixel blocks different textures.
    uint8_t meta[6] = {static_cast<uint8_t>(layout->bpp_mode), 0,
                       static_cast<uint8_t>(layout->width_words & 0xFF), static_cast<uint8_t>(layout->width_words >> 8),
                       static_cast<uint8_t>(layout->height & 0xFF), static_cast<uint8_t>(layout->height >> 8)};
    uint32_t h = hash_bytes(meta, sizeof(meta), seed);
    return hash_bytes(tim + layout->pixel_data_offset, layout->pixel_data_size, h);
}

std::string TextureOverrideManager::HashToHex(uint32_t hash) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08X", hash);
    return buf;
}

std::optional<uint32_t> TextureOverrideManager::ParseHashString(const std::string& s) {
    std::string t = s;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.empty() || t.size() > 8) return std::nullopt;
    for (char c : t) if (!IsHex(c)) return std::nullopt;
    return static_cast<uint32_t>(std::stoul(t, nullptr, 16));
}

size_t TextureOverrideManager::LoadDirectory(const std::string& dir) {
    m_index.clear();
    m_failed.clear();
    ClearCache();
    m_directory = dir;

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        PRM_LOG_WARN("TextureOverride", "Directory not found: " + dir);
        return 0;
    }

    // Optional manifest first: it may set the hash settings the pack was authored with.
    const fs::path manifest_path = fs::path(dir) / "overrides.json";
    std::vector<std::pair<uint32_t, std::string>> manifest_entries;
    if (fs::is_regular_file(manifest_path, ec)) {
        std::ifstream f(manifest_path);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string err;
        auto json = JsonValue::Parse(ss.str(), &err);
        if (!json || !json->IsObject()) {
            PRM_LOG_WARN("TextureOverride", "Ignoring invalid overrides.json: " + err);
        } else {
            std::string algo = ToLower(json->StringOr("hash_algorithm", ""));
            std::string scope = ToLower(json->StringOr("hash_scope", ""));
            if (algo == "crc32") m_algo = TextureHashAlgo::Crc32;
            else if (algo == "fnv1a") m_algo = TextureHashAlgo::Fnv1a32;
            if (scope == "pixels") m_scope = TextureHashScope::PixelsOnly;
            else if (scope == "full") m_scope = TextureHashScope::Full;
            if (const JsonValue* list = json->Find("textures"); list && list->IsArray()) {
                for (const auto& e : list->Items()) {
                    auto h = e.IsObject() ? ParseHashString(e.StringOr("hash", "")) : std::nullopt;
                    std::string file = e.IsObject() ? e.StringOr("file", "") : "";
                    if (!h || !IsSafeRelativePath(file)) {
                        PRM_LOG_WARN("TextureOverride", "Skipping invalid manifest entry");
                        continue;
                    }
                    manifest_entries.emplace_back(*h, file);
                }
            }
        }
    }

    // Collect PNGs in a deterministic order so duplicate hashes resolve the same way everywhere.
    std::vector<fs::path> pngs;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && HashFromFileName(it->path())) pngs.push_back(it->path());
    }
    std::sort(pngs.begin(), pngs.end());
    for (const auto& p : pngs) {
        uint32_t h = *HashFromFileName(p);
        if (!m_index.emplace(h, p.string()).second) {
            PRM_LOG_WARN("TextureOverride", "Duplicate override for hash " + HashToHex(h) + ": " + p.string() + " ignored");
        }
    }
    // Manifest entries win over name-derived ones.
    for (const auto& [h, rel] : manifest_entries) {
        fs::path full = fs::path(dir) / rel;
        if (!fs::is_regular_file(full, ec)) {
            PRM_LOG_WARN("TextureOverride", "Manifest file missing: " + rel);
            continue;
        }
        m_index[h] = full.string();
    }

    PRM_LOG_INFO("TextureOverride", "Indexed " + std::to_string(m_index.size()) + " HD textures from " + dir);
    return m_index.size();
}

void TextureOverrideManager::EvictIfNeeded(std::optional<uint32_t> keep_hash) {
    while (m_stats.cached_bytes > m_cache_budget && !m_lru.empty()) {
        uint32_t victim = m_lru.back();
        if (keep_hash && victim == *keep_hash) {
            if (m_lru.size() == 1) break; // never evict the entry just requested
            // move it to the front and retry with the next-oldest
            m_lru.pop_back();
            m_lru.push_front(victim);
            m_cache[victim].lru_it = m_lru.begin();
            continue;
        }
        auto it = m_cache.find(victim);
        if (it != m_cache.end()) {
            m_stats.cached_bytes -= it->second.texture->rgba.size();
            m_cache.erase(it);
            ++m_stats.evictions;
        }
        m_lru.pop_back();
    }
}

std::shared_ptr<const HdTexture> TextureOverrideManager::FindOverride(uint32_t hash) {
    if (!m_enabled) return nullptr;

    auto cached = m_cache.find(hash);
    if (cached != m_cache.end()) {
        m_lru.splice(m_lru.begin(), m_lru, cached->second.lru_it);
        ++m_stats.hits;
        return cached->second.texture;
    }

    auto idx = m_index.find(hash);
    if (idx == m_index.end() || m_failed.count(hash)) {
        ++m_stats.misses;
        return nullptr;
    }

    std::string err;
    auto png = PngCodec::DecodeFile(idx->second, &err);
    if (!png) {
        PRM_LOG_WARN("TextureOverride", "Failed to load " + idx->second + ": " + err);
        m_failed.insert(hash); // do not retry (and re-log) every frame
        ++m_stats.load_failures;
        ++m_stats.misses;
        return nullptr;
    }

    auto tex = std::make_shared<HdTexture>();
    tex->source_hash = hash;
    tex->width = png->width;
    tex->height = png->height;
    tex->rgba = std::move(png->rgba);
    ++m_stats.loads;
    ++m_stats.hits;

    m_lru.push_front(hash);
    m_stats.cached_bytes += tex->rgba.size();
    m_cache[hash] = CacheEntry{tex, m_lru.begin()};
    EvictIfNeeded(hash);
    return tex;
}

TextureOverrideResult TextureOverrideManager::LookupForTim(const uint8_t* tim, size_t size) {
    TextureOverrideResult result;
    if (!m_enabled || m_index.empty()) return result;
    auto layout = ParseTimLayout(tim, size);
    if (!layout) return result;
    auto hash = ComputeHash(tim, size);
    if (!hash) return result;
    result.texture = FindOverride(*hash);
    if (result.texture && layout->width_pixels > 0 && layout->height > 0) {
        result.scale_x = static_cast<float>(result.texture->width) / static_cast<float>(layout->width_pixels);
        result.scale_y = static_cast<float>(result.texture->height) / static_cast<float>(layout->height);
    }
    return result;
}

bool TextureOverrideManager::ExportOriginal(const uint8_t* tim, size_t size, const std::string& dir,
                                            std::string* out_path) const {
    auto hash = ComputeHash(tim, size);
    if (!hash) return false;
    auto image = TimParser::ParseFromMemory(tim, size);
    if (!image || image->width == 0 || image->height == 0) return false;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;

    std::vector<uint8_t> rgba;
    rgba.reserve(image->pixels_rgba.size() * 4);
    for (const auto& c : image->pixels_rgba) {
        rgba.push_back(c.r); rgba.push_back(c.g); rgba.push_back(c.b); rgba.push_back(c.a);
    }
    const std::string path = (fs::path(dir) / (HashToHex(*hash) + ".png")).string();
    if (!PngCodec::EncodeToFile(path, image->width, image->height, rgba.data())) return false;
    if (out_path) *out_path = path;
    return true;
}

} // namespace personarm
