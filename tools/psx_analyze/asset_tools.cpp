#include "asset_tools.h"

#include "loader/disc_reader.h"
#include "recomp/mips_decode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

namespace personarm::tools {

static uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }  // little-endian host

std::vector<FileposEntry> ParseFilepos(const uint8_t* data, size_t size) {
    std::vector<FileposEntry> v;
    for (size_t o = 0; o + 8 <= size; o += 8) v.push_back({Rd32(data + o), Rd32(data + o + 4)});
    return v;
}

bool WalkChunks(const uint8_t* data, size_t size, std::vector<Chunk>& out, std::string& error) {
    size_t off = 0;
    while (off < size) {
        if (std::all_of(data + off, data + size, [](uint8_t b) { return b == 0; })) return true;
        if (size - off < 8) { error = "cabecera de chunk truncada en offset " + std::to_string(off); return false; }
        const uint32_t type = Rd32(data + off), csize = Rd32(data + off + 4);
        if (type == 0 && csize == 0) {   // padding up to the next sector boundary
            if (off % 0x800 == 0) { error = "chunk vacio en offset " + std::to_string(off); return false; }
            off = (off + 0x7FF) & ~size_t(0x7FF);
            continue;
        }
        if (csize < 8 || csize > size - off) { error = "tamano de chunk invalido en offset " + std::to_string(off); return false; }
        out.push_back({type, csize, off});
        off += (static_cast<size_t>(csize) + 3) & ~size_t(3);
    }
    return true;
}

bool RleDecode(const uint8_t* in, size_t size, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < size) {
        const uint8_t c = in[i++];
        if (c < 0x80) {
            const size_t n = size_t(c) + 1;
            if (size - i < n) return false;
            out.insert(out.end(), in + i, in + i + n);
            i += n;
        } else {
            if (i >= size) return false;
            out.insert(out.end(), size_t(c & 0x7F) + 3, in[i++]);
        }
    }
    return true;
}

// Does `w` read register `r`? Conservative per-opcode table for MIPS-I.
static bool ReadsReg(uint32_t w, unsigned r) {
    const unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    switch (op) {
    case 0: {
        const unsigned f = w & 63;
        if (f == 0 || f == 2 || f == 3) return rt == r;                       // sll/srl/sra
        if (f == 8 || f == 9 || f == 0x11 || f == 0x13) return rs == r;       // jr/jalr/mthi/mtlo
        if (f == 0x0C || f == 0x0D || f == 0x10 || f == 0x12) return false;  // syscall/break/mfhi/mflo
        return rs == r || rt == r;
    }
    case 1: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13:
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x32: case 0x3A: return rs == r;
    case 4: case 5: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: case 0x22: case 0x26:
        return rs == r || rt == r;
    case 0x10: case 0x12: return (rs == 4 || rs == 6) && rt == r;             // mtc/ctc
    default: return false;                                                    // j/jal/lui/other cop
    }
}

static bool IsHazardLoad(uint32_t w) {
    const unsigned op = w >> 26;
    return (op == 0x20 || op == 0x21 || op == 0x23 || op == 0x24 || op == 0x25) && ((w >> 16) & 31) != 0;
}

std::vector<Hazard> ScanHazards(const uint8_t* data, size_t size, uint32_t base, bool use_entry, uint32_t entry) {
    const size_t n = size / 4;
    std::vector<bool> code(n, !use_entry);
    if (use_entry) {
        std::vector<uint32_t> work{entry};
        auto idx = [&](uint32_t a, size_t& i) { i = (a - base) / 4; return a >= base && (a & 3) == 0 && i < n; };
        while (!work.empty()) {
            uint32_t a = work.back(); work.pop_back();
            size_t i;
            while (idx(a, i) && !code[i]) {
                code[i] = true;
                const recomp::Insn in = recomp::Decode(Rd32(data + i * 4), a);
                using K = recomp::InsnKind;
                if (in.kind == K::Invalid) break;
                if (in.has_delay_slot) {
                    size_t d;
                    if (idx(a + 4, d)) code[d] = true;
                    if (in.has_target) work.push_back(in.target);
                    if (in.kind == K::Jump || in.kind == K::Return || in.kind == K::IndirectJump) break;
                    a += 8;
                } else {
                    if (in.kind == K::Syscall) break;
                    a += 4;
                }
            }
        }
    }
    std::vector<Hazard> out;
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint32_t w = Rd32(data + i * 4), nx = Rd32(data + i * 4 + 4);
        if (code[i] && code[i + 1] && IsHazardLoad(w) && ReadsReg(nx, (w >> 16) & 31))
            out.push_back({base + uint32_t(i * 4), w, nx});
    }
    return out;
}

// ── Disc-backed commands ───────────────────────────────────────────────

static std::string Fname(uint32_t i) { char b[16]; std::snprintf(b, sizeof b, "F%04u", i); return b; }
static std::string Upper(std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }
static bool IsExt(const std::string& n) { const std::string u = Upper(n); return u.size() >= 4 && u.compare(u.size() - 4, 4, ".EXT") == 0; }
static std::string BaseOf(const std::string& n) { const auto p = n.find_last_of("/\\"); return p == std::string::npos ? n : n.substr(p + 1); }

