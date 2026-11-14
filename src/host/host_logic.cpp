#include "host/host_logic.h"

#include <cstdlib>
#include <cstring>

namespace personarm::host {

namespace {
// psx-spx "Controllers" digital pad bits.
constexpr uint16_t kSelect = 1u << 0, kStart = 1u << 3, kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6,
                   kLeft = 1u << 7, kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
                   kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15;
} // namespace

Rect Letterbox4x3(int win_w, int win_h) {
    if (win_w <= 0 || win_h <= 0) return {};
    Rect r;
    if (win_w * 3 >= win_h * 4) { r.h = win_h; r.w = win_h * 4 / 3; }  // wider than 4:3: pillarbox
    else { r.w = win_w; r.h = win_w * 3 / 4; }                          // taller: letterbox
    r.x = (win_w - r.w) / 2;
    r.y = (win_h - r.h) / 2;
    return r;
}

void DisplayToRgba(const std::vector<uint16_t>& vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   bool is_24bit, std::vector<uint32_t>& out) {
    out.assign(size_t(w) * h, 0xFF000000u);
    if (vram.size() < 1024u * 512u) return;
    for (uint32_t row = 0; row < h; ++row) {
        const uint16_t* line = vram.data() + size_t((y + row) & 511) * 1024;
        uint32_t* dst = out.data() + size_t(row) * w;
        if (!is_24bit) {
            for (uint32_t col = 0; col < w; ++col) {
                const uint16_t p = line[(x + col) & 1023];
                const uint32_t r = (p & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = ((p >> 10) & 0x1F) << 3;
                dst[col] = 0xFF000000u | b << 16 | g << 8 | r;
            }
        } else {
            auto byte_at = [&](uint32_t i) {
                const uint16_t p = line[(x + i / 2) & 1023];
                return uint32_t(i & 1 ? p >> 8 : p & 0xFF);
            };
            for (uint32_t col = 0; col < w; ++col)
                dst[col] = 0xFF000000u | byte_at(3 * col) | byte_at(3 * col + 1) << 8 | byte_at(3 * col + 2) << 16;
        }
    }
}

const std::vector<NamedBit>& KeyboardMap() {
    static const std::vector<NamedBit> map = {
        {"Up", kUp}, {"Down", kDown}, {"Left", kLeft}, {"Right", kRight},
        {"X", kCross}, {"Z", kCircle}, {"S", kSquare}, {"A", kTriangle},
        {"Return", kStart}, {"Backspace", kSelect}, {"Right Shift", kSelect},
        {"Q", kL1}, {"W", kR1}, {"1", kL2}, {"2", kR2},
    };
    return map;
}

const std::vector<NamedBit>& GamepadMap() {
    // SDL3 names face buttons by position: a/b/x/y = south/east/west/north = Cross/Circle/Square/Triangle.
    static const std::vector<NamedBit> map = {
        {"dpup", kUp}, {"dpdown", kDown}, {"dpleft", kLeft}, {"dpright", kRight},
        {"a", kCross}, {"b", kCircle}, {"x", kSquare}, {"y", kTriangle},
        {"start", kStart}, {"back", kSelect},
        {"leftshoulder", kL1}, {"rightshoulder", kR1}, {"lefttrigger", kL2}, {"righttrigger", kR2},
    };
    return map;
}

uint16_t PadBitForName(const std::vector<NamedBit>& map, const std::string& name) {
    for (const NamedBit& m : map)
        if (name == m.name) return m.bit;
    return 0;
}

bool ParseHostArgs(int argc, const char* const* argv, HostOptions& o) {
    o = HostOptions{};
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        auto is = [&](const char* name) { return !std::strcmp(a, name); };
        if (is("--stats")) o.stats = true;
        else if (is("--unthrottled")) o.unthrottled = true;
        else if (is("--hidden")) o.hidden = true;
        else if (is("--recomp")) o.recomp = true;
        else if (a[0] != '-') {
            if (!o.disc.empty()) return false;
            o.disc = a;
        } else {
            if (!v) return false;  // every other option takes a value
            ++i;
            if (is("--frames")) o.max_frames = std::strtoull(v, nullptr, 10);
            else if (is("--screenshot")) o.screenshot = v;
            else if (is("--input-script")) o.input_script = v;
            else if (is("--interp")) o.interp_hz = std::atoi(v);
            else if (is("--dump-dir")) o.dump_dir = v;
            else if (is("--texture-dump")) o.texture_dump = v;
            else if (is("--mods")) o.mods = v;
            else if (is("--dump-from")) o.dump_from = std::strtoull(v, nullptr, 10);
            else if (is("--dump-to")) o.dump_to = std::strtoull(v, nullptr, 10);
            else if (is("--memcard1")) o.memcard[0] = v;
            else if (is("--memcard2")) o.memcard[1] = v;
            else if (is("--recomp-quantum")) o.recomp_quantum = uint32_t(std::strtoul(v, nullptr, 10));
            else return false;
        }
    }
    return !o.disc.empty() && o.interp_hz >= 0 && o.interp_hz % 60 == 0 && o.recomp_quantum > 0;
}

} // namespace personarm::host
