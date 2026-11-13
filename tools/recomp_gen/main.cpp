// recomp_gen: raw MIPS image -> C++ (static recompiler v0). Offline developer tool.
//
//   recomp_gen <image> <base> <out.cpp> [--skip N] [--entries a,b,...] [--entries-file f]
//              [--cfg-entry addr] [--module name]
//
// <image> is read from byte offset --skip (0x800 for a PS-X EXE payload) and loaded at <base>.
// Entries come from the list, from a file (one address per line), and/or from the control-flow
// analysis started at --cfg-entry. The output path is required: nothing is written by default.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "recomp/cfg.h"
#include "recomp/codegen.h"

using namespace personarm::recomp;

namespace {

bool ParseU32(const std::string& s, uint32_t& v) {
    char* end = nullptr;
    const unsigned long long x = std::strtoull(s.c_str(), &end, 0);
    if (s.empty() || *end != '\0' || x > 0xFFFFFFFFull) return false;
    v = static_cast<uint32_t>(x);
    return true;
}

bool ParseList(const std::string& s, char sep, std::vector<uint32_t>& out) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) {
        while (!item.empty() && (item.back() == '\r' || item.back() == ' ')) item.pop_back();
        if (item.empty() || item[0] == '#') continue;
        uint32_t v = 0;
        if (!ParseU32(item, v)) { std::cerr << "bad address: " << item << "\n"; return false; }
        out.push_back(v);
    }
    return true;
}

int Usage() {
    std::cerr << "usage: recomp_gen <image> <base> <out.cpp> [--skip N] [--entries a,b,...] "
                 "[--entries-file f] [--cfg-entry addr] [--module name]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) return Usage();
    const std::string in_path = argv[1], out_path = argv[3];
    uint32_t base = 0, skip = 0, cfg_entry = 0;
    bool use_cfg = false;
    std::string module = "module";
    std::vector<uint32_t> entries;
    if (!ParseU32(argv[2], base)) return Usage();
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) return Usage();
        const std::string v = argv[++i];
        if (a == "--skip") { if (!ParseU32(v, skip)) return Usage(); }
        else if (a == "--entries") { if (!ParseList(v, ',', entries)) return 2; }
        else if (a == "--entries-file") {
            std::ifstream f(v);
            if (!f) { std::cerr << "cannot read " << v << "\n"; return 1; }
            if (!ParseList(std::string(std::istreambuf_iterator<char>(f), {}), '\n', entries)) return 2;
        }
        else if (a == "--cfg-entry") { if (!ParseU32(v, cfg_entry)) return Usage(); use_cfg = true; }
        else if (a == "--module") module = v;
        else return Usage();
    }

    std::ifstream f(in_path, std::ios::binary);
    if (!f) { std::cerr << "cannot read " << in_path << "\n"; return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    if (skip > data.size()) { std::cerr << "--skip beyond the end of the file\n"; return 1; }
    data.erase(data.begin(), data.begin() + skip);
    data.resize(data.size() & ~size_t{3});

    if (use_cfg) {
        const CfgReport r = AnalyzeControlFlow(data.data(), data.size(), base, cfg_entry);
        entries.insert(entries.end(), r.function_entries.begin(), r.function_entries.end());
    }
    if (entries.empty()) { std::cerr << "no entries\n"; return 2; }

    const CodegenResult res = GenerateCpp(data.data(), data.size(), base, entries, module);
    std::ofstream out(out_path, std::ios::binary);
    if (!out || !(out << res.source)) { std::cerr << "cannot write " << out_path << "\n"; return 1; }

    size_t ok = 0;
    for (const GeneratedFunction& g : res.functions) ok += g.generated ? 1 : 0;
    std::cout << "recomp_gen: " << ok << "/" << res.functions.size() << " functions generated -> " << out_path << "\n";
    for (const GeneratedFunction& g : res.functions) {
        if (!g.generated) std::printf("  skipped %08X: %s\n", g.entry, g.reason.c_str());
    }
    return 0;
}