static bool LoadFilepos(DiscReader& d, std::vector<FileposEntry>& fp, std::vector<DiscFileEntry>& files) {
    files = d.ListFiles("/");
    std::vector<uint8_t> raw;
    if (!d.ReadFile("FILEPOS.DAT", raw)) { std::cerr << "FILEPOS.DAT no encontrado en la imagen\n"; return false; }
    fp = ParseFilepos(raw.data(), raw.size());
    return true;
}

// ISO entry named Fnnnn[.ext][;1], or failing that the one at the same LBA.
static const DiscFileEntry* FindIso(const std::vector<DiscFileEntry>& files, uint32_t idx, uint32_t lba) {
    const std::string stem = Fname(idx);
    for (const auto& f : files) {
        const std::string b = Upper(BaseOf(f.name));
        if (!f.is_directory && b.compare(0, stem.size(), stem) == 0 &&
            (b.size() == stem.size() || b[stem.size()] == '.' || b[stem.size()] == ';')) return &f;
    }
    for (const auto& f : files) if (!f.is_directory && f.sector_lba == lba) return &f;
    return nullptr;
}

int RunFilepos(const std::string& disc) {
    DiscReader d;
    if (!d.Open(disc)) { std::cerr << "No se pudo abrir " << disc << "\n"; return 2; }
    std::vector<FileposEntry> fp; std::vector<DiscFileEntry> files;
    if (!LoadFilepos(d, fp, files)) return 2;
    unsigned ok = 0, bad = 0, empty = 0;
    std::printf("idx  lba      size      iso_name   estado\n");
    for (uint32_t i = 0; i < fp.size(); ++i) {
        const auto& e = fp[i];
        if (e.lba == 0 && e.size == 0) { ++empty; continue; }
        const DiscFileEntry* iso = FindIso(files, i, e.lba);
        std::string state = "OK";
        if (!iso) state = "SIN ENTRADA ISO";
        else if (iso->sector_lba != e.lba) state = "LBA DISTINTO (iso " + std::to_string(iso->sector_lba) + ")";
        else if (iso->size_bytes != e.size) {
            // .EXT files: FILEPOS holds sectors*2336 while the ISO records its own length.
            const uint32_t sectors = (iso->size_bytes + 2047) / 2048;
            if (!(IsExt(iso->name) && e.size == sectors * 2336u)) state = "SIZE DISTINTO (iso " + std::to_string(iso->size_bytes) + ")";
        }
        ++(state == "OK" ? ok : bad);
        std::printf("%-4u %-8u %-9u %-10s %s\n", i, e.lba, e.size, iso ? BaseOf(iso->name).c_str() : "-", state.c_str());
    }
    std::printf("entradas=%zu ok=%u mismatch=%u vacias=%u\n", fp.size(), ok, bad, empty);
    return bad ? 1 : 0;
}

int RunExtractFile(const std::string& disc, uint32_t index, const std::string& out_path) {
    DiscReader d;
    if (!d.Open(disc)) { std::cerr << "No se pudo abrir " << disc << "\n"; return 2; }
    std::vector<FileposEntry> fp; std::vector<DiscFileEntry> files;
    if (!LoadFilepos(d, fp, files)) return 2;
    if (index >= fp.size()) { std::cerr << "indice fuera de rango (" << fp.size() << " entradas)\n"; return 2; }
    const auto& e = fp[index];
    const DiscFileEntry* iso = FindIso(files, index, e.lba);
    if (!iso || iso->sector_lba != e.lba) { std::cerr << "el indice no tiene entrada ISO coincidente\n"; return 2; }
    std::vector<uint8_t> data;
    if (IsExt(iso->name)) {
        // Raw 2336-byte payloads after the 16-byte sync/header; only possible on 2352-byte images.
        if (d.GetSectorSize() != 2352) { std::cerr << ".EXT requiere imagen de 2352 bytes/sector\n"; return 2; }
        std::ifstream f(d.GetFilePath(), std::ios::binary);
        for (uint32_t s = 0; s < e.size / 2336; ++s) {
            uint8_t b[2336];
            f.seekg(static_cast<std::streamoff>(e.lba + s) * 2352 + 16);
            if (!f.read(reinterpret_cast<char*>(b), sizeof b)) { std::cerr << "lectura truncada\n"; return 2; }
            data.insert(data.end(), b, b + sizeof b);
        }
    } else if (!d.ReadFile(iso->name, data)) { std::cerr << "no se pudo leer " << iso->name << "\n"; return 2; }
    std::ofstream o(out_path, std::ios::binary | std::ios::trunc);
    if (!o) { std::cerr << "No se pudo escribir " << out_path << "\n"; return 3; }
    o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    std::cerr << "escritos " << data.size() << " bytes (" << iso->name << ")\n";
    return 0;
}

} // namespace personarm::tools
