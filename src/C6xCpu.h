#pragma once

// The C674x CPU running machine code: execute packets fetched from memory, issued one a cycle, each
// instruction reading its sources and writing its results in the pipeline phases SPRUFE8 chapter 4 gives
// it - so a result is seen only by packets issued after its last delay slot, a branch lands after five,
// a load's word comes back in E5 and a store's goes out in E3 - multi-cycle NOPs, the compact header's
// PROT, the control registers, and the software pipelined loop buffer (chapter 7). What the program
// asks of a debugger - CIO through C$$IO$$, the stop at C$$EXIT - is answered by the host side (C6xHost).

#include "C6xDecode.h"
#include "C6xMem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace c6x {

// The decoder's control register (an index into C6xIsa's table) as Cpu6x's CtrlId, or -1.
int ctrlOf(int64_t tableIndex);

class Cpu6x {
public:
    explicit Cpu6x(Memory &mem);

    // Registers: 0-31 A0-A31, 32-63 B0-B31.
    uint32_t reg(int r) const { return r_[r]; }
    void setReg(int r, uint32_t v) { r_[r] = v; }
    uint32_t pc() const { return pc_; }
    void setPc(uint32_t pc) { pc_ = pc; }
    // Control registers by name (the CR_ constants below).
    uint32_t ctrl(int c) const { return cr_[c]; }
    void setCtrl(int c, uint32_t v) { cr_[c] = v; }
    uint64_t cycles() const { return cycle_; }
    uint64_t packets() const { return packets_; }
    Memory &memory() { return mem_; }

    // One step: one execute packet issued - with the cycles its NOPs or a loop-buffer epilog hold the machine
    // for - or, while the loop buffer runs, one cycle. Returns false once the CPU has stopped (fault or IDLE).
    bool step();
    // Let everything in flight land: what a debugger shows after a halt.
    void drain();
    // A register as a halted debugger shows it - with every result already in flight landed, a pending load's
    // word read from memory as it stands - without the machine moving on (--trace's default view).
    uint32_t landedReg(int r) const;
    uint32_t landedCtrl(int c) const;
    // Stores in flight reach memory now, without a cycle passing: what a debugger halted at a breakpoint sees.
    void flushStores();
    bool stopped() const { return stopped_; }
    const std::string &stopReason() const { return why_; }

    // Called before each packet issues; returning false holds the CPU there (a breakpoint: C$$IO$$, C$$EXIT).
    std::function<bool(uint32_t pc)> beforeIssue;
    std::function<void(uint32_t pc, const std::vector<const Insn *> &packet)> onIssue;
    std::string (*whereFn)(uint32_t) = nullptr;

    enum CtrlId {
        CR_AMR, CR_CSR, CR_IFR, CR_ISR, CR_ICR, CR_IER, CR_ISTP, CR_IRP, CR_NRP, CR_TSCL, CR_TSCH, CR_ILC,
        CR_RILC, CR_REP, CR_PCE1, CR_DNUM, CR_FADCR, CR_FAUCR, CR_FMCR, CR_SSR, CR_GPLYA, CR_GPLYB, CR_GFPGFR,
        CR_TSR, CR_ITSR, CR_NTSR, CR_EFR, CR_ECR, CR_IERR, CR_count
    };
    static const char *ctrlName(int c);

private:
    Memory &mem_;
    uint32_t r_[64];
    uint32_t cr_[CR_count];
    uint32_t pc_ = 0;
    uint64_t cycle_ = 0, packets_ = 0;
    bool stopped_ = false;
    std::string why_;
    uint64_t tscBase_ = 0; bool tscRunning_ = false;

    // ---- decoding, cached a fetch packet at a time; a store into a cached packet drops it ----
    struct Reader : CodeReader {
        Memory *m;
        bool read32(uint32_t a, uint32_t &w) const override { if (!m->mapped(a)) return false; w = m->read32(a); return true; }
    } reader_;
    std::unordered_map<uint32_t, std::vector<Insn>> fpCache_;
    std::vector<std::vector<Insn>> stale_;    // packets a store replaced: in-flight work may still point into them
    void codeWritten(uint32_t addr);
    const std::vector<Insn> &fetchPacket(uint32_t fp);
    // The execute packet at pc: its instructions, and the address after it.
    std::vector<const Insn *> executePacket(uint32_t pc, uint32_t &next);

    // ---- the pipeline ----
    // A result in flight: the register (0-63) or control register (64 + CtrlId), and the cycle it lands at the end of.
    struct Write { uint64_t at; int reg; uint32_t value; const Insn *by; };
    std::vector<Write> writes_;
    // Each register's recent values, so a source read in E2..E4 sees the register as it was then.
    struct Hist { uint64_t at[8]; uint32_t v[8]; unsigned n = 0; };
    Hist hist_[64];
    uint32_t valueAt(int reg, uint64_t cycle) const;    // the value seen by a read in that cycle
    void land(uint64_t cycle);

    // An instruction issued and not yet executed: it executes in the cycle of its last source read.
    struct Exec { const Insn *in; uint64_t issue; uint64_t at; uint32_t packetPc, nextPc; bool fromBuffer; };
    std::vector<Exec> execs_;
    // Memory accesses in E3: a load's read and a store's write; the load's data lands in E5.
    struct MemOp { uint64_t at; bool store; uint32_t addr; uint8_t size; uint64_t value; int reg; bool pair; const Insn *by; bool signExt; uint64_t landAt; };
    std::vector<MemOp> memops_;
    struct Branch { uint64_t at; uint32_t target; };
    std::vector<Branch> branches_;
    int nopLeft_ = 0;                    // cycles the current packet's multi-cycle NOP still holds

    void cycleEnd();
    void issue(const std::vector<const Insn *> &packet, uint32_t packetPc, uint32_t nextPc, bool fromBuffer);
    void execute(const Exec &x);
    void scheduleWrite(const Insn &in, unsigned operand, uint64_t issue, uint64_t value);
    void writeReg(int reg, uint32_t value, uint64_t at, const Insn *by);
    uint64_t readOperand(const Exec &x, unsigned k) const;
    uint32_t effectiveAddress(const Exec &x, const Operand &o, uint32_t &newBase, bool &updates) const;
    bool predicate(const Insn &in, uint64_t cycle) const;
    void fault(const std::string &what, const Insn *in = nullptr);
    void setSat() { cr_[CR_CSR] |= 1u << 9; }
    uint32_t readCtrl(int c, uint64_t cycle) const;

    // ---- the software pipelined loop buffer (SPRUFE8 chapter 7) ----
    struct LoopEntry { const Insn *in; int slot; int load; bool valid; bool next; uint64_t from; };
    struct Loop {
        bool active = false;
        int kind = 0;                    // 0 SPLOOP, 1 SPLOOPD, 2 SPLOOPW
        int ii = 1, lbc = 0, load = 0, dynlen = -1;
        bool loading = false, kernelDone = false, initialTerm = false, termPending = false;
        bool draining = false; int drain = 0; int fetchDelay = 0; bool spkernelR = false;
        bool fetch = true; int progLeft = 0;
        uint64_t start = 0;
        int predReg = -1; bool predZero = false;
        std::unordered_map<uint64_t, bool> cond;
        bool reloadArmed = false, reloadStart = false, reloading = false; int rload = 0;
        bool prevActive = false; int plbc = 0, pdrain = 0;
        std::vector<LoopEntry> buf;
    };
    Loop lp_;
    void loopStart(const Insn &in);
    bool loopBoundary();
    void loopIdle();
    void loopCycle();
};

} // namespace c6x
