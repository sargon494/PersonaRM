// p2text: extract / insert dialogue of Persona 2 (JP) event banks, verify the codec on a real disc,
// and dump a font preview sheet. Output goes to user-chosen paths only; nothing is embedded.
#include "p2text.h"

#include "asset_tools.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace personarm::tools;
using namespace personarm::tools::p2text;
namespace fs = std::filesystem;

namespace {

using Args = std::map<std::string, std::string>;

void Usage() {
    std::puts(
        "p2text <comando> [opciones]\n"
        "  extract    (--disc <img> [--index N] | --file <F.bin>) --out <dir> [--map glifos.txt] [--us|--jp]\n"
        "             escribe <dir>/<Fnnnn>.tsv por fichero con bancos; imprime recuentos\n"
        "  insert     --file <F.bin> --tsv <Fnnnn.tsv> --out <parcheado.bin> [--map glifos.txt] [--recompress] [--us|--jp]\n"
        "             [--disc <img> | --exe <SLUS_011.58>] [--strict]\n"
        "             US (SLUS): texto empaquetado 0x1136 (2 bytes ASCII por u16), detectado por fichero; --us/--jp lo fuerzan\n"
        "             avisa de lineas mas anchas que la ventana y de paginas con demasiadas lineas (en US el ancho sale del EXE\n"
        "             de --disc/--exe); --strict convierte los avisos en error y no escribe nada\n"
        "  verify     (--disc <img> [--index N] | --file <F.bin> [--exe <SLUS_011.58>])   ida y vuelta LZ/banco/fichero y\n"
        "             maquetacion del texto original (solo lectura)\n"
        "  font-sheet (--disc <img> | --font <F0041.bin>) --out <hoja.bmp> [--first N] [--count N]\n"
        "El mapa de glifos: lineas 'id<TAB>caracter' (id decimal o 0xHEX); semilla ASCII 0x20-0x7E.");
}

bool ReadAll(const std::string& p, std::vector<uint8_t>& v) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    v.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
bool WriteAll(const std::string& p, const void* d, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(d), std::streamsize(n));
    return bool(f);
}
std::string Hex(size_t v) { char b[24]; std::snprintf(b, sizeof b, "0x%X", unsigned(v)); return b; }
std::string Fname(unsigned i) { char b[16]; std::snprintf(b, sizeof b, "F%04u", i); return b; }

// Calls fn(name, bytes) for the file(s) selected by --file / --disc [--index].
template <class F> int ForEachFile(const Args& a, F fn) {
    auto get = [&](const char* k) { auto it = a.find(k); return it == a.end() ? std::string() : it->second; };
    if (!get("file").empty()) {
        std::vector<uint8_t> b;
        if (!ReadAll(get("file"), b)) { std::cerr << "no se puede leer " << get("file") << "\n"; return 2; }
        fn(fs::path(get("file")).stem().string(), b);
        return 0;
    }
    if (get("disc").empty()) { Usage(); return 2; }
    DiscFiles d;
    if (!d.Open(get("disc"))) { std::cerr << "no se pudo abrir el disco " << get("disc") << "\n"; return 2; }
    size_t lo = 0, hi = d.Count();
    if (!get("index").empty()) { lo = std::strtoul(get("index").c_str(), nullptr, 10); hi = lo + 1; }
    for (size_t i = lo; i < hi; ++i) {
        std::vector<uint8_t> b;
        if (d.Read(uint32_t(i), b) && b.size() <= (3u << 20)) fn(Fname(unsigned(i)), b);   // >3 MB: not event files (TEXT_SYSTEM TS-006)
    }
    return 0;
}

bool LoadMap(const Args& a, GlyphMap& m) {
    auto it = a.find("map");
    if (it == a.end()) return true;
    std::string e;
    if (!m.Load(it->second, e)) { std::cerr << e << "\n"; return false; }
    return true;
}

uint32_t NewChunkSize(const std::vector<uint8_t>& f, size_t pos) { uint32_t v; std::memcpy(&v, f.data() + pos + 4, 4); return v; }

