// psx_analyze — analyse PlayStation disc images and print a Markdown report.
//
//   psx_analyze <disc.cue|.bin|.iso> [<disc2> ...] [--out report.md] [--hash]
//               [--max-scan-mb N] [--full-paths]

#include "analyzer.h"
#include "asset_tools.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace personarm::tools;

static void Usage() {
    std::cout <<
        "psx_analyze - analiza imagenes de disco de PlayStation (solo metadatos)\n\n"
        "Uso: psx_analyze <disco.cue|.bin|.iso> [<disco2> ...] [opciones]\n\n"
        "Opciones:\n"
        "  --out <fichero>     Escribe el informe Markdown en un fichero (por defecto, en pantalla)\n"
        "  --hash              Calcula el CRC32 de la imagen (lento en discos de 700 MB)\n"
        "  --max-scan-mb <N>   Tamano maximo por archivo para buscar codigo (por defecto 8)\n"
        "  --full-paths        Muestra la ruta de la imagen tal cual en vez de su nombre\n"
        "  --cfg               Analiza el alcance del control de flujo del ejecutable de arranque (WP-14)\n"
        "  --filepos <disco>                    Tabla FILEPOS.DAT verificada contra el ISO (exit 1 si hay desajustes)\n"
        "  --extract-file <disco> <idx> <out>   Extrae el fichero Fnnnn a la ruta indicada\n"
        "  --chunks <fichero>                   Recorre el flujo de chunks (tipo/tamano/offset)\n"
        "  --rle-decode <in> <out>              Decodifica un payload RLE\n"
        "  --hazards <fichero> <base-hex> [--entry <hex>]  Riesgos de load-delay en MIPS crudo\n"
        "  --help              Muestra esta ayuda\n";
}

static bool Slurp(const char* path, std::vector<uint8_t>& v) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    v.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size()));
    return true;
}

// Stand-alone sub-commands; returns -1 when argv[1] is not one of them.
static int RunSubcommand(int argc, char** argv) {
    if (argc < 2) return -1;
    const std::string c = argv[1];
    std::vector<uint8_t> d;
    if (c == "--filepos" && argc == 3) return RunFilepos(argv[2]);
    if (c == "--extract-file" && argc == 5)
        return RunExtractFile(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)), argv[4]);
    if (c == "--chunks" && argc == 3) {
        if (!Slurp(argv[2], d)) { std::cerr << "No se pudo leer " << argv[2] << "\n"; return 2; }
        std::vector<Chunk> ch; std::string err;
        const bool ok = WalkChunks(d.data(), d.size(), ch, err);
        for (const auto& k : ch) std::printf("offset=0x%zX type=0x%08X size=%u\n", k.offset, k.type, k.size);
        std::printf("chunks=%zu\n", ch.size());
        if (!ok) { std::cerr << "Error: " << err << "\n"; return 1; }
        return 0;
    }
    if (c == "--rle-decode" && argc == 4) {
        if (!Slurp(argv[2], d)) { std::cerr << "No se pudo leer " << argv[2] << "\n"; return 2; }
        std::vector<uint8_t> o;
        const bool ok = RleDecode(d.data(), d.size(), o);
        std::ofstream f(argv[3], std::ios::binary | std::ios::trunc);
        if (!f) { std::cerr << "No se pudo escribir " << argv[3] << "\n"; return 3; }
        f.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
        std::cerr << o.size() << " bytes decodificados" << (ok ? "" : " (entrada truncada)") << "\n";
        return ok ? 0 : 1;
    }
    if (c == "--hazards" && (argc == 4 || (argc == 6 && std::string(argv[4]) == "--entry"))) {
        if (!Slurp(argv[2], d)) { std::cerr << "No se pudo leer " << argv[2] << "\n"; return 2; }
        const uint32_t base = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16));
        const bool has_entry = argc == 6;
        const uint32_t entry = has_entry ? static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 16)) : 0;
        const auto hz = ScanHazards(d.data(), d.size(), base, has_entry, entry);
        for (const auto& h : hz) std::printf("0x%08X: %08X -> %08X\n", h.addr, h.load, h.next);
        std::printf("hazards=%zu (%s)\n", hz.size(), has_entry ? "alcanzable desde la entrada" : "linear (may include data)");
        return 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    if (const int rc = RunSubcommand(argc, argv); rc >= 0) return rc;
    AnalyzeOptions options;
    std::vector<std::string> inputs;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { Usage(); return 0; }
        if (a == "--hash") { options.hash = true; continue; }
        if (a == "--full-paths") { options.full_paths = true; continue; }
        if (a == "--cfg") { options.cfg = true; continue; }
        if (a == "--out" && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (a == "--max-scan-mb" && i + 1 < argc) {
            const long mb = std::strtol(argv[++i], nullptr, 10);
            if (mb < 1 || mb > 512) { std::cerr << "--max-scan-mb debe estar entre 1 y 512\n"; return 1; }
            options.max_scan_bytes = static_cast<size_t>(mb) << 20;
            continue;
        }
        if (!a.empty() && a[0] == '-') { std::cerr << "Opcion desconocida: " << a << "\n"; Usage(); return 1; }
        inputs.push_back(a);
    }
    if (inputs.empty()) { Usage(); return 1; }

    std::vector<DiscReport> reports;
    for (const auto& path : inputs) {
        DiscReport r;
        std::string error;
        std::cerr << "Analizando " << path << " ...\n";
        if (!AnalyzeDisc(path, options, r, error)) {
            std::cerr << "Error: " << error << "\n";
            return 2;
        }
        reports.push_back(std::move(r));
    }

    const std::string md = RenderMarkdown(reports);
    if (out_path.empty()) {
        std::cout << md;
    } else {
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        if (!f) { std::cerr << "No se pudo escribir " << out_path << "\n"; return 3; }
        f << md;
        std::cerr << "Informe escrito en " << out_path << "\n";
    }
    return 0;
}
