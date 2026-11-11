#pragma once

// Small non-cryptographic hashes shared by the texture override system and PNG codec.
// None of these are collision-resistant against an adversary; they identify assets.

#include <array>
#include <cstddef>
#include <cstdint>

namespace personarm {

/// FNV-1a, 32-bit.
inline uint32_t Fnv1a32(const uint8_t* data, size_t size, uint32_t seed = 0x811C9DC5u) {
    uint32_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

namespace detail {
inline const std::array<uint32_t, 256>& Crc32Table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}
} // namespace detail

/// CRC-32 (IEEE 802.3, same as zlib/PNG). Pass the previous result as `crc` to continue a stream.
inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    const auto& table = detail::Crc32Table();
    uint32_t c = ~crc;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/// Adler-32 (zlib trailer).
inline uint32_t Adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

} // namespace personarm