// US widths come from the user's own boot EXE (--exe, or the boot executable of --disc); without them only lines/page are checked.
Metrics LoadMetrics(const Args& a, bool us, bool warn = true) {
    Metrics m = DefaultMetrics(us);
    if (!us) return m;
    std::vector<uint8_t> exe;
    if (a.count("exe")) ReadAll(a.at("exe"), exe);
    else if (a.count("disc")) { DiscFiles d; if (d.Open(a.at("disc"))) d.Reader().ReadFile(d.Reader().DetectBootExecutable(), exe); }
    std::string err = "falta --disc/--exe";
    if (!exe.empty() && UsMetricsFromExe(exe, m, err)) return m;
    if (warn) std::cerr << "aviso: ancho de linea sin comprobar (" << err << "); solo se comprueban las lineas por pagina\n";
    m.max_px = 0;
    return m;
}

struct Loaded { Bank bank; Messages msgs; };
bool Load(const std::vector<uint8_t>& file, const BankRef& r, Loaded& l) {
    std::vector<uint8_t> dec;
    if (!DecodeBankChunk(file, r, dec) || !ParseBank(dec, l.bank)) return false;
    l.msgs = SplitMessages(l.bank);
    return true;
}

int CmdExtract(const Args& a) {
    GlyphMap base;
    if (!LoadMap(a, base)) return 2;
    if (!a.count("out")) { Usage(); return 2; }
    fs::create_directories(a.at("out"));
    size_t files = 0, banks = 0, msgs = 0, orphans = 0, bad = 0, glyph_unmapped = 0;
    int rc = ForEachFile(a, [&](const std::string& name, const std::vector<uint8_t>& file) {
        const auto found = FindBanks(file);
        if (found.empty()) return;
        GlyphMap map = base;
        if (!a.count("jp")) for (const auto& r : found) { Loaded l; if (Load(file, r, l) && UsesPackedText(l.msgs)) map.packed = true; }
        if (a.count("us")) map.packed = true;
        std::ostringstream o;
        o << "# p2text v1 " << name << (map.packed ? " us-packed" : "") << " | bank, msg, offset, original codes (hex u16), text. Edit only the text column.\n";
        o << "# bank\tmsg\toffset\tcodes\ttext\n";
        for (const auto& r : found) {
            Loaded l;
            if (!Load(file, r, l)) continue;
            ++banks;
            orphans += l.msgs.orphans;
            bad += l.msgs.bad_refs;
            for (const auto& m : l.msgs.list) {
                ++msgs;
                o << Hex(r.pos) << '\t' << m.id << '\t' << Hex(m.offset) << '\t';
                for (size_t i = 0; i < m.codes.size(); ++i) {
                    char b[8];
                    std::snprintf(b, sizeof b, i ? " %04x" : "%04x", m.codes[i]);
                    o << b;
                    if (m.codes[i] < 0x1000 && !map.to_text.count(m.codes[i]) && !map.packed) ++glyph_unmapped;
                }
                o << '\t' << Render(m.codes, map) << '\n';
            }
        }
        ++files;
        const std::string s = o.str();
        WriteAll((fs::path(a.at("out")) / (name + ".tsv")).string(), s.data(), s.size());
    });
    std::printf("ficheros=%zu bancos=%zu mensajes=%zu huerfanos=%zu refs_malas=%zu glifos_sin_mapa=%zu\n",
                files, banks, msgs, orphans, bad, glyph_unmapped);
    return rc;
}

