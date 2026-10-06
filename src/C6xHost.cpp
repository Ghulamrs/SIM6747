// vm6747 --run: a C6000 program as TI's tools built it - a cl6x/lnk6x .out, a hex6x image, a raw binary -
// loaded into the C6747's memory and run on the machine-code CPU from its entry point, as CCS 5.5's simulator
// runs it: TI's own boot code and rts6740 execute, and what they ask of the debugger is answered here.
//   C$$IO$$   CIO: the target's runtime has put a request in _CIOBUF_ (rts trgmsg.c's writemsg) - open, close,
//             read, write, lseek, unlink, rename, getenv, time, clock - and waits for the reply in the same buffer
//   C$$EXIT   exit and abort end there: the run stops, A4 is the status
//
//   vm6747 --run FILE [--bin ADDR] [--entry ADDR] [-c] [--trace FILE] [--steps N] [--max-cycles N]
//   -c          on exit, a line on stderr: the CPU cycles and packets from main (or --count-from ADDR), as TI's
//               simulator counts once its load has run to main, and the cycles from the entry point
//   --trace F   after every execute packet, the PC, A0-B31, the control registers and the cycle count - the
//               lines oracle/remote/trace.js writes from TI's simulator, so the two can be held line by line;
//               registers as a halted debugger shows them (results in flight landed), or with
//   --trace-view=issue  as the register file holds them at the end of the step

#include "C6xHost.h"
#include "C6xCpu.h"
#include "C6xImage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

namespace c6x {

namespace {

const Image *gImage = nullptr;
std::string whereIn(uint32_t a) { return gImage ? gImage->where(a) : std::string(); }

enum Cio : uint8_t {
    DTOPEN = 0xF0, DTCLOSE = 0xF1, DTREAD = 0xF2, DTWRITE = 0xF3, DTLSEEK = 0xF4, DTUNLINK = 0xF5,
    DTGETENV = 0xF6, DTRENAME = 0xF7, DTGETTIME = 0xF8, DTGETCLK = 0xF9, DTSYNC = 0xFF
};

struct Host {
    Cpu6x &cpu;
    Memory &mem;
    uint32_t cioBuf = 0;
    std::map<int, FILE *> files;       // by the target's descriptor
    std::map<int, int> slots, slotOf;  // the host's own slot numbers, which open answers, and back
    explicit Host(Cpu6x &c) : cpu(c), mem(c.memory()) {}

    static uint16_t ld16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
    static uint32_t ld32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
    static void st16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
    static void st32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }

    FILE *fileOf(int fd) {
        if (fd == 1) return stdout;
        if (fd == 2) return stderr;
        if (fd == 0) return stdin;
        std::map<int, FILE *>::iterator i = files.find(fd);
        return i == files.end() ? nullptr : i->second;
    }

