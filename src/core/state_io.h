#pragma once
// Save-state stream: one Serialize(StateIO&) per device works both ways (save appends to a buffer,
// load reads it back into the same fields), so the two directions cannot drift apart.
// Little-endian host assumed (x86/ARM): the bytes are the in-memory layout.

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace personarm {

class StateIO {
public:
    explicit StateIO(std::vector<uint8_t>& out) : m_out(&out) {}             // save
    StateIO(const uint8_t* data, size_t size) : m_in(data), m_end(data + size), m_loading(true) {}  // load
    bool Loading() const { return m_loading; }
    bool Ok() const { return m_ok; }
    void Fail() { m_ok = false; }
    size_t Remaining() const { return m_loading ? size_t(m_end - m_in) : 0; }

    void Raw(void* p, size_t n) {
        if (!m_loading) { const auto* b = static_cast<const uint8_t*>(p); m_out->insert(m_out->end(), b, b + n); return; }
        if (!m_ok || n > Remaining()) { m_ok = false; return; }
        std::memcpy(p, m_in, n);
        m_in += n;
    }
    template <class T> void Pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "StateIO::Pod needs a trivially copyable type");
        Raw(&v, sizeof v);
    }
    template <class... T> void operator()(T&... v) { (Pod(v), ...); }

    /// Any resizable sequence of trivially copyable elements (vector, deque, string).
    template <class C> void Seq(C& c) {
        uint64_t n = c.size();
        Pod(n);
        if (m_loading) {
            if (!m_ok || n > Remaining()) { m_ok = false; return; }  // every element is >= 1 byte
            c.resize(size_t(n));
        }
        if constexpr (std::is_same_v<C, std::vector<typename C::value_type>> ||
                      std::is_same_v<C, std::string>) {
            if (n) Raw(c.data(), size_t(n) * sizeof(typename C::value_type));
        } else {
            for (auto& e : c) Pod(e);
        }
    }

    /// Section marker: a load fails here if the stream is out of step with the code.
    void Tag(const char (&name)[5]) {
        uint32_t t = uint32_t(uint8_t(name[0])) | uint32_t(uint8_t(name[1])) << 8 |
                     uint32_t(uint8_t(name[2])) << 16 | uint32_t(uint8_t(name[3])) << 24;
        const uint32_t want = t;
        Pod(t);
        if (t != want) m_ok = false;
    }

private:
    std::vector<uint8_t>* m_out = nullptr;
    const uint8_t* m_in = nullptr;
    const uint8_t* m_end = nullptr;
    bool m_loading = false;  // an empty buffer still loads (and fails), never writes
    bool m_ok = true;
};

} // namespace personarm