int CmdInsert(const Args& a) {
    for (const char* k : {"file", "tsv", "out"}) if (!a.count(k)) { Usage(); return 2; }
    GlyphMap map;
    if (!LoadMap(a, map)) return 2;
    std::vector<uint8_t> file;
    if (!ReadAll(a.at("file"), file)) { std::cerr << "no se puede leer " << a.at("file") << "\n"; return 2; }
    std::ifstream t(a.at("tsv"), std::ios::binary);
    if (!t) { std::cerr << "no se puede leer " << a.at("tsv") << "\n"; return 2; }

    const auto banks = FindBanks(file);
    std::map<size_t, Loaded> loaded;
    std::map<size_t, std::vector<std::vector<uint16_t>>> out_codes;
    for (const auto& r : banks) {
        Loaded l;
        if (!Load(file, r, l)) continue;
        auto& v = out_codes[r.pos];
        for (const auto& m : l.msgs.list) v.push_back(m.codes);
        if (UsesPackedText(l.msgs)) map.packed = true;
        loaded[r.pos] = std::move(l);
    }
    if (a.count("us")) map.packed = true;
    if (a.count("jp")) map.packed = false;
    const Metrics metrics = LoadMetrics(a, map.packed);
    const std::string fname = fs::path(a.at("file")).stem().string();
    std::vector<std::string> errors, layout;
    std::string line;
    for (int ln = 1; std::getline(t, line); ++ln) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (ln == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        size_t p = 0;
        for (int c = 0; c < 4; ++c) {
            const size_t q = line.find('\t', p);
            if (q == std::string::npos) break;
            col.push_back(line.substr(p, q - p));
            p = q + 1;
        }
        const std::string where = a.at("tsv") + ":" + std::to_string(ln) + ": ";
        if (col.size() != 4) { errors.push_back(where + "se esperaban 5 columnas separadas por TAB"); continue; }
        const size_t pos = std::strtoul(col[0].c_str(), nullptr, 0);
        const size_t id = std::strtoul(col[1].c_str(), nullptr, 10);
        auto it = out_codes.find(pos);
        if (it == out_codes.end() || id >= it->second.size()) { errors.push_back(where + "banco/mensaje inexistente en este fichero"); continue; }
        const std::string text = line.substr(p);
        if (text == Render(it->second[id], map)) continue;                       // untouched: keep original codes
        std::vector<std::string> e;
        std::vector<uint16_t> codes;
        if (Encode(text, map, codes, e)) {
            for (auto& w : CheckLayout(codes, metrics, fname + " msg " + std::to_string(id))) layout.push_back(std::move(w));
            it->second[id] = std::move(codes);
        } else for (const auto& s : e) errors.push_back(where + "msg " + std::to_string(id) + ": " + s);
    }
    if (!errors.empty()) {
        std::cerr << errors.size() << " errores, no se escribe nada:\n";
        for (const auto& s : errors) std::cerr << "  " << s << "\n";
        return 1;
    }
    for (const auto& w : layout) std::printf("aviso: %s\n", w.c_str());
    if (a.count("strict") && !layout.empty()) { std::cerr << layout.size() << " avisos de maquetacion con --strict, no se escribe nada\n"; return 1; }
    std::vector<uint8_t> cur = file;
    const bool force = a.count("recompress") > 0;
    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {                 // last chunk first: earlier offsets stay valid
        const BankRef* ref = nullptr;
        for (const auto& r : banks) if (r.pos == it->first) ref = &r;
        std::vector<uint8_t> dec, patched;
        RebuildReport rep;
        std::string err;
        if (!RebuildBank(it->second.bank, it->second.msgs, out_codes[it->first], dec, rep, err) ||
            !PatchFile(cur, *ref, dec, !force, patched, err)) { std::cerr << err << "\n"; return 1; }
        std::printf("banco %s: %u mensajes cambiados, mensajes %zu -> %zu B, descomprimido %zu -> %zu B, chunk %u -> %zu B, fichero %zu -> %zu B\n",
                    Hex(it->first).c_str(), rep.changed, rep.old_msg_bytes, rep.new_msg_bytes, rep.old_dec, rep.new_dec,
                    ref->size, size_t(NewChunkSize(patched, ref->pos)), cur.size(), patched.size());
        for (const auto& w : rep.warnings) std::printf("  aviso: %s\n", w.c_str());
        cur.swap(patched);
    }
    if (!WriteAll(a.at("out"), cur.data(), cur.size())) { std::cerr << "no se pudo escribir " << a.at("out") << "\n"; return 3; }
    std::printf("escrito %s (%zu B; original %zu B). Si crece, el FILEPOS servido debe llevar el tamano nuevo.\n",
                a.at("out").c_str(), cur.size(), file.size());
    return 0;
}