    // One request: [length:4][command:1][parameters:8][data], answered as [length:4][parameters:8][data].
    void serve() {
        cpu.flushStores();
        uint32_t len = mem.read32(cioBuf);
        uint8_t cmd = mem.read8(cioBuf + 4);
        uint8_t parm[8];
        for (int i = 0; i < 8; i++) parm[i] = mem.read8(cioBuf + 5 + uint32_t(i));
        std::string data;
        for (uint32_t i = 0; i < len && i < 1u << 20; i++) data += char(mem.read8(cioBuf + 13 + i));
        uint8_t out[8] = {};
        std::string reply;
        switch (cmd) {
        case DTOPEN: {
            // As CCS 5.5's simulator answers (measured): the file is kept under the descriptor the target
            // names at parameter 0, every later request uses that one, and the answer is the host's own
            // lowest free slot from 3, or -1 in 32 bits for a refusal.
            std::string path(data.c_str());
            int fd = int16_t(ld16(parm));
            unsigned flags = ld16(parm + 2);
            int answer = -1;
            if (path == "stdout") answer = 1; else if (path == "stderr") answer = 2; else if (path == "stdin") answer = 0;
            else {
                // rts file.h: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 8, O_CREAT 0x200, O_TRUNC 0x400, O_BINARY 0x8000
                const char *mode = (flags & 3) == 0 ? "rb" : (flags & 8) ? ((flags & 3) == 2 ? "a+b" : "ab")
                                 : (flags & 0x400) || (flags & 0x200) ? ((flags & 3) == 2 ? "w+b" : "wb") : "r+b";
                if (FILE *f = std::fopen(path.c_str(), mode)) {
                    std::map<int, FILE *>::iterator old = files.find(fd);
                    if (old != files.end()) std::fclose(old->second);
                    files[fd] = f;
                    answer = 3;
                    while (slots.count(answer)) answer++;
                    slots[answer] = fd;
                    slotOf[fd] = answer;
                }
            }
            st32(out, uint32_t(answer));
            break;
        }
        case DTCLOSE: {
            int fd = int16_t(ld16(parm));
            int r = 0;
            if (fd > 2) {
                FILE *f = fileOf(fd);
                r = f ? std::fclose(f) : -1;
                files.erase(fd);
                std::map<int, int>::iterator s = slotOf.find(fd);
                if (s != slotOf.end()) { slots.erase(s->second); slotOf.erase(s); }
            }
            st32(out, uint32_t(r));
            break;
        }
        case DTWRITE: {
            int fd = int16_t(ld16(parm));
            unsigned count = ld16(parm + 2);
            FILE *f = fileOf(fd);
            size_t w = f ? std::fwrite(data.data(), 1, std::min<size_t>(count, data.size()), f) : 0;
            if (f) std::fflush(f);
            st16(out, f ? uint32_t(w) : 0xffffu);
            break;
        }
        case DTREAD: {
            int fd = int16_t(ld16(parm));
            unsigned count = ld16(parm + 2);
            FILE *f = fileOf(fd);
            std::string buf(count, '\0');
            size_t r = f ? std::fread(&buf[0], 1, count, f) : 0;
            buf.resize(r);
            reply = buf;
            st16(out, f ? uint32_t(r) : 0xffffu);
            break;
        }
        case DTLSEEK: {
            int fd = int16_t(ld16(parm));
            int32_t off = int32_t(ld32(parm + 2));
            int origin = int16_t(ld16(parm + 6));
            FILE *f = fileOf(fd);
            long r = -1;
            if (f && std::fseek(f, off, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END) == 0) r = std::ftell(f);
            st32(out, uint32_t(r));
            break;
        }
        case DTUNLINK: st16(out, uint32_t(std::remove(data.c_str()))); break;
        case DTRENAME: {
            std::string from(data.c_str()), to(data.size() > from.size() + 1 ? data.c_str() + from.size() + 1 : "");
            st16(out, uint32_t(std::rename(from.c_str(), to.c_str())));
            break;
        }
        case DTGETENV: { const char *v = std::getenv(data.c_str()); reply = v ? std::string(v) + '\0' : std::string(1, '\0'); break; }
        case DTGETTIME: st32(out, uint32_t(std::time(nullptr))); break;
        case DTGETCLK: st32(out, uint32_t(cpu.cycles())); break;
        default: break;
        }
        mem.write32(cioBuf, uint32_t(reply.size()));
        for (int i = 0; i < 8; i++) mem.write8(cioBuf + 4 + uint32_t(i), out[i]);
        for (size_t i = 0; i < reply.size(); i++) mem.write8(cioBuf + 12 + uint32_t(i), uint8_t(reply[i]));
    }
};

bool findSymbol(const Image &img, std::initializer_list<const char *> names, uint32_t &a) {
    for (const char *n : names) {
        std::map<std::string, uint32_t>::const_iterator i = img.symbols.find(n);
        if (i != img.symbols.end()) { a = i->second; return true; }
    }
    return false;
}

void traceHeader(FILE *t) {
    std::fprintf(t, "# REGS PC");
    for (int r = 0; r < 64; r++) std::fprintf(t, " %s", regName(r).c_str());
    for (int c = 0; c < Cpu6x::CR_count; c++) std::fprintf(t, " %s", Cpu6x::ctrlName(c));
    std::fprintf(t, " CYC\n");
}
// landed: registers as a halted debugger shows them, results in flight landed (what trace.js reads from TI's
// simulator); otherwise as they stand in the register file at the end of the step.
void traceLine(FILE *t, uint64_t k, const Cpu6x &cpu, bool landed) {
    std::fprintf(t, "S %llu %08x", static_cast<unsigned long long>(k), cpu.pc());
    for (int r = 0; r < 64; r++) std::fprintf(t, " %08x", landed ? cpu.landedReg(r) : cpu.reg(r));
    for (int c = 0; c < Cpu6x::CR_count; c++) std::fprintf(t, " %08x", landed ? cpu.landedCtrl(c) : cpu.ctrl(c));
    std::fprintf(t, " %llu\n", static_cast<unsigned long long>(cpu.cycles()));
}

} // namespace

