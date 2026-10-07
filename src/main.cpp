// sim6747 - run C6000 assembly as c90 and cpp11 emit it for -arch tms6747:
//   sim6747 [-t] [-m megabytes] file.s|directory [more ...] [-- args]
// The files are assembled together, the C library is provided natively, and the exit status is main's return or exit's argument; -t traces every instruction.

#include "Asm.h"
#include "C6xDis.h"
#include "C6xHost.h"
#include "Cpu.h"
#include "Runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// A directory named on the command line stands for every .s file in it, in
// name order - how RStudio hands over a program of several sources: one
// directory, one .s per source, nothing linked.
static bool isDirectory(const std::string &path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
// The assembly in a directory: .s as the compilers write it, .asm as cl6x does.
static bool isAssembly(const std::string &n) {
    return (n.size() > 2 && n.compare(n.size() - 2, 2, ".s") == 0) ||
           (n.size() > 4 && n.compare(n.size() - 4, 4, ".asm") == 0);
}
static std::vector<std::string> assemblyIn(const std::string &dir) {
    std::vector<std::string> names;
#ifdef _WIN32
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (isAssembly(f.cFileName)) names.push_back(f.cFileName); } while (FindNextFileA(h, &f));
        FindClose(h);
    }
#else
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *e = readdir(d)) if (isAssembly(e->d_name)) names.push_back(e->d_name);
        closedir(d);
    }
