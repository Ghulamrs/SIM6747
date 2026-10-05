#include "C6xCpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace c6x {

static const char *const kCtrlNames[] = {
    "AMR", "CSR", "IFR", "ISR", "ICR", "IER", "ISTP", "IRP", "NRP", "TSCL", "TSCH", "ILC", "RILC", "REP", "PCE1",
    "DNUM", "FADCR", "FAUCR", "FMCR", "SSR", "GPLYA", "GPLYB", "GFPGFR", "TSR", "ITSR", "NTSR", "EFR", "ECR", "IERR"
};
const char *Cpu6x::ctrlName(int c) { return kCtrlNames[c]; }
// The decoder's control register (an index into C6xIsa's table) as this CPU's.
int ctrlOf(int64_t tableIndex) {
    static int map[64];
    static bool built = false;
    if (!built) {
        unsigned n; const ControlReg *t = controlRegs(n);
        for (unsigned i = 0; i < n && i < 64; i++) {
            map[i] = -1;
            for (int c = 0; c < Cpu6x::CR_count; c++) if (std::strcmp(kCtrlNames[c], t[i].name) == 0) map[i] = c;
        }
        built = true;
    }
    return tableIndex >= 0 && tableIndex < 64 ? map[tableIndex] : -1;
}

Cpu6x::Cpu6x(Memory &mem) : mem_(mem) {
    std::memset(r_, 0, sizeof r_);
    std::memset(cr_, 0, sizeof cr_);
    // CSR's CPU ID and revision for the C674x (SPRUFE8 2.8.4: CPU ID 0x14), and supervisor mode.
    cr_[CR_CSR] = 0x14000000u | 0x0100u;
    cr_[CR_GFPGFR] = 0x0700001Du;     // its reset value: size 7, polynomial 0x1D
    reader_.m = &mem_;
}

// ---- fetch -------------------------------------------------------------------------------------------------
const std::vector<Insn> &Cpu6x::fetchPacket(uint32_t fp) {
    std::unordered_map<uint32_t, std::vector<Insn>>::iterator i = fpCache_.find(fp);
    if (i != fpCache_.end()) return i->second;
    return fpCache_.emplace(fp, decodeFetchPacket(reader_, fp)).first->second;
}

void Cpu6x::codeWritten(uint32_t addr) {
    std::unordered_map<uint32_t, std::vector<Insn>>::iterator i = fpCache_.find(addr & ~31u);
    if (i == fpCache_.end()) return;
    stale_.push_back(std::move(i->second));
    fpCache_.erase(i);
}

std::vector<const Insn *> Cpu6x::executePacket(uint32_t pc, uint32_t &next) {
    std::vector<const Insn *> packet;
    uint32_t a = pc;
    for (int guard = 0; guard < 16; guard++) {
        const std::vector<Insn> &fp = fetchPacket(a & ~31u);
        const Insn *in = nullptr;
        for (const Insn &x : fp) if (x.addr == a) { in = &x; break; }
        if (in == nullptr) { next = a; fault("no instruction at this address (an odd address, or one inside a 32-bit word)"); return packet; }
        if (in->header) { a = (a & ~31u) + 32; continue; }    // the header is not an instruction: on to the next fetch packet
        packet.push_back(in);
        a += in->size;
        // past a compact header at the end of the fetch packet
        if ((a & 31u) == 28) { const std::vector<Insn> &f2 = fetchPacket(a & ~31u); if (!f2.empty() && f2.back().header) a += 4; }
        if (!in->parallel) break;
    }
    next = a;
    return packet;
}

// ---- registers in time -------------------------------------------------------------------------------------
uint32_t Cpu6x::valueAt(int reg, uint64_t cycle) const {
    uint32_t v = r_[reg];
    if (cycle >= cycle_) return v;
    const Hist &h = hist_[reg];
    // newest first: undo every write that landed at or after the cycle read
    for (unsigned k = 0; k < h.n && k < 8; k++) {
        unsigned i = (h.n - 1 - k) & 7;
        if (h.at[i] >= cycle) v = h.v[i]; else break;
    }
    return v;
}

void Cpu6x::writeReg(int reg, uint32_t value, uint64_t at, const Insn *by) {
    for (const Write &w : writes_)
        if (w.at == at && w.reg == reg) {
            char b[160];
            std::snprintf(b, sizeof b, "two results land in %s in one cycle (SPRUFE8 3.8.8): from 0x%08x and 0x%08x",
                          reg < 64 ? regName(reg).c_str() : kCtrlNames[reg - 64], w.by ? w.by->addr : 0, by ? by->addr : 0);
            fault(b, by);
        }
    Write w; w.at = at; w.reg = reg; w.value = value; w.by = by;
    writes_.push_back(w);
}

