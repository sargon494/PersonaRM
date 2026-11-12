#include "analyzer.h"

#include "core/hash.h"
#include "loader/disc_reader.h"
#include "recomp/cfg.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace personarm::tools {

namespace {

uint32_t LoadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::string Hex(uint32_t v, int width = 8) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%0*X", width, v);
    return buf;
}

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Extension(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "(sin extensión)";
    const std::string name = Upper(path.substr(slash == std::string::npos ? 0 : slash + 1));
    // Boot executables are named after the serial (e.g. SLPS_027.85): not a real extension.
    if (name.size() == 11 && name[4] == '_' && name[8] == '.') return "(ejecutable por serial)";
    return Upper(path.substr(dot));
}

std::string BaseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

} // namespace

// ── Hardware / code scanning ───────────────────────────────────────────

std::string HardwareBlockName(uint32_t a) {
    a &= 0x1FFFFFFFu;
    if (a < 0x1F801000u || a >= 0x1F803000u) return "";
    const uint32_t o = a - 0x1F800000u;
    if (o < 0x1040) return "Control de memoria";
    if (o < 0x1050) return "Mandos / Memory Card (JOY)";
    if (o < 0x1060) return "Serie (SIO1)";
    if (o < 0x1070) return "Tamaño de RAM";
    if (o < 0x1078) return "Interrupciones (I_STAT/I_MASK)";
    if (o < 0x1100) return "DMA";
    if (o < 0x1130) return "Timers";
    if (o >= 0x1800 && o < 0x1804) return "CD-ROM";
    if (o >= 0x1810 && o < 0x1818) return "GPU";
    if (o >= 0x1820 && o < 0x1828) return "MDEC";
    if (o >= 0x1C00 && o < 0x2000) return "SPU";
    if (o >= 0x2000) return "Expansión 2 / depuración";
    return "Otros registros de E/S";
}

CodeScan ScanMips(const uint8_t* data, size_t size) {
    CodeScan scan;
    scan.words = size / 4;

    // Register -> statically known value inside the I/O window (set by `lui` and `addiu/ori`).
    bool known[32] = {false};
    uint32_t value[32] = {0};

    auto invalidate = [&](uint32_t reg) { if (reg < 32) known[reg] = false; };

    for (size_t i = 0; i < scan.words; ++i) {
        const uint32_t w = LoadU32(data + i * 4);
        const uint32_t op = w >> 26;
        const uint32_t rs = (w >> 21) & 31;
        const uint32_t rt = (w >> 16) & 31;
        const uint32_t rd = (w >> 11) & 31;
        const uint32_t funct = w & 63;
        const uint32_t imm_u = w & 0xFFFF;
        const int32_t imm_s = static_cast<int16_t>(w & 0xFFFF);

        if (w == 0x03E00008u) ++scan.jr_ra;
        if (op == 0x09 && rs == 29 && rt == 29 && imm_s < 0) ++scan.prologues;
        if (op == 0x2B && rs == 29 && rt == 31) ++scan.ra_saves;

        // BIOS call stub: addiu $t2,$zero,0xA0|0xB0|0xC0 ; jr $t2 ; addiu $t1,$zero,N
        if ((w == 0x240A00A0u || w == 0x240A00B0u || w == 0x240A00C0u) && i + 2 < scan.words) {
            const uint32_t w1 = LoadU32(data + (i + 1) * 4);
            const uint32_t w2 = LoadU32(data + (i + 2) * 4);
            if (w1 == 0x01400008u && (w2 >> 16) == 0x2409u) {
                const char table = w == 0x240A00A0u ? 'A' : (w == 0x240A00B0u ? 'B' : 'C');
                scan.bios_calls.insert({table, w2 & 0xFFFFu});
            }
        }

        // Jumps end a straight-line run: forget what we knew.
        if (op == 2 || (op == 0 && (funct == 8 || funct == 9))) {
            std::fill(std::begin(known), std::end(known), false);
            continue;
        }

        if (op == 0x0F) { // lui
            const uint32_t hi = imm_u << 16;
            if ((hi & 0x1FFFFFFFu) == 0x1F800000u) {
                known[rt] = true;
                value[rt] = hi & 0x1FFFFFFFu;
            } else {
                invalidate(rt);
            }
            continue;
        }

        const bool is_load = (op >= 0x20 && op <= 0x26);
        const bool is_store = (op >= 0x28 && op <= 0x2E);
        if ((is_load || is_store) && known[rs]) {
            const uint32_t addr = (value[rs] + static_cast<uint32_t>(imm_s)) & 0x1FFFFFFFu;
            const std::string block = HardwareBlockName(addr);
            if (!block.empty()) {
                ++scan.hw_blocks[block];
                ++scan.hw_registers[addr];
            }
        }

        if (is_load) { invalidate(rt); continue; }
        if (is_store) continue;

        if ((op == 0x09 || op == 0x0D) && known[rs]) { // addiu / ori on a known base
            const uint32_t off = op == 0x0D ? imm_u : static_cast<uint32_t>(imm_s);
            const uint32_t v = (value[rs] + off) & 0x1FFFFFFFu;
            if (v >= 0x1F800000u && v < 0x1F803000u) { known[rt] = true; value[rt] = v; }
            else invalidate(rt);
            continue;
        }

        if (op == 0) invalidate(rd);
        else if (op >= 0x08 && op <= 0x0E) invalidate(rt);
        else if (op == 0x03) invalidate(31);                       // jal
        else if (op == 0x10 || op == 0x12) { if (rs == 0 || rs == 2) invalidate(rt); } // mfc/cfc
    }
    return scan;
}