#endif
    std::sort(names.begin(), names.end());
    std::vector<std::string> out;
    for (const std::string &n : names) {
        if (n.find(' ') != std::string::npos) continue;      // macOS "name 2.s" duplicates
        out.push_back(dir + (dir.empty() || dir.back() == '/' || dir.back() == '\\' ? "" : "/") + n);
    }
    return out;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // The program's output byte for byte, as on Linux and TI's simulator: text mode wrote CRLF on Windows (T6).
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (argc > 1 && (std::strcmp(argv[1], "--dis") == 0 || std::strcmp(argv[1], "--dis-words") == 0))
        return c6x::disMain(argc - 1, argv + 1);
    if (argc > 1 && std::strcmp(argv[1], "--run") == 0) return c6x::runMain(argc - 1, argv + 1);
    std::vector<std::string> files, args;
    bool trace = false;
    bool counts = false;
    bool profile = false;
    Layout layout;
    bool rest = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (rest) { args.push_back(a); continue; }
        if (a == "--") { rest = true; continue; }
        if (a == "-t") { trace = true; continue; }
        if (a == "-c") { counts = true; continue; }
        if (a == "-p") { profile = true; continue; }
        if (a == "-m" && i + 1 < argc) { layout.memoryBytes = static_cast<uint32_t>(std::atoi(argv[++i])) << 20; continue; }
        if (a == "-h" || a == "--help") {
            std::printf("usage: sim6747 [-t] [-c] [-p] [-m megabytes] file.s ... [-- args]\n"
                        "  -c  on exit, a line on stderr: the cycles from main, as TI's simulator\n"
                        "      counts them (cycle.CPU: no memory stalls), and the packets and\n"
                        "      native library calls among them\n"
                        "  -p  on exit, the cycles from main charged to each function, and\n"
                        "      native library calls by name: PROFILE lines on stderr\n");
            return 0;
        }
        if (a == "--version") {
            std::printf("\xc2\xa9" "2026 G. R. Akhtar - SIM6747 (C6747 simulator) 1.1\n");
            return 0;
        }
        if (isDirectory(a)) {
            std::vector<std::string> inside = assemblyIn(a);
            if (inside.empty()) { std::fprintf(stderr, "sim6747: no .s or .asm files in %s\n", a.c_str()); return 2; }
            for (const std::string &f : inside) files.push_back(f);
            continue;
        }
        files.push_back(a);
    }
    if (files.empty()) { std::fprintf(stderr, "sim6747: no input\n"); return 2; }

    layout.prelude = Runtime::prelude();

    Program prog;
    std::string error;
    if (!assemble(files, layout, Runtime::names(), prog, error)) {
        std::fprintf(stderr, "sim6747: %s\n", error.c_str());
        return 1;
    }
    std::map<std::string, uint32_t>::const_iterator m = prog.symbols.find("main");
    if (m == prog.symbols.end()) { std::fprintf(stderr, "sim6747: no main\n"); return 1; }

    Runtime rt;
    Cpu cpu(prog, rt);

    // The stack at the top of memory, argv just below it, the heap above the
    // program. B3 is where main returns to: the exit stub.
    uint32_t top = layout.memoryBytes - 16;
    std::vector<uint32_t> argPtrs;
    std::vector<std::string> all;
    all.push_back(files[0]);
    for (const std::string &a : args) all.push_back(a);
    for (const std::string &s : all) {
        top -= static_cast<uint32_t>(s.size()) + 1;
        cpu.writeBytes(top, s.c_str(), static_cast<uint32_t>(s.size()) + 1);
        argPtrs.push_back(top);
    }
    top &= ~7u;
    top -= 4 * (static_cast<uint32_t>(argPtrs.size()) + 1);
    top &= ~7u;
    uint32_t argvAt = top;
    for (size_t i = 0; i < argPtrs.size(); i++) cpu.store32(argvAt + 4 * static_cast<uint32_t>(i), argPtrs[i]);
    cpu.store32(argvAt + 4 * static_cast<uint32_t>(argPtrs.size()), 0);
    top -= 16;
    cpu.setReg(Cpu::B15, top);
    cpu.setReg(Cpu::A15, 0);
    cpu.setReg(Cpu::B3, 0xFC);
    cpu.setReg(Cpu::A4, static_cast<uint32_t>(argPtrs.size()));
    cpu.setReg(Cpu::B4, argvAt);
    rt.setHeap(prog.dataEnd, layout.memoryBytes / 2);

    // Dynamic initialisation before main: every .init_array entry, in order.
    for (size_t i = 0; i < prog.initArray.size(); i++)
        for (uint32_t a = prog.initArray[i].first; a < prog.initArray[i].first + prog.initArray[i].second; a += 4) {
            uint32_t fn = cpu.load32(a);
            if (fn != 0) cpu.callback(fn, 0, 0);
        }
    // From main, as TI's simulator counts once the load has run to main; through the atexit
    // handlers, which run before its C$$EXIT.
    const uint64_t cycle0 = cpu.cycle(), packets0 = cpu.packets(), natives0 = cpu.nativeCalls();
    if (profile) cpu.startProfile();
    int status = cpu.run(m->second, trace);
    rt.runAtExit(cpu);
    status = cpu.exitCode();
    if (profile) {
        std::fflush(stdout);
        const uint64_t total = cpu.cycle() - cycle0;
        std::fprintf(stderr, "PROFILE %14s %6s %12s %10s  %s\n", "cycles", "%", "packets", "entries", "function");
        for (const Cpu::ProfileRow &r : cpu.profile())
            std::fprintf(stderr, "PROFILE %14llu %6.2f %12llu %10llu  %s%s\n",
                         static_cast<unsigned long long>(r.cycles), total ? 100.0 * r.cycles / total : 0.0,
                         static_cast<unsigned long long>(r.packets), static_cast<unsigned long long>(r.entries),
                         r.native ? "[native] " : "", r.name.c_str());
    }
    if (counts) {
        std::fflush(stdout);
        std::fprintf(stderr, "CYCLES count=%llu packets=%llu natives=%llu\n",
                     static_cast<unsigned long long>(cpu.cycle() - cycle0),
                     static_cast<unsigned long long>(cpu.packets() - packets0),
                     static_cast<unsigned long long>(cpu.nativeCalls() - natives0));
    }
    std::fflush(stdout);
    return status & 0xff;
}