void Cpu6x::land(uint64_t cycle) {
    for (size_t i = 0; i < writes_.size(); ) {
        if (writes_[i].at <= cycle) {
            int r = writes_[i].reg;
            if (r < 64) {
                Hist &h = hist_[r];
                unsigned slot = h.n & 7;
                h.at[slot] = writes_[i].at; h.v[slot] = r_[r];   // the value before
                h.n++;
                r_[r] = writes_[i].value;
            } else {
                int c = r - 64;
                cr_[c] = writes_[i].value;
                if (c == CR_TSCL) { tscRunning_ = true; tscBase_ = cycle + 1; }
            }
            writes_[i] = writes_.back();
            writes_.pop_back();
        } else i++;
    }
}

void Cpu6x::fault(const std::string &what, const Insn *in) {
    if (stopped_) return;
    stopped_ = true;
    char b[64];
    std::snprintf(b, sizeof b, "at 0x%08x, cycle %llu: ", in ? in->addr : pc_, static_cast<unsigned long long>(cycle_));
    why_ = b + what;
    if (in && whereFn) why_ += " (" + whereFn(in->addr) + ")";
}

bool Cpu6x::predicate(const Insn &in, uint64_t cycle) const {
    if (in.predReg < 0) return true;
    bool nz = valueAt(in.predReg, cycle) != 0;
    return in.predZero ? !nz : nz;
}

uint32_t Cpu6x::readCtrl(int c, uint64_t cycle) const {
    switch (c) {
    case CR_TSCL: return tscRunning_ ? static_cast<uint32_t>(cycle - tscBase_) : 0;
    case CR_TSCH: return tscRunning_ ? static_cast<uint32_t>((cycle - tscBase_) >> 32) : 0;
    default: return cr_[c];
    }
}

// ---- the cycle ------------------------------------------------------------------------------------------------
void Cpu6x::cycleEnd() {
    const uint64_t c = cycle_;
    // Instructions whose last source read is this cycle.
    for (size_t i = 0; i < execs_.size(); ) {
        if (execs_[i].at == c) { Exec x = execs_[i]; execs_.erase(execs_.begin() + long(i)); execute(x); }
        else i++;
    }
    // E3: loads read memory before this cycle's stores write it.
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < memops_.size(); ) {
            MemOp &m = memops_[i];
            if (m.at != c || m.store != (pass == 1)) { i++; continue; }
            if (!m.store) {
                uint64_t v = 0;
                switch (m.size) {
                case 1: v = mem_.read8(m.addr); if (m.signExt) v = uint64_t(int64_t(int8_t(v))); break;
                case 2: v = mem_.read16(m.addr); if (m.signExt) v = uint64_t(int64_t(int16_t(v))); break;
                case 4: v = mem_.read32(m.addr); break;
                default: v = mem_.read64(m.addr); break;
                }
                writeReg(m.reg, uint32_t(v), m.landAt, m.by);
                if (m.pair) writeReg(m.reg + 1, uint32_t(v >> 32), m.landAt, m.by);
            } else {
                switch (m.size) {
                case 1: mem_.write8(m.addr, uint8_t(m.value)); break;
                case 2: mem_.write16(m.addr, uint16_t(m.value)); break;
                case 4: mem_.write32(m.addr, uint32_t(m.value)); break;
                default: mem_.write64(m.addr, m.value); break;
                }
                // code written by the program (a copy table, a loader): its fetch packets are read again
                codeWritten(m.addr);
                if (m.size == 8) codeWritten(m.addr + 7);
            }
            memops_[i] = memops_.back();
            memops_.pop_back();
        }
    land(c);
    cycle_++;
}

bool Cpu6x::step() {
    if (stopped_) return false;
    if (lp_.active) { loopCycle(); return !stopped_; }
    if (beforeIssue && !beforeIssue(pc_)) return false;
    uint32_t next = 0;
    const uint32_t at = pc_;
    std::vector<const Insn *> packet = executePacket(pc_, next);
    if (stopped_) return false;
    if (onIssue) onIssue(at, packet);
    nopLeft_ = 0;
    issue(packet, at, next, false);
    if (stopped_) return false;
    pc_ = next;
    packets_++;
    // The packet's cycle, then the cycles its NOPs hold the machine; a branch landing cuts them short.
    int extra = nopLeft_;
    for (int k = 0; ; k++) {
        cycleEnd();
        bool landed = false;
        for (size_t i = 0; i < branches_.size(); ) {
            if (branches_[i].at < cycle_) { pc_ = branches_[i].target; landed = true; branches_.erase(branches_.begin() + long(i)); }
            else i++;
        }
        if (landed && lp_.active) {
            if (lp_.reloading || lp_.prevActive) { lp_.fetch = false; lp_.progLeft = 0; }
            else lp_ = Loop();
        }
        if (landed || k >= extra || lp_.active) { if (lp_.active) lp_.progLeft = landed ? 0 : extra - k; break; }
    }
    return !stopped_;
}