bool LooksLikeMipsCode(const CodeScan& s) {
    if (s.words < 256) return false;
    const uint64_t evidence = static_cast<uint64_t>(s.prologues) + s.ra_saves;
    return s.prologues >= 3 && evidence * 1000ull >= s.words;
}

std::map<std::string, uint32_t> FindSdkHints(const uint8_t* data, size_t size) {
    static const char* kKeywords[] = {
        "LIBGPU", "LIBGTE", "LIBGS", "LIBSPU", "LIBSND", "LIBCD", "LIBDS", "LIBAPI", "LIBETC",
        "LIBPAD", "LIBCARD", "LIBMCRD", "LIBPRESS", "LIBMCGUI", "PSYQ", "PSY-Q"};
    std::map<std::string, uint32_t> found;
    for (const char* kw : kKeywords) {
        const size_t n = std::strlen(kw);
        if (size < n) continue;
        uint32_t count = 0;
        for (size_t i = 0; i + n <= size; ++i) {
            size_t j = 0;
            while (j < n && std::toupper(static_cast<unsigned char>(data[i + j])) == kw[j]) ++j;
            if (j == n) { ++count; i += n - 1; }
        }
        if (count) found[kw] = count;
    }
    return found;
}

ExeHeader ParsePsxExeHeader(const uint8_t* data, size_t size) {
    ExeHeader h;
    if (size < 0x800 || std::memcmp(data, "PS-X EXE", 8) != 0) return h;
    h.valid = true;
    h.entry = LoadU32(data + 0x10);
    h.gp = LoadU32(data + 0x14);
    h.load_address = LoadU32(data + 0x18);
    h.payload_size = LoadU32(data + 0x1C);
    h.sp_base = LoadU32(data + 0x30);
    h.sp_offset = LoadU32(data + 0x34);
    for (size_t i = 0x4C; i < 0x800 && data[i] != 0; ++i) {
        const char c = static_cast<char>(data[i]);
        if (c < 0x20 || c > 0x7E) break;
        h.region_marker.push_back(c);
    }
    return h;
}

// ── Disc analysis ──────────────────────────────────────────────────────

