// trace_compare: command-line front end for the PRMTRACE v1 comparator (docs/TRACE_FORMAT.md).
//   trace_compare <A.jsonl> <B.jsonl> [--resync <ev>] [--ignore-ev a,b] [--ignore-field a,b] [--context N]
// Without --resync: first divergence (CompareTraces). With --resync load_file: one verdict per segment
// between consecutive load_file events (CompareTracesBySegment). Exit 0 = no divergence, 1 = divergence,
// 2 = usage / read error.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "trace/trace.h"

using namespace personarm::trace;

static std::set<std::string> List(const std::string& s) {
    std::set<std::string> r;
    std::stringstream ss(s);
    for (std::string t; std::getline(ss, t, ',');) if (!t.empty()) r.insert(t);
    return r;
}

static bool Load(const char* path, TraceFile* f) {
    std::ifstream in(path, std::ios::binary);
    std::string err;
    if (in && ReadTrace(in, f, &err)) return true;
    std::cerr << path << ": " << (in ? err : std::string("cannot open")) << "\n";
    return false;
}

static void Print(const Divergence& d) {
    if (!d.found) { std::cout << "  MATCH\n"; return; }
    std::cout << "  DIVERGE at " << d.index << ": " << d.reason << "\n";
    for (const auto& l : d.context_a) std::cout << "    A " << l << "\n";
    for (const auto& l : d.context_b) std::cout << "    B " << l << "\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: trace_compare <A> <B> [--resync <ev>] [--ignore-ev a,b] [--ignore-field a,b] [--context N]\n";
        return 2;
    }
    CompareOptions opt;
    std::string resync;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--resync") resync = v;
        else if (k == "--ignore-ev") for (const auto& e : List(v)) opt.ignore_events.insert(e);
        else if (k == "--ignore-field") opt.ignore_fields = List(v);
        else if (k == "--context") opt.context = std::strtoul(v.c_str(), nullptr, 10);
        else { std::cerr << "unknown option " << k << "\n"; return 2; }
    }
    TraceFile a, b;
    if (!Load(argv[1], &a) || !Load(argv[2], &b)) return 2;

    if (resync.empty()) {
        Divergence d = CompareTraces(a, b, opt);
        Print(d);
        return d.found ? 1 : 0;
    }
    int bad = 0;
    auto segs = CompareTracesBySegment(a, b, resync, opt);
    for (size_t i = 0; i < segs.size(); ++i) {
        std::cout << "segment " << i << (i ? " (from " + resync + " #" + std::to_string(i) + ")" : " (before first " + resync + ")") << "\n";
        Print(segs[i]);
        bad += segs[i].found;
    }
    std::cout << bad << " of " << segs.size() << " segments diverge\n";
    return bad ? 1 : 0;
}
