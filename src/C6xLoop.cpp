// The software pipelined loop buffer (SPRUFE8 chapter 7), on machine code: what cl6x writes for nearly every
// loop at -O2. An SPLOOP, SPLOOPD or SPLOOPW starts loading: each cycle's program instructions execute and are
// stored at the loop buffer count (LBC) with their loading counter, and from the next cycle the buffer replays
// them every ii cycles beside whatever program memory supplies. ILC counts iterations down at each stage
// boundary; SPLOOPW ends on its predicate as it stood three cycles before the boundary, with no epilog.
// SPKERNEL ends the loading and delays program fetch fstg stages and fcyc cycles into the epilog; SPMASK runs
// its packet's masked instructions once without storing them and inhibits the buffer's on the units it names.
// A conditional SPLOOP(D) whose condition holds four cycles before its last kernel boundary reloads.
// The same model as the assembly-level emulator's (Cpu.cpp), which was held to cl6x's -O2 loops.

#include "C6xCpu.h"

#include <algorithm>

namespace c6x {

static bool isBranch(Mnemonic m) { return m == MN_B || m == MN_BNOP || m == MN_CALLP || m == MN_BDEC || m == MN_BPOS; }
static bool isLoopCtl(Mnemonic m) {
    return m == MN_SPLOOP || m == MN_SPLOOPD || m == MN_SPLOOPW || m == MN_SPKERNEL || m == MN_SPKERNELR ||
           m == MN_SPMASK || m == MN_SPMASKR;
}
// The functional unit as SPMASK's mask names it: bit 0 L1, 1 L2, 2 S1, 3 S2, 4 D1, 5 D2, 6 M1, 7 M2.
static unsigned unitBit(const Insn &in) {
    if (in.unit == U_N || in.side == 0) return 0;
    static const int base[4] = { 4, 0, 6, 2 };     // U_D, U_L, U_M, U_S
    return 1u << (base[in.unit] + in.side - 1);
}

void Cpu6x::loopStart(const Insn &in) {
    Loop &L = lp_;
    L = Loop();
    L.active = true;
    const Mnemonic m = in.opc()->mn;
    L.kind = m == MN_SPLOOP ? 0 : m == MN_SPLOOPD ? 1 : 2;
    L.ii = in.noperands > 0 ? int(in.op[0].value) : 1;
    L.loading = true;
    L.start = cycle_;
    L.predReg = in.predReg; L.predZero = in.predZero;
    if (L.kind == 2 && L.predReg < 0) { fault("SPLOOPW without a predicate: its condition is what ends the loop", &in); return; }
    if (L.predReg >= 0) L.cond[cycle_] = predicate(in, cycle_);
    if (L.kind == 0) {
        if (cr_[CR_ILC] == 0) L.initialTerm = true; else cr_[CR_ILC]--;
    }
}

// The buffer goes idle with program memory still held by a multi-cycle NOP - BNOP's n, say, issued in the epilog.
// The cycles it has left run out here, unless its branch lands first: then the target is the next packet, and the
// packet after the BNOP never issues.
void Cpu6x::loopIdle() {
    int left = lp_.progLeft;
    lp_ = Loop();
    for (int k = 0; k < left; k++) {
        cycleEnd();
        bool landed = false;
        for (size_t i = 0; i < branches_.size(); ) {
            if (branches_[i].at < cycle_) { pc_ = branches_[i].target; landed = true; branches_.erase(branches_.begin() + long(i)); }
            else i++;
        }
        if (landed) break;
    }
}

bool Cpu6x::loopBoundary() {
    Loop &L = lp_;
    L.lbc = 0;
    const uint64_t body = cycle_ - L.start;
    if (L.kind == 2) {
        if (body <= 3) return false;
        std::unordered_map<uint64_t, bool>::const_iterator c = L.cond.find(cycle_ - 3);
        if (c != L.cond.end() && !c->second) L.termPending = true;
        return L.termPending && L.kernelDone;
    }
    if (L.initialTerm) return L.kernelDone;
    if (L.draining) return false;
    if (L.kind == 1 && body <= 3) return false;
    if (cr_[CR_ILC] != 0) { cr_[CR_ILC]--; return false; }
    L.draining = true; L.drain = 0;
    if (L.reloading) { fault("a reloaded loop ended before its reload completed - not modelled"); return false; }
    if (L.prevActive) { fault("a reloaded loop ended while the one before it was still draining - not modelled"); return false; }
    if (L.predReg >= 0) {
        std::unordered_map<uint64_t, bool>::const_iterator c = L.cond.find(cycle_ - 4);
        L.reloadArmed = c != L.cond.end() && c->second;
        if (L.reloadArmed && L.spkernelR) L.reloadStart = true;
    }
    return false;
}

void Cpu6x::loopCycle() {
    Loop &L = lp_;
    if (L.draining && L.dynlen >= 0 && L.drain >= L.dynlen - L.ii) {
        if (!L.reloadArmed) { loopIdle(); return; }
        L.draining = false; L.drain = 0;
        for (LoopEntry &e : L.buf) e.valid = false;
    }
    if (L.prevActive && L.pdrain >= L.dynlen - L.ii) L.prevActive = false;
    if (L.predReg >= 0) {
        bool nz = r_[L.predReg] != 0;
        L.cond[cycle_] = L.predZero ? !nz : nz;
        for (std::unordered_map<uint64_t, bool>::iterator i = L.cond.begin(); i != L.cond.end(); )
            if (i->first + 8 < cycle_) i = L.cond.erase(i); else ++i;
    }
    if (L.draining && L.kernelDone && !L.fetch && L.drain >= L.fetchDelay) L.fetch = true;

    // Program memory's packet for this cycle, while it is fetching; its NOPs hold it.
    std::vector<const Insn *> prog;
    uint32_t packetPc = pc_, next = pc_;
    if (L.fetch) {
        if (L.progLeft > 0) L.progLeft--;
        else {
            if (beforeIssue && !beforeIssue(pc_)) return;
            prog = executePacket(pc_, next);
            if (stopped_) return;
            if (onIssue) onIssue(pc_, prog);
            pc_ = next;
        }
    }
    unsigned masked = 0;
    bool kernel = false, kernelR = false, spmaskr = false;
    int64_t fstgfcyc = -1;
    for (const Insn *in : prog) {
        const Mnemonic m = in->opc() ? in->opc()->mn : MN_count;
        if (m == MN_SPMASK || m == MN_SPMASKR) {
            if (in->noperands > 0 && in->op[0].kind == Operand::Units) masked |= unsigned(in->op[0].value);
            if (m == MN_SPMASKR) spmaskr = true;
        }
        if (m == MN_SPKERNEL || m == MN_SPKERNELR) {
            kernel = true; kernelR = m == MN_SPKERNELR;
            if (in->noperands > 0) fstgfcyc = in->op[0].value;
        }
    }
    auto isMasked = [&](const Insn *in) { return (unitBit(*in) & masked) != 0; };

    std::vector<const Insn *> run;
    if (L.prevActive) {
        for (LoopEntry &e : L.buf) if (e.slot == L.plbc && e.load == L.pdrain) e.next = false;
        for (LoopEntry &e : L.buf) if (e.slot == L.plbc && e.next && !isMasked(e.in)) run.push_back(e.in);
    }
    if (L.draining) for (LoopEntry &e : L.buf) if (e.slot == L.lbc && e.load == L.drain) e.valid = false;
    if (L.reloading) for (LoopEntry &e : L.buf) if (e.slot == L.lbc && e.load == L.rload) { e.valid = true; e.from = 0; }
    for (LoopEntry &e : L.buf)
        if (e.slot == L.lbc && e.valid && e.from <= cycle_ && !isMasked(e.in)) run.push_back(e.in);

    std::vector<const Insn *> progRun;
    for (const Insn *in : prog) {
        if (in->opIndex < 0) { progRun.push_back(in); continue; }
        const Mnemonic m = in->opc()->mn;
        if (isLoopCtl(m)) continue;
        bool store = L.loading && !isMasked(in) && !isBranch(m);
        if (store) {
            LoopEntry e; e.in = in; e.slot = L.lbc; e.load = L.load; e.valid = !L.initialTerm; e.next = false; e.from = cycle_ + 1;
            L.buf.push_back(e);
            if (L.initialTerm) continue;
        }
        progRun.push_back(in);
    }

    nopLeft_ = 0;
    issue(run, packetPc, next, true);
    if (stopped_) return;
    issue(progRun, packetPc, next, false);
    if (stopped_) return;
    if (!prog.empty()) { L.progLeft = nopLeft_; packets_++; }

    if (kernel && L.loading) {
        L.loading = false; L.kernelDone = true;
        L.dynlen = L.load + 1;
        L.spkernelR = kernelR;
        int fstg = 0, fcyc = 0;
        if (fstgfcyc >= 0) {
            unsigned fb = L.ii <= 1 ? 0 : L.ii <= 2 ? 1 : L.ii <= 4 ? 2 : L.ii <= 8 ? 3 : 4;
            unsigned v = unsigned(fstgfcyc), t = 0;
            for (unsigned i = fb; i < 6; i++) t = (t << 1) | ((v >> i) & 1);
            fstg = int(t); fcyc = int(v & ((1u << fb) - 1));
        }
        L.fetchDelay = kernelR ? 0 : fstg * L.ii + fcyc;
        L.fetch = false; L.progLeft = 0;
    }
    if (spmaskr && L.reloadArmed) L.reloadStart = true;

    if (L.loading) L.load++;
    if (L.prevActive) { L.pdrain++; if (++L.plbc == L.ii) L.plbc = 0; }
    if (L.draining) L.drain++;
    if (L.reloading && ++L.rload >= L.dynlen) { L.reloading = false; L.fetch = false; L.progLeft = 0; }
    bool idle = false;
    if (++L.lbc == L.ii) idle = loopBoundary();
    if (L.reloadStart) {
        L.reloadStart = false; L.reloadArmed = false;
        if (cr_[CR_RILC] == 0) { fault("a reload with RILC 0 - a skipped invocation - is not modelled"); return; }
        for (LoopEntry &e : L.buf) { e.next = e.valid; e.valid = false; }
        L.prevActive = L.draining; L.plbc = L.lbc; L.pdrain = L.drain;
        L.lbc = 0; L.draining = false; L.drain = 0;
        L.reloading = true; L.rload = 0;
        cr_[CR_ILC] = cr_[CR_RILC] - 1;
    }

    cycleEnd();
    // a branch landing: the buffer goes idle, unless it is reloading - then fetch stops and the PC waits at the target
    bool landed = false;
    for (size_t i = 0; i < branches_.size(); ) {
        if (branches_[i].at < cycle_) { pc_ = branches_[i].target; landed = true; branches_.erase(branches_.begin() + long(i)); }
        else i++;
    }
    if (landed) {
        if (L.reloading || L.prevActive) { L.fetch = false; L.progLeft = 0; }
        else { lp_ = Loop(); return; }
    }
    if (idle) loopIdle();
}

} // namespace c6x