namespace {

std::string ProbeSectorKind(std::ifstream& f, uint32_t sector_size, uint32_t lba) {
    if (sector_size != 2352) return "2048 (ISO, sin subheader)";
    uint8_t s[24];
    f.clear();
    f.seekg(static_cast<std::streamoff>(lba) * 2352, std::ios::beg);
    f.read(reinterpret_cast<char*>(s), sizeof(s));
    if (f.gcount() < static_cast<std::streamsize>(sizeof(s))) return "n/d";
    static const uint8_t kSync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    if (std::memcmp(s, kSync, 12) != 0) return "n/d (sin sync)";
    if (s[15] == 1) return "Mode 1";
    if (s[15] != 2) return "n/d (modo desconocido)";
    const uint8_t sub = s[18];
    std::string kind = "Mode 2 ";
    kind += (sub & 0x20) ? "Form 2" : "Form 1";
    if (sub & 0x02) kind += " video";
    else if (sub & 0x04) kind += " audio";
    else if (sub & 0x08) kind += " datos";
    return kind;
}

uint32_t Crc32OfFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> buf(1u << 20);
    uint32_t crc = 0;
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<size_t>(f.gcount());
        if (n == 0) break;
        crc = Crc32(buf.data(), n, crc);
    }
    return crc;
}

void AnalyzeBuffer(const std::vector<uint8_t>& data, FileReport& rep) {
    rep.exe = ParsePsxExeHeader(data.data(), data.size());
    rep.is_psx_exe = rep.exe.valid;
    const size_t skip = rep.is_psx_exe ? std::min<size_t>(0x800, data.size()) : 0;
    rep.code = ScanMips(data.data() + skip, data.size() - skip);
    rep.code_candidate = !rep.is_psx_exe && LooksLikeMipsCode(rep.code);
    rep.scanned = true;
}

} // namespace

bool AnalyzeDisc(const std::string& image_path, const AnalyzeOptions& options,
                 DiscReport& out, std::string& error) {
    out = DiscReport{};
    DiscReader disc;
    if (!disc.Open(image_path) || disc.GetFormat() == DiscFormat::Unknown) {
        error = "no se pudo abrir la imagen o no es ISO 9660/BIN/CUE: " + BaseName(image_path);
        return false;
    }
    if (disc.GetFormat() == DiscFormat::RawExecutable) {
        error = "la entrada es un ejecutable, no una imagen de disco: " + BaseName(image_path);
        return false;
    }

    out.image_name = options.full_paths ? image_path : BaseName(image_path);
    out.sector_size = disc.GetSectorSize();
    out.format = out.sector_size == 2352 ? "BIN/CUE (sectores de 2352 bytes)" : "ISO 9660 (sectores de 2048 bytes)";
    out.volume_label = disc.GetVolumeLabel();

    const std::string bin_path = disc.GetFilePath();
    {
        std::ifstream f(bin_path, std::ios::binary | std::ios::ate);
        out.image_bytes = f ? static_cast<uint64_t>(f.tellg()) : 0;
        out.total_sectors = out.sector_size ? out.image_bytes / out.sector_size : 0;
    }
    if (options.hash) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", Crc32OfFile(bin_path));
        out.crc32_hex = buf;
    }

    // SYSTEM.CNF identifies the boot file; keep its text (a few lines, needed to reproduce the result).
    std::vector<uint8_t> cnf;
    if (disc.ReadFile("SYSTEM.CNF", cnf) && cnf.size() < 4096) {
        out.system_cnf.assign(cnf.begin(), cnf.end());
        out.system_cnf.erase(std::remove(out.system_cnf.begin(), out.system_cnf.end(), '\0'), out.system_cnf.end());
    }
    out.boot_exe = disc.DetectBootExecutable();
    const Persona2DiscInfo id = DiscReader::IdentifyPersona2(out.boot_exe);
    if (id.recognized) {
        out.product_id = id.product_id;
        out.edition = id.edition;
        out.region = id.region;
        out.disc_number = id.disc_number;
    }

    std::ifstream raw(bin_path, std::ios::binary);
    auto entries = disc.ListFiles("/");
    std::sort(entries.begin(), entries.end(),
              [](const DiscFileEntry& a, const DiscFileEntry& b) { return a.sector_lba < b.sector_lba; });

    for (const auto& e : entries) {
        if (e.is_directory) { ++out.directories; continue; }
        FileReport rep;
        rep.path = e.name;
        rep.lba = e.sector_lba;
        rep.size = e.size_bytes;
        rep.sector_kind = ProbeSectorKind(raw, out.sector_size, e.sector_lba);

        auto& ext = out.extensions[Extension(e.name)];
        ++ext.files;
        ext.bytes += e.size_bytes;

        const bool is_boot = !out.boot_exe.empty() && Upper(BaseName(e.name)) == Upper(out.boot_exe);
        // Streaming/audio payloads are never code; skip them without reading.
        const std::string x = Extension(e.name);
        const bool media = (x == ".STR" || x == ".XA" || x == ".VAG" || x == ".VB" || x == ".VH" || x == ".SEQ");
        if (!media && (is_boot || (e.size_bytes >= 0x100 && e.size_bytes <= options.max_scan_bytes))) {
            std::vector<uint8_t> data;
            if (disc.ReadFile(e.name, data) && !data.empty()) {
                AnalyzeBuffer(data, rep);
                if (is_boot) {
                    out.sdk_hints = FindSdkHints(data.data(), data.size());
                    if (options.cfg && rep.is_psx_exe) {
                        const size_t payload = std::min<size_t>(rep.exe.payload_size, data.size() - 0x800);
                        out.boot_cfg = recomp::AnalyzeControlFlow(data.data() + 0x800, payload,
                                                                  rep.exe.load_address, rep.exe.entry);
                        recomp::CfgOptions follow;
                        follow.follow_assigned_pointers = true;
                        follow.second_level_bases = true;
                        out.boot_cfg_follow = recomp::AnalyzeControlFlow(data.data() + 0x800, payload,
                                                                         rep.exe.load_address, rep.exe.entry, follow);
                        out.has_cfg = true;
                    }
                }
            }
        } else if (!media && e.size_bytes > options.max_scan_bytes) {
            ++out.skipped_large;
        }
        if (is_boot) { out.boot = rep; out.has_boot = true; }
        out.files.push_back(std::move(rep));
    }
    return true;
}