void Cpu6x::flushStores() {
    for (size_t i = 0; i < memops_.size(); ) {
        MemOp &m = memops_[i];
        if (!m.store) { i++; continue; }
        uint64_t v = m.value;
        switch (m.size) {
        case 1: mem_.write8(m.addr, uint8_t(v)); break;
        case 2: mem_.write16(m.addr, uint16_t(v)); break;
        case 4: mem_.write32(m.addr, uint32_t(v)); break;
        default: mem_.write64(m.addr, v); break;
        }
        memops_[i] = memops_.back();
        memops_.pop_back();
    }
}

void Cpu6x::drain() {
    for (int k = 0; k < 16 && (!writes_.empty() || !execs_.empty() || !memops_.empty()); k++) cycleEnd();
}

// ---- issue: E1 -------------------------------------------------------------------------------------------------
static bool isLoopControl(Mnemonic m) {
    return m == MN_SPLOOP || m == MN_SPLOOPD || m == MN_SPLOOPW || m == MN_SPKERNEL || m == MN_SPKERNELR ||
           m == MN_SPMASK || m == MN_SPMASKR;
}

void Cpu6x::issue(const std::vector<const Insn *> &packet, uint32_t packetPc, uint32_t nextPc, bool fromBuffer) {
    const uint64_t c = cycle_;
    cr_[CR_PCE1] = packetPc & ~31u;
    const Insn *sploop = nullptr;
    for (const Insn *in : packet) {
        if (in->opIndex < 0) { fault("an undefined instruction word", in); return; }
        const Opcode &op = *in->opc();
        if (op.mn == MN_SPLOOP || op.mn == MN_SPLOOPD || op.mn == MN_SPLOOPW) { sploop = in; continue; }
        if (isLoopControl(op.mn)) continue;            // SPKERNEL and SPMASK: the loop buffer reads them
        // The NOPs a packet holds the machine for: NOP n's n, BNOP's and ADDKPC's n beside the instruction's own
        // cycle, CALLP's five; a predicate that is off skips the branch, not the cycles. A load in a protected
        // fetch packet is followed by four.
        int hold = 0;
        if (op.mn == MN_NOP && in->noperands == 1) hold = int(in->op[0].value) - 1;
        else if ((op.mn == MN_BNOP || op.mn == MN_ADDKPC) && in->noperands >= 2) hold = int(in->op[in->noperands - 1].value);
        else if (op.mn == MN_CALLP) hold = 5;
        if (in->prot && (op.flags & K_LOAD)) hold = std::max(hold, 4);
        if (op.mn == MN_IDLE && !fromBuffer) { stopped_ = true; why_ = "IDLE: the CPU waits for an interrupt, and none is modelled"; return; }
        if (!fromBuffer) nopLeft_ = std::max(nopLeft_, hold);
        if (op.mn == MN_NOP) continue;
        if (!predicate(*in, c)) continue;
        // It executes in the cycle of its last source read - E1 for nearly everything.
        unsigned last = 1;
        for (unsigned k = 0; k < in->noperands; k++) {
            const OperandInfo &oi = op.op[k];
            if (oi.rw != RW_r && oi.rw != RW_rw) continue;
            if (op.flags & K_STORE) continue;          // a store's data: read in E1 by its memory operation
            last = std::max<unsigned>(last, std::max(oi.lowFirst, oi.highFirst));
        }
        Exec x; x.in = in; x.issue = c; x.at = c + last - 1; x.packetPc = packetPc; x.nextPc = nextPc; x.fromBuffer = fromBuffer;
        if (x.at == c) execute(x); else execs_.push_back(x);
        if (stopped_) return;
    }
    if (sploop) loopStart(*sploop);
}

// ---- operands ---------------------------------------------------------------------------------------------------
// A source as the instruction reads it: a register in its read cycle, a pair's halves in theirs, a 40-bit long
// sign-extended from its eight high bits.
uint64_t Cpu6x::readOperand(const Exec &x, unsigned k) const {
    const Operand &o = x.in->op[k];
    const OperandInfo &oi = x.in->opc()->op[k];
    uint64_t lowAt = x.issue + (oi.lowFirst ? oi.lowFirst : 1) - 1, highAt = x.issue + (oi.highFirst ? oi.highFirst : oi.lowFirst ? oi.lowFirst : 1) - 1;
    switch (o.kind) {
    case Operand::Reg: return valueAt(o.reg, lowAt);
    case Operand::Pair: {
        uint64_t lo = valueAt(o.reg, lowAt), hi = valueAt(o.reg + 1, highAt);
        if (oi.size == 5) return uint64_t((int64_t((hi & 0xff) << 32 | lo) << 24) >> 24);
        return hi << 32 | lo;
    }
    case Operand::Const: case Operand::Target: return uint64_t(o.value);
    case Operand::Ctrl: { int cr = ctrlOf(o.value); return cr < 0 ? 0 : readCtrl(cr, lowAt); }
    default: return 0;
    }
}

} // namespace c6x
