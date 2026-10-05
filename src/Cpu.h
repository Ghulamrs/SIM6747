#pragma once

// The C674x as this emulator models it: the two register files, a program counter, and the
// pipeline's one visible property - a result lands some cycles after its instruction issues, and
// a branch takes effect five packets after it - which is what the NOPs in the emitted code are for. The rest of the model is in README.md, "What it models".

#include "Program.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Runtime;

class Cpu {
public:
    Cpu(Program &prog, Runtime &rt);

    // Run from main until exit; returns the exit status.
    int run(uint32_t entry, bool trace);

    // What the runtime needs.
    uint32_t reg(int r) const { return r_[r]; }
    void setReg(int r, uint32_t v) { r_[r] = v; }
    uint64_t pair(int lo) const { return (uint64_t(r_[lo + 1]) << 32) | r_[lo]; }
    void setPair(int lo, uint64_t v) { r_[lo] = uint32_t(v); r_[lo + 1] = uint32_t(v >> 32); }
    uint8_t load8(uint32_t a);
    uint16_t load16(uint32_t a);
    uint32_t load32(uint32_t a);
    uint64_t load64(uint32_t a);
    void store8(uint32_t a, uint8_t v);
    void store16(uint32_t a, uint16_t v);
    void store32(uint32_t a, uint32_t v);
    void store64(uint32_t a, uint64_t v);
    std::string readString(uint32_t a);
    void writeBytes(uint32_t a, const void *p, uint32_t n);
    void exitWith(int code) { running_ = false; exitCode_ = code; }
    void resume() { running_ = true; }   // to run atexit handlers after exit
    int exitCode() const { return exitCode_; }
    void jumpTo(uint32_t target) { pc_ = target; }   // for longjmp: immediate
    // Call a function in the program from the runtime (qsort's comparator):
    // A4 and B4 as arguments, A4 back. Runs nested until it returns.
    uint32_t callback(uint32_t fn, uint32_t a4, uint32_t b4);
    [[noreturn]] void fault(const std::string &what);
    Program &program() { return prog_; }
    // Counted always: cycles as the core issues them - one a packet, n for a NOP n - and the
    // packets and native library calls among them. A native call is one cycle, not TI's rts6740.
    uint64_t cycle() const { return cycle_; }
    uint64_t packets() const { return packets_; }
    uint64_t nativeCalls() const { return nativeCalls_; }

    // -p: cycles, packets and entries charged to the function each packet ran in - the names on
    // code that are not labels - and native library calls by name. Nothing is charged unless on.
    struct ProfileRow { std::string name; uint64_t cycles = 0, packets = 0, entries = 0; bool native = false; };
    void startProfile();
    std::vector<ProfileRow> profile() const;
    std::string where(uint32_t pc) const;

    // The C674x file: A0-A31 and B0-B31. The compilers here use the first
    // sixteen of each; cl6x uses all of them.
    static const int A = 0, B = 32;
    static const int A4 = 4, B3 = 32 + 3, B4 = 32 + 4, B15 = 32 + 15, A15 = 15;
    // The loop buffer's inner loop count and its reload copy, numbered after the files for MVC.
    static const int ILC = 64, RILC = 65;

private:
    Program &prog_;
    Runtime &rt_;
    uint32_t r_[66];
    uint32_t pc_ = 0;
    uint32_t packetEnd_ = 0;       // the address after the packet executing - a call's return
    uint64_t cycle_ = 0;
    uint64_t packets_ = 0;
    uint64_t nativeCalls_ = 0;
    bool running_ = true;
    int exitCode_ = 0;
    bool trace_ = false;

    // A result in flight, with the instruction that issued it: two landing in one register in one cycle is the
    // write conflict SPRUFE8 3.8.8 forbids - undefined on the part, a hardware exception on the C674x - and a fault here.
    struct Pending { uint64_t at; int reg; uint32_t value; const Instr *by; };
    const Instr *issuing_ = nullptr;
    void checkConflict(const Pending &p, const std::vector<Pending> &others);
    std::vector<Pending> pending_;
    // A double-precision source is read in two phases - the low word at issue, the high word a
    // cycle later (SPRUFE8's E1 and E2 reads) - so such an instruction is held with its low words and completes a cycle on, with the high words as they are then.
    // Its result lands low word first, a cycle before the high one, which is what the delay-slot count names. cl6x schedules to exactly this; c90 pads past it.
    struct Deferred { const Instr *in; uint32_t lo1, lo2; };
    std::vector<Deferred> deferred_;
    bool completing_ = false;
    uint32_t deferLo1_ = 0, deferLo2_ = 0;
    void completeDeferred();
    void writePairSplit(std::vector<Pending> &w, int lo, uint64_t v, int delay);
    // Branches in flight, each landing five packets after it issued: a branch in another's delay slots is
    // legal on the C674x and cl6x writes them, so they are a queue rather than one pending target.
    struct Branch { uint64_t at; uint32_t target; };
    std::vector<Branch> branches_;

    // The software pipelined loop buffer (SPRUFE8 chapter 7), active from an SPLOOP(D/W) packet until it goes idle.
    // Each entry is an instruction loaded from the first iteration, at its LBC slot with its loading counter; `valid` is
    // the running invocation's bit and `next` the bit a reload sets for the invocation that overlaps its epilog.
    struct LoopEntry { const Instr *in; int slot; int load; bool valid; bool next; uint64_t from; };
    struct Loop {
        bool active = false;
        int kind = 0;                  // 0 SPLOOP, 1 SPLOOPD, 2 SPLOOPW
        int ii = 1, lbc = 0, load = 0, dynlen = -1;
        bool loading = false, kernelDone = false, initialTerm = false, termPending = false;
        bool draining = false; int drain = 0; int fetchDelay = 0; bool spkernelR = false;
        bool fetch = true; int progLeft = 0;  // program memory fetch, and the cycles its current packet still takes
        uint64_t start = 0;            // the cycle the SPLOOP(D/W) packet issued
        int predReg = -1; bool predNeg = false;   // SPLOOPW's continue condition, or SPLOOP(D)'s reload condition
        std::map<uint64_t, bool> cond; // that condition as each cycle's instructions saw it
        // A reload: the running invocation is the reloaded one, its valid bits set in load order (rload); the one it
        // replaced keeps draining its epilog on an LBC of its own - prev*, with each entry's `next` as its valid bit.
        bool reloadArmed = false, reloadStart = false, reloading = false; int rload = 0;
        bool prevActive = false; int plbc = 0, pdrain = 0;
        std::vector<LoopEntry> buf;
    };
    Loop lp_;
    void loopCycle();
    void loopStart(const Instr &in);
    bool loopBoundary(int &lbc, bool reloadInvocation);
    void loopIdle();

    void step();
    void stepOne();
    bool profiling_ = false;
    std::vector<uint32_t> fnStart_;            // function entry addresses, ascending
    std::vector<ProfileRow> fnRows_;           // one per entry above
    std::map<std::string, ProfileRow> nativeRows_;
    size_t lastFn_ = 0;
    std::string lastNative_;
    size_t functionAt(uint32_t pc);
    void applyPending();
    void tick();                       // one idle cycle
    void executePacket();
    void execute(const Instr &in, std::vector<Pending> &writes, bool &branched, uint32_t &target);
    void write(std::vector<Pending> &w, int reg, uint32_t v, int delay);
    void writePair(std::vector<Pending> &w, int lo, uint64_t v, int delay);
    uint32_t value(const Instr &in, const Operand &o);
    uint32_t address(const Instr &in, const Operand &o, int size, std::vector<Pending> &w);
    void check(uint32_t a, int size, bool aligned);
};