// ── Markdown ───────────────────────────────────────────────────────────

namespace {

std::string Mib(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

void RenderHardware(std::ostringstream& os, const CodeScan& scan) {
    if (scan.hw_blocks.empty()) {
        os << "No se detectaron accesos directos a registros de hardware (heurística: `lui 0x1F80` + carga/almacenamiento).\n\n";
        return;
    }
    os << "| Bloque de hardware | Accesos con dirección conocida |\n|---|---:|\n";
    for (const auto& [block, n] : scan.hw_blocks) os << "| " << block << " | " << n << " |\n";
    os << "\n";
}

} // namespace

std::string RenderMarkdown(const std::vector<DiscReport>& reports) {
    std::ostringstream os;
    os << "# Informe de análisis de discos (psx_analyze)\n\n"
       << "- Solo metadatos y estadísticas: el informe no contiene datos del juego.\n"
       << "- Las secciones marcadas *heurística* son lecturas lineales de palabras MIPS-I; no sustituyen al análisis del recompilador.\n"
       << "- Solo se recorre la pista de datos (una imagen BIN/CUE con pistas de audio puede tener más).\n\n";

    for (const auto& d : reports) {
        os << "## " << d.image_name << "\n\n";
        os << "| Campo | Valor |\n|---|---|\n";
        os << "| Formato | " << d.format << " |\n";
        os << "| Tamaño de imagen | " << d.image_bytes << " bytes (" << Mib(d.image_bytes) << "), " << d.total_sectors << " sectores |\n";
        if (!d.crc32_hex.empty()) os << "| CRC32 de la imagen | `" << d.crc32_hex << "` |\n";
        os << "| Etiqueta de volumen | " << (d.volume_label.empty() ? "(vacía)" : d.volume_label) << " |\n";
        os << "| Ejecutable de arranque (SYSTEM.CNF) | " << (d.boot_exe.empty() ? "(no detectado)" : d.boot_exe) << " |\n";
        if (!d.product_id.empty()) {
            os << "| Serial reconocido | " << d.product_id << " — " << d.region << ", " << d.edition
               << (d.disc_number ? ", disco " + std::to_string(d.disc_number) : "") << " |\n";
        } else {
            os << "| Serial reconocido | no (no figura en la tabla de seriales de PersonaRM) |\n";
        }
        os << "| Archivos / directorios | " << d.files.size() << " / " << d.directories << " |\n\n";

        if (!d.system_cnf.empty()) {
            os << "### SYSTEM.CNF\n\n```\n" << d.system_cnf << "\n```\n\n";
        }

        if (d.has_boot && d.boot.is_psx_exe) {
            const auto& h = d.boot.exe;
            os << "### Ejecutable de arranque\n\n| Campo | Valor |\n|---|---|\n";
            os << "| LBA / tamaño | " << d.boot.lba << " / " << d.boot.size << " bytes |\n";
            os << "| Dirección de carga | " << Hex(h.load_address) << " |\n";
            os << "| Tamaño del código+datos | " << h.payload_size << " bytes |\n";
            os << "| Punto de entrada (PC) | " << Hex(h.entry) << " |\n";
            os << "| GP | " << Hex(h.gp) << " |\n";
            os << "| SP base / offset | " << Hex(h.sp_base) << " / " << Hex(h.sp_offset) << " |\n";
            os << "| Marca de región | " << (h.region_marker.empty() ? "(vacía)" : h.region_marker) << " |\n";
            os << "| Palabras MIPS / prólogos de función / `jr $ra` | " << d.boot.code.words << " / "
               << d.boot.code.prologues << " / " << d.boot.code.jr_ra << " |\n\n";

            os << "#### Acceso directo a hardware (heurística)\n\n";
            RenderHardware(os, d.boot.code);

            os << "#### Llamadas a BIOS (heurística)\n\n";
            if (d.boot.code.bios_calls.empty()) {
                os << "No se encontraron *stubs* de llamada a BIOS en el ejecutable de arranque.\n\n";
            } else {
                os << d.boot.code.bios_calls.size() << (d.boot.code.bios_calls.size() == 1 ? " función distinta: " : " funciones distintas: ");
                bool first = true;
                for (const auto& [table, fn] : d.boot.code.bios_calls) {
                    os << (first ? "" : ", ") << table << "(" << Hex(fn, 2) << ")";
                    first = false;
                }
                os << "\n\n";
            }

            if (d.has_cfg) {
                const auto& c = d.boot_cfg;
                os << "#### Control de flujo desde el punto de entrada (CFG)\n\n";
                os << "| Medida | Valor |\n|---|---:|\n";
                os << "| Palabras alcanzadas / total | " << c.reachable_words << " / " << c.text_words
                   << " (" << c.coverage_permille / 10 << "." << c.coverage_permille % 10 << " %) |\n";
                os << "| Funciones (entrada + llamadas directas distintas) | " << c.functions << " |\n";
                os << "| Llamadas directas / saltos directos / ramas condicionales | " << c.direct_calls << " / "
                   << c.direct_jumps << " / " << c.conditional_branches << " |\n";
                os << "| Retornos (`jr $ra`) | " << c.returns << " |\n";
                os << "| Llamadas a BIOS (`jr` a 0xA0/0xB0/0xC0) | " << c.bios_calls << " |\n";
                os << "| Saltos `jr` (sin BIOS) / sin resolver | " << c.indirect_jumps << " / " << c.unresolved_jumps << " |\n";
                os << "| Llamadas `jalr` (total) | " << c.indirect_calls << " |\n";
                os << "| `jalr` con candidato por escritura constante (no seguido) | " << c.indirect_calls_assigned << " |\n";
                os << "| `jalr` sin resolver | " << c.indirect_calls_unresolved << " |\n";
                os << "| Tablas de salto resueltas / entradas leídas | " << c.resolved_jump_tables << " / "
                   << c.resolved_table_entries << " |\n";
                os << "| Palabras reservadas alcanzadas como código | " << c.invalid_reached << " |\n";
                os << "| Destinos fuera de la imagen | " << c.out_of_range_targets << " |\n";
                os << "| Llamadas al sistema (syscall/break) | " << c.syscalls << " |\n\n";
                {
                    const auto& f = d.boot_cfg_follow;
                    os << "Seguimiento de candidatos de `jalr` y bases de segundo nivel (experimental; una escritura constante no prueba que el valor sea una función válida, y el contenido de memoria puede cambiar en ejecución):\n\n";
                    os << "| Medida | Sin seguir | Siguiendo |\n|---|---:|---:|\n";
                    os << "| Palabras alcanzadas | " << c.reachable_words << " | " << f.reachable_words << " ("
                       << f.coverage_permille / 10 << "." << f.coverage_permille % 10 << " %) |\n";
                    os << "| Funciones | " << c.functions << " | " << f.functions << " |\n";
                    os << "| `jalr` seguidos / funciones añadidas | 0 / 0 | " << f.indirect_calls_followed << " / "
                       << f.followed_candidates << " |\n";
                    os << "| `jalr` con base no constante | " << c.pointers.unrecognised << " | " << f.pointers.unrecognised << " |\n";
                    os << "| `jalr` total / sin resolver | " << c.indirect_calls << " / " << c.indirect_calls_unresolved
                       << " | " << f.indirect_calls << " / " << f.indirect_calls_unresolved << " |\n\n";
                }
                if (!c.indirect_sites.empty()) {
                    os << "Primeros saltos indirectos sin resolver: ";
                    for (size_t i = 0; i < c.indirect_sites.size() && i < 10; ++i) os << (i ? ", " : "") << Hex(c.indirect_sites[i]);
                    os << "\n\n";
                }
                const auto& pt = c.pointers;
                os << "\nPunteros de función cargados de memoria (`jalr`, por el valor del slot en la imagen):\n\n";
                os << "| Estado | Sitios |\n|---|---:|\n";
                os << "| Slot a cero, asignado por escrituras con dirección dentro de la imagen | " << pt.assigned_in_image << " |\n";
                os << "| Slot a cero sin escritura constante encontrada | " << pt.null_value << " |\n";
                os << "| Apunta dentro de la imagen | " << pt.in_image_value << " |\n";
                os << "| Apunta fuera de la imagen | " << pt.value_outside << " |\n";
                os << "| Slot fuera de la imagen (RAM/BSS) | " << pt.address_outside << " |\n";
                os << "| Base no constante (no localizable) | " << pt.unrecognised << " |\n\n";
                if (!c.pointer_samples.empty()) {
                    os << "Primeros slots (diagnóstico local):\n\n```\n";
                    for (const auto& ps : c.pointer_samples) {
                        const char* st = "no localizable";
                        switch (ps.status) {
                            case recomp::PointerStatus::Null: st = "a cero, sin escritura constante"; break;
                            case recomp::PointerStatus::AssignedInImage: st = "asignado por escritura"; break;
                            case recomp::PointerStatus::InImage: st = "dentro de la imagen"; break;
                            case recomp::PointerStatus::ValueOutside: st = "fuera de la imagen"; break;
                            case recomp::PointerStatus::AddressOutside: st = "slot fuera de la imagen"; break;
                            case recomp::PointerStatus::Unrecognised: break;
                        }
                        os << Hex(ps.site) << "  slot " << Hex(ps.address) << "  valor " << Hex(ps.value) << "  (" << st << ")";
                        for (size_t k = 0; k < ps.candidates.size(); ++k) os << (k ? ", " : "  candidatos: ") << Hex(ps.candidates[k]);
                        os << "\n";
                    }
                    os << "```\n\n";
                }
                if (!c.unresolved_contexts.empty()) {
                    os << "Contexto de los primeros sitios sin resolver (diagnóstico local: contiene código del juego, no lo compartas):\n\n```\n";
                    for (const auto& ctx : c.unresolved_contexts) {
                        for (const auto& line : ctx.window) os << line << "\n";
                        os << "\n";
                    }
                    os << "```\n\n";
                }
                if (!c.out_of_range_sites.empty()) {
                    os << "Primeros puntos que apuntan fuera de la imagen: ";
                    for (size_t i = 0; i < c.out_of_range_sites.size() && i < 10; ++i) os << (i ? ", " : "") << Hex(c.out_of_range_sites[i]);
                    os << "\n\n";
                }
                os << "_Lectura: el CFG solo sigue saltos directos. Lo que no alcanza (saltos indirectos, overlays, código generado) es lo que decidirá si hace falta el respaldo del intérprete._\n\n";
            }

            os << "#### Indicios de librerías del SDK (heurística)\n\n";
            if (d.sdk_hints.empty()) {
                os << "Ningún nombre de librería encontrado como texto. No es concluyente: el SDK puede estar enlazado sin nombres.\n\n";
            } else {
                for (const auto& [kw, n] : d.sdk_hints) os << "- " << kw << ": " << n << "\n";
                os << "\n";
            }
        } else if (d.has_boot) {
            os << "### Ejecutable de arranque\n\nEl archivo `" << d.boot_exe << "` existe pero no tiene cabecera PS-X EXE reconocible.\n\n";
        } else if (!d.boot_exe.empty()) {
            os << "### Ejecutable de arranque\n\n`" << d.boot_exe << "` figura en SYSTEM.CNF pero no se encontró en el listado.\n\n";
        }

        // Other executables and overlay candidates
        std::vector<const FileReport*> code_files;
        for (const auto& f : d.files) {
            if ((f.is_psx_exe || f.code_candidate) && !(d.has_boot && f.path == d.boot.path)) code_files.push_back(&f);
        }
        os << "### Otros ejecutables y candidatos a overlay\n\n";
        if (code_files.empty()) {
            os << "Ninguno encontrado (límite de análisis por archivo: ver `--max-scan-mb`; omitidos por tamaño: " << d.skipped_large << ").\n\n";
        } else {
            os << "| Archivo | LBA | Tamaño | Tipo | Carga | Entrada | Prólogos | Hardware directo |\n|---|---:|---:|---|---|---|---:|---|\n";
            for (const auto* f : code_files) {
                std::string hw;
                for (const auto& [block, n] : f->code.hw_blocks) hw += (hw.empty() ? "" : ", ") + block;
                os << "| " << f->path << " | " << f->lba << " | " << f->size << " | "
                   << (f->is_psx_exe ? "PS-X EXE" : "código MIPS (heurística)") << " | "
                   << (f->is_psx_exe ? Hex(f->exe.load_address) : "?") << " | "
                   << (f->is_psx_exe ? Hex(f->exe.entry) : "?") << " | "
                   << f->code.prologues << " | " << (hw.empty() ? "—" : hw) << " |\n";
            }
            os << "\nArchivos omitidos por superar el límite de análisis: " << d.skipped_large << ".\n\n";
        }

        os << "### Tipos de archivo\n\n| Extensión | Archivos | Tamaño total |\n|---|---:|---:|\n";
        for (const auto& [ext, st] : d.extensions) os << "| " << ext << " | " << st.files << " | " << Mib(st.bytes) << " |\n";
        os << "\n";

        std::map<std::string, uint32_t> kinds;
        for (const auto& f : d.files) ++kinds[f.sector_kind];
        os << "### Tipo de sector (primer sector de cada archivo)\n\n| Tipo | Archivos |\n|---|---:|\n";
        for (const auto& [k, n] : kinds) os << "| " << k << " | " << n << " |\n";
        os << "\n";

        os << "### Listado de archivos\n\n| Ruta | LBA | Tamaño (bytes) | Sector |\n|---|---:|---:|---|\n";
        for (const auto& f : d.files) os << "| " << f.path << " | " << f.lba << " | " << f.size << " | " << f.sector_kind << " |\n";
        os << "\n";
    }
    return os.str();
}

} // namespace personarm::tools