int runMain(int argc, char **argv) {
    std::string path, tracePath;
    bool bin = false, counts = false, haveEntry = false, landedView = true, haveCountFrom = false, mainStatus = false;
    uint32_t countFrom = 0;
    uint32_t base = 0, entry = 0;
    uint64_t steps = ~uint64_t(0), maxCycles = ~uint64_t(0);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--bin" && i + 1 < argc) { bin = true; base = uint32_t(std::strtoul(argv[++i], nullptr, 16)); }
        else if (a == "--entry" && i + 1 < argc) { haveEntry = true; entry = uint32_t(std::strtoul(argv[++i], nullptr, 16)); }
        else if (a == "--trace" && i + 1 < argc) tracePath = argv[++i];
        else if (a == "--steps" && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--max-cycles" && i + 1 < argc) maxCycles = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "-c") counts = true;
        else if (a == "--main-status") mainStatus = true;
        else if (a == "--count-from" && i + 1 < argc) { haveCountFrom = true; countFrom = uint32_t(std::strtoul(argv[++i], nullptr, 16)); }
        else if (a.compare(0, 13, "--trace-view=") == 0) landedView = a.substr(13) != "issue";
        else path = a;
    }
    if (path.empty()) { std::fprintf(stderr, "usage: vm6747 --run FILE [--bin ADDR] [--entry ADDR] [-c] [--main-status] [--count-from ADDR] [--trace FILE] [--trace-view=issue] [--steps N] [--max-cycles N]\n"); return 2; }
    Image img;
    std::string why;
    if (!loadImage(path, img, why, bin, base)) { std::fprintf(stderr, "vm6747: %s\n", why.c_str()); return 1; }
    gImage = &img;
    Memory mem;
    for (const Section &s : img.sections) {
        if (!mem.mapped(s.addr) || (s.size && !mem.mapped(s.addr + s.size - 1))) {
            std::fprintf(stderr, "vm6747: section %s at 0x%08x (%u bytes) is not in the C6747's memory\n", s.name.c_str(), s.addr, s.size);
            return 1;
        }
        mem.writeBytes(s.addr, s.bytes.data(), s.bytes.size());
    }
    Cpu6x cpu(mem);
    cpu.whereFn = whereIn;
    if (!haveEntry) {
        if (!img.hasEntry) { std::fprintf(stderr, "vm6747: %s has no entry point: give one with --entry ADDR\n", path.c_str()); return 1; }
        entry = img.entry;
    }
    cpu.setPc(entry);

    Host host(cpu);
    uint32_t cioAt = 0, exitAt = 0;
    bool haveCio = findSymbol(img, { "C$$IO$$" }, cioAt);
    bool haveExit = findSymbol(img, { "C$$EXIT" }, exitAt);
    findSymbol(img, { "_CIOBUF_", "__CIOBUF_" }, host.cioBuf);
    bool exited = false;
    // TI's simulator counts from main: its loadProgram runs the boot to main before the clock is reset.
    uint32_t mainAt = 0;
    bool haveMain = haveCountFrom ? true : findSymbol(img, { "main", "_main" }, mainAt);
    if (haveCountFrom) mainAt = countFrom;
    bool atMain = false;
    uint64_t mainCycle = 0, mainPackets = 0;
    // --main-status: TI's boot calls exit(1) whatever main returned, and exit's cleanup calls reuse A4 before
    // C$$EXIT; so the status is main's A4 where it returns, else exit's argument where it is entered.
    uint32_t mainReturn = 0, mainFn = 0, exitFn = 0;
    bool haveMainFn = findSymbol(img, { "main", "_main" }, mainFn), inMain = false, mainReturned = false;
    bool haveExitFn = findSymbol(img, { "exit", "_exit" }, exitFn), exitCalled = false;
    int mainValue = 0, exitValue = 0;
    cpu.beforeIssue = [&](uint32_t pc) -> bool {
        if (haveMain && !atMain && pc == mainAt) {
            atMain = true; mainCycle = cpu.cycles(); mainPackets = cpu.packets();
        }
        if (mainStatus && !inMain && haveMainFn && pc == mainFn) { inMain = true; mainReturn = cpu.landedReg(32 + 3); }
        else if (mainStatus && inMain && !mainReturned && pc == mainReturn) {
            mainReturned = true; mainValue = int(cpu.landedReg(4));
        }
        if (mainStatus && haveExitFn && !exitCalled && pc == exitFn) { exitCalled = true; exitValue = int(cpu.landedReg(4)); }
        if (haveExit && pc == exitAt) { exited = true; return false; }
        if (haveCio && pc == cioAt && host.cioBuf) host.serve();
        return true;
    };

    FILE *trace = nullptr;
    if (!tracePath.empty()) {
        trace = std::fopen(tracePath.c_str(), "w");
        if (!trace) { std::fprintf(stderr, "vm6747: cannot write %s\n", tracePath.c_str()); return 1; }
        traceHeader(trace);
        std::fprintf(trace, "# EXIT %08x\n", haveExit ? exitAt : 0xffffffffu);
        traceLine(trace, 0, cpu, landedView);
    }
    uint64_t k = 0;
    while (k < steps && cpu.cycles() < maxCycles) {
        if (!cpu.step()) break;
        k++;
        if (trace) traceLine(trace, k, cpu, landedView);
    }
    std::fflush(stdout);
    if (trace) { std::fprintf(trace, "# END steps=%llu\n", static_cast<unsigned long long>(k)); std::fclose(trace); }
    int status = 0;
    if (exited) status = mainReturned ? mainValue : exitCalled ? exitValue : int(cpu.reg(4));
    else if (cpu.stopped()) { std::fprintf(stderr, "vm6747: %s\n", cpu.stopReason().c_str()); status = 70; }
    else if (k >= steps || cpu.cycles() >= maxCycles) { std::fprintf(stderr, "vm6747: stopped after %llu steps, %llu cycles, at 0x%08x (%s)\n",
        static_cast<unsigned long long>(k), static_cast<unsigned long long>(cpu.cycles()), cpu.pc(), img.where(cpu.pc()).c_str()); status = 71; }
    // count: from main, as cycle.CPU reads after TI's loadProgram; entry: from the entry point, the boot included
    if (counts)
        std::fprintf(stderr, "CYCLES count=%llu packets=%llu entry=%llu exit=%s status=%d\n",
                     static_cast<unsigned long long>(atMain ? cpu.cycles() - mainCycle : 0),
                     static_cast<unsigned long long>(atMain ? cpu.packets() - mainPackets : 0),
                     static_cast<unsigned long long>(cpu.cycles()), exited ? "C$$EXIT" : "none", status);
    return status & 0xff;
}

} // namespace c6x