int CmdVerify(const Args& a) {
    size_t files = 0, banks = 0, msgs = 0, orphans = 0, bad = 0, fail = 0, comp_orig = 0, comp_new = 0, tried = 0, same_bytes = 0;
    // layout of the original text (a sanity check of the window limits); US widths need --disc/--exe
    const Metrics usm = (a.count("exe") || a.count("disc")) ? LoadMetrics(a, true, false) : Metrics{};
    const Metrics jpm = DefaultMetrics(false);
    unsigned max_px = 0, max_lines = 0;
    size_t over = 0;
    auto bad_case = [&](const std::string& n, const BankRef& r, const char* why) {
        ++fail;
        std::printf("FALLO %s %s: %s\n", n.c_str(), Hex(r.pos).c_str(), why);
    };
    int rc = ForEachFile(a, [&](const std::string& name, const std::vector<uint8_t>& file) {
        unsigned t = 0;
        const auto found = FindBanks(file, &t);
        tried += t;
        if (found.empty()) return;
        ++files;
        GlyphMap map;
        for (const auto& r : found) { Loaded l; if (Load(file, r, l) && UsesPackedText(l.msgs)) map.packed = true; }
        std::vector<std::vector<uint8_t>> want;
        for (const auto& r : found) {
            Loaded l;
            if (!Load(file, r, l)) { bad_case(name, r, "no carga"); continue; }
            ++banks;
            msgs += l.msgs.list.size(); orphans += l.msgs.orphans; bad += l.msgs.bad_refs;
            want.push_back(l.bank.data);
            const auto& d = l.bank.data;
            const auto c = LzCompress(d.data(), d.size());
            std::vector<uint8_t> back;
            if (!LzDecompress(c.data(), c.size(), d.size(), back) || back != d) bad_case(name, r, "LZ ida y vuelta");
            comp_orig += r.size - 12; comp_new += c.size();
            std::vector<std::vector<uint16_t>> codes, via_text;
            for (const auto& m : l.msgs.list) {
                codes.push_back(m.codes);
                const Metrics& mm = map.packed ? usm : jpm;
                std::map<unsigned, unsigned> per_page;
                for (const auto& ln : MeasureLines(m.codes, mm)) { max_px = std::max(max_px, ln.px); max_lines = std::max(max_lines, ++per_page[ln.page]); }
                over += CheckLayout(m.codes, mm, "").size();
                std::vector<uint16_t> e; std::vector<std::string> er;
                if (!Encode(Render(m.codes, map), map, e, er) || e != m.codes) bad_case(name, r, "texto ida y vuelta");
            }
            std::vector<uint8_t> dec; RebuildReport rep; std::string err;
            if (!RebuildBank(l.bank, l.msgs, codes, dec, rep, err) || dec != d) bad_case(name, r, "reconstruccion sin cambios");
            // growth: one extra space glyph in every message -> every op-19 operand must follow
            auto grown = codes;
            for (auto& m : grown) m.insert(m.end() - 1, uint16_t(0x20));
            Bank gb; std::vector<uint8_t> gdec;
            if (!RebuildBank(l.bank, l.msgs, grown, gdec, rep, err) || !ParseBank(gdec, gb)) { bad_case(name, r, "crecimiento"); continue; }
            const Messages gm = SplitMessages(gb);
            bool ok = gm.list.size() == l.msgs.list.size() && gm.bad_refs == l.msgs.bad_refs && gm.orphans == l.msgs.orphans;
            for (size_t i = 0; ok && i < gm.list.size(); ++i) ok = gm.list[i].codes == grown[i] && gm.list[i].refs == l.msgs.list[i].refs;
            if (!ok) bad_case(name, r, "operandos op 19 tras crecer");
        }
        // whole-file patch with forced recompression of every bank, last first
        std::vector<uint8_t> cur = file;
        bool unchanged_ok = true;
        for (auto it = found.rbegin(); it != found.rend(); ++it) {
            std::vector<uint8_t> dec, out;
            std::string err;
            if (!DecodeBankChunk(cur, *it, dec) || !PatchFile(cur, *it, dec, false, out, err)) { unchanged_ok = false; break; }
            cur.swap(out);
        }
        if (cur == file) ++same_bytes;
        const auto again = FindBanks(cur);
        bool ok = unchanged_ok && again.size() == found.size();
        for (size_t i = 0; ok && i < again.size(); ++i) { std::vector<uint8_t> d; ok = DecodeBankChunk(cur, again[i], d) && d == want[i]; }
        if (!ok) bad_case(name, found[0], "fichero recomprimido");
    });
    std::printf("ficheros=%zu bancos=%zu (chunks LZ probados=%zu) mensajes=%zu huerfanos=%zu refs_malas=%zu\n", files, banks, tried, msgs, orphans, bad);
    std::printf("LZ de los bancos: original %zu B, recomprimido %zu B; ficheros identicos tras recomprimir: %zu/%zu; fallos=%zu\n",
                comp_orig, comp_new, same_bytes, files, fail);
    std::printf("maquetacion del texto original: linea mas ancha %u px (limite JP %u / US %u), pagina con mas lineas %u (limite %u), fuera de limite=%zu\n",
                max_px, jpm.max_px, DefaultMetrics(true).max_px, max_lines, jpm.max_lines, over);
    return rc ? rc : (fail ? 1 : 0);
}

