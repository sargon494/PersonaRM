// psx_analyze — analyse PlayStation disc images and print a Markdown report.
//
//   psx_analyze <disc.cue|.bin|.iso> [<disc2> ...] [--out report.md] [--hash]
//               [--max-scan-mb N] [--full-paths]

#include "analyzer.h"

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
        "  --help              Muestra esta ayuda\n";
}

int main(int argc, char** argv) {
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