int CmdFont(const Args& a) {
    if (!a.count("out")) { Usage(); return 2; }
    std::vector<uint8_t> font;
    if (a.count("font")) {
        if (!ReadAll(a.at("font"), font)) { std::cerr << "no se puede leer la fuente\n"; return 2; }
    } else if (a.count("disc")) {
        DiscFiles d;
        std::vector<uint8_t> f13;
        std::vector<Chunk> ch;
        std::string e;
        if (!d.Open(a.at("disc")) || !d.Read(13, f13) || !WalkChunks(f13.data(), f13.size(), ch, e)) { std::cerr << "no se pudo leer F0013\n"; return 2; }
        for (const auto& c : ch) {
            if ((c.type & 0xFFFF) != 0x201 || c.size < 12) continue;
            uint32_t us; std::memcpy(&us, f13.data() + c.offset + 8, 4);
            if (!LzDecompress(f13.data() + c.offset + 12, c.size - 12, us, font)) font.clear();
            break;
        }
        if (font.empty()) { std::cerr << "chunk de fuente no decodificado\n"; return 2; }
    } else { Usage(); return 2; }
    const unsigned first = a.count("first") ? unsigned(std::strtoul(a.at("first").c_str(), nullptr, 0)) : 0;
    const unsigned count = a.count("count") ? unsigned(std::strtoul(a.at("count").c_str(), nullptr, 0)) : 512;
    const auto bmp = FontSheetBmp(font, first, count);
    if (!WriteAll(a.at("out"), bmp.data(), bmp.size())) return 3;
    std::printf("hoja %u glifos desde %u (32 por fila; id = primero + fila*32 + columna)\n", count, first);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { Usage(); return 2; }
    Args a;
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        if (k.rfind("--", 0) != 0) { Usage(); return 2; }
        a[k.substr(2)] = (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) ? argv[++i] : "";
    }
    const std::string c = argv[1];
    if (c == "extract") return CmdExtract(a);
    if (c == "insert") return CmdInsert(a);
    if (c == "verify") return CmdVerify(a);
    if (c == "font-sheet") return CmdFont(a);
    Usage();
    return 2;
}
