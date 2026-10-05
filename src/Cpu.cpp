#include "Cpu.h"

#include <algorithm>
#include "Isa.h"
#include "Runtime.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

Cpu::Cpu(Program &prog, Runtime &rt) : prog_(prog), rt_(rt) {
    std::memset(r_, 0, sizeof r_);
}

// ---- memory ------------------------------------------------------------------
void Cpu::check(uint32_t a, int size, bool aligned) {
    if (a + size > prog_.memory.size() || a + size < a)
        fault("memory access at " + std::to_string(a) + " is outside memory");
    // Nothing lives below the text - the C6747 has nothing at 0 either - so
    // a null pointer, and a small offset from one, faults here as there.
    if (a < prog_.textBase)
        fault("memory access at " + std::to_string(a) + " is through a null pointer");
    if (aligned && (a % size) != 0)
        fault("a " + std::to_string(size) + "-byte access at " + std::to_string(a) + " is not aligned");
}
uint8_t Cpu::load8(uint32_t a) { check(a, 1, false); return prog_.memory[a]; }
uint16_t Cpu::load16(uint32_t a) { check(a, 2, true); return rd16(prog_.memory, a); }
uint32_t Cpu::load32(uint32_t a) { check(a, 4, true); return rd32(prog_.memory, a); }
uint64_t Cpu::load64(uint32_t a) { check(a, 8, true); return rd32(prog_.memory, a) | (uint64_t(rd32(prog_.memory, a + 4)) << 32); }
void Cpu::store8(uint32_t a, uint8_t v) { check(a, 1, false); prog_.memory[a] = v; }
void Cpu::store16(uint32_t a, uint16_t v) { check(a, 2, true); wr16(prog_.memory, a, v); }
void Cpu::store32(uint32_t a, uint32_t v) { check(a, 4, true); wr32(prog_.memory, a, v); }
void Cpu::store64(uint32_t a, uint64_t v) { check(a, 8, true); wr32(prog_.memory, a, uint32_t(v)); wr32(prog_.memory, a + 4, uint32_t(v >> 32)); }
std::string Cpu::readString(uint32_t a) {
    std::string s;
    for (;;) { uint8_t c = load8(a++); if (c == 0) break; s += static_cast<char>(c); }
    return s;
}
void Cpu::writeBytes(uint32_t a, const void *p, uint32_t n) {
    check(a, static_cast<int>(n == 0 ? 1 : n), false);
    std::memcpy(&prog_.memory[a], p, n);
}

std::string Cpu::where(uint32_t pc) const {
    std::map<uint32_t, Instr>::const_iterator i = prog_.code.find(pc);
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%x", pc);
    std::string s = buf;
    if (i != prog_.code.end()) s += " (" + i->second.file + ":" + std::to_string(i->second.line) + ")";
    // the nearest label at or before pc
    std::map<uint32_t, std::string>::const_iterator n = prog_.names.upper_bound(pc);
    if (n != prog_.names.begin()) { --n; s += " in " + n->second + "+" + std::to_string(pc - n->first); }
    return s;
}

void Cpu::fault(const std::string &what) {
    std::fprintf(stderr, "vm6747: fault at %s, cycle %llu: %s\n", where(pc_).c_str(),
                 static_cast<unsigned long long>(cycle_), what.c_str());
    std::fprintf(stderr, "        A4=0x%x B4=0x%x A15=0x%x B3=0x%x B15=0x%x\n", r_[A4], r_[B4], r_[A15], r_[B3], r_[B15]);
    std::exit(70);
}

// ---- the pipeline --------------------------------------------------------------
void Cpu::write(std::vector<Pending> &w, int reg, uint32_t v, int delay) {
    if (delay < 0) { r_[reg] = v; return; }        // a low word already due
    Pending p; p.at = cycle_ + 1 + delay; p.reg = reg; p.value = v; p.by = issuing_;
    checkConflict(p, w);
    checkConflict(p, pending_);
    w.push_back(p);
}

static std::string regName(int r) {
    if (r == Cpu::ILC) return "ILC";
    if (r == Cpu::RILC) return "RILC";
    return (r < Cpu::B ? "A" : "B") + std::to_string(r % Cpu::B);
}
static std::string lineOf(const Instr *in) {
    return in == nullptr ? std::string("?") : in->mnem + " at " + in->file + ":" + std::to_string(in->line);
}

void Cpu::checkConflict(const Pending &p, const std::vector<Pending> &others) {
    for (const Pending &q : others)
        if (q.at == p.at && q.reg == p.reg)
            fault("two results land in " + regName(p.reg) + " in one cycle (SPRUFE8 3.8.8): " + lineOf(q.by) +
                  " and " + lineOf(p.by));
}
void Cpu::writePair(std::vector<Pending> &w, int lo, uint64_t v, int delay) {
    write(w, lo, uint32_t(v), delay);
    write(w, lo + 1, uint32_t(v >> 32), delay);
}

void Cpu::applyPending() {
    for (size_t i = 0; i < pending_.size(); ) {
        if (pending_[i].at <= cycle_) {
            r_[pending_[i].reg] = pending_[i].value;
            pending_[i] = pending_.back();
            pending_.pop_back();
        } else i++;
    }
    while (!branches_.empty() && branches_.front().at <= cycle_) {
        pc_ = branches_.front().target;
        branches_.erase(branches_.begin());
        // After a taken branch's last delay slot the loop buffer goes idle - unless it is reloading, when program
        // fetch stops and the PC waits at the target (SPRUFE8 7.7.3.2, 7.9.6.3).
        if (lp_.active) {
            if (lp_.reloading || lp_.prevActive) { lp_.fetch = false; lp_.progLeft = 0; }
            else lp_ = Loop();
        }
    }
}

void Cpu::writePairSplit(std::vector<Pending> &w, int lo, uint64_t v, int delay) {
    write(w, lo, uint32_t(v), delay - 1);
    write(w, lo + 1, uint32_t(v >> 32), delay);
}

// Which instructions read a double-precision source.
static bool readsPair(Op op) {
    switch (op) {
    case Op::ADDDP: case Op::SUBDP: case Op::MPYDP: case Op::CMPEQDP: case Op::CMPLTDP: case Op::CMPGTDP:
    case Op::ABSDP: case Op::DPINT: case Op::DPTRUNC: case Op::DPSP: case Op::RCPDP: return true;
    default: return false;
    }
}

// The cycle after issue: the held instructions run with the high words read
// now, and their results are dated from the issue cycle - one less than the
// table says, this being a cycle on.
void Cpu::completeDeferred() {
    if (deferred_.empty()) return;
    std::vector<Deferred> held;
    held.swap(deferred_);
    std::vector<Pending> writes;
    completing_ = true;
    for (const Deferred &x : held) {
        deferLo1_ = x.lo1; deferLo2_ = x.lo2;
        bool branched = false; uint32_t target = 0;
        execute(*x.in, writes, branched, target);
    }
    completing_ = false;
    for (const Pending &p : writes) pending_.push_back(p);
}

void Cpu::tick() { cycle_++; applyPending(); completeDeferred(); }

uint32_t Cpu::value(const Instr &in, const Operand &o) {
    (void)in;
    if (o.kind == Operand::Reg) return r_[o.reg];
    return static_cast<uint32_t>(o.imm);
}

// The address of a memory operand, applying its increment or decrement to the
// base register (that write lands in the next cycle, as an address-unit result).
uint32_t Cpu::address(const Instr &in, const Operand &o, int size, std::vector<Pending> &w) {
    (void)in;
    uint32_t base = r_[o.base];
    int64_t off = o.offReg >= 0 ? static_cast<int64_t>(r_[o.offReg]) : o.off;
    if (o.scaled) off *= size;
    if (o.negative) off = -off;
    uint32_t a;
    switch (o.mode) {
    case 0:  a = base + static_cast<uint32_t>(off); break;
    case 1:  a = base + static_cast<uint32_t>(off); write(w, o.base, a, 0); break;               // *++R(n)
    case 2:  a = base - static_cast<uint32_t>(off); write(w, o.base, a, 0); break;               // *--R(n)
    case 3:  a = base; write(w, o.base, base + static_cast<uint32_t>(off), 0); break;             // *R++(n)
    default: a = base; write(w, o.base, base - static_cast<uint32_t>(off), 0); break;             // *R--(n)
    }
    return a;
}

static float asFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
// A NaN result is the canonical quiet NaN with the sign clear, whichever NaN the host's
// arithmetic produced: the host varies (Windows gives the sign bit), and a program's output
// must not depend on where the emulator runs.
static uint32_t fromFloat(float f) { if (f != f) return 0x7fc00000u; uint32_t v; std::memcpy(&v, &f, 4); return v; }
static double asDouble(uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }
static uint64_t fromDouble(double d) { if (d != d) return 0x7ff8000000000000ULL; uint64_t v; std::memcpy(&v, &d, 8); return v; }

static int32_t truncToInt(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return static_cast<int32_t>(d);
}
static int32_t roundToInt(double d) {
    if (d != d) return 0;
    double r = std::nearbyint(d);
    return truncToInt(r);
}

void Cpu::execute(const Instr &in, std::vector<Pending> &w, bool &branched, uint32_t &target) {
    issuing_ = &in;
    const IsaEntry *e = isaLookup(in.mnem);
    const std::vector<Operand> &o = in.ops;
    int d = e->delaySlots;
    uint32_t s1 = o.size() > 0 && o[0].kind != Operand::Mem ? value(in, o[0]) : 0;
    uint32_t s2 = o.size() > 1 && o[1].kind != Operand::Mem ? value(in, o[1]) : 0;
    int dst = o.empty() ? -1 : o.back().reg;
    bool dstPair = !o.empty() && o.back().kind == Operand::Reg && o.back().reg2 >= 0;
    uint64_t p1 = o.size() > 0 && o[0].kind == Operand::Reg && o[0].reg2 >= 0 ? pair(o[0].reg) : s1;
    uint64_t p2 = o.size() > 1 && o[1].kind == Operand::Reg && o[1].reg2 >= 0 ? pair(o[1].reg) : s2;
    if (readsPair(e->op)) {
        if (!completing_) {
            Deferred x; x.in = &in; x.lo1 = uint32_t(p1); x.lo2 = uint32_t(p2);
            deferred_.push_back(x);
            return;
        }
        p1 = (p1 & 0xffffffff00000000ULL) | deferLo1_;
        p2 = (p2 & 0xffffffff00000000ULL) | deferLo2_;
        d--;
    }

    switch (e->op) {
    case Op::NOP: case Op::IDLE: case Op::DINT: case Op::RINT: return;
    case Op::MVD: write(w, dst, s1, d); return;
    case Op::SWE: fault("SWE: a software exception"); return;
    case Op::MVK:  write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1 & 0xffff))), 0); return;
    case Op::MVKL: write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1 & 0xffff))), 0); return;
    case Op::MVKH: write(w, dst, (s1 & 0xffff0000u) | (r_[dst] & 0xffff), 0); return;
    case Op::ADDK: write(w, dst, r_[dst] + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1 & 0xffff))), 0); return;
    case Op::MV:
        if (dstPair) writePair(w, dst, p1, 0); else write(w, dst, s1, 0);
        return;
    case Op::ZERO:
        if (dstPair) writePair(w, dst, 0, 0); else write(w, dst, 0, 0);
        return;
    case Op::ADD: case Op::ADDU: case Op::SUB: case Op::SUBU: {
        bool sub = e->op == Op::SUB || e->op == Op::SUBU;
        if (!dstPair) { write(w, dst, sub ? s1 - s2 : s1 + s2, 0); return; }
        // A 40-bit result in a pair: the low word and the top eight bits.
        uint64_t a = (e->op == Op::ADDU || e->op == Op::SUBU) ? p1 : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s1)));
        uint64_t b = (e->op == Op::ADDU || e->op == Op::SUBU) ? p2 : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s2)));
        uint64_t r = sub ? a - b : a + b;
        writePair(w, dst, r & 0xffffffffffULL, 0);
        return;
    }
    case Op::NEG: write(w, dst, 0u - s1, 0); return;
    case Op::NOT: write(w, dst, ~s1, 0); return;
    case Op::ABS: write(w, dst, static_cast<int32_t>(s1) < 0 ? 0u - s1 : s1, 0); return;
    case Op::AND: write(w, dst, s1 & s2, 0); return;
    case Op::ANDN: write(w, dst, s1 & ~s2, 0); return;
    case Op::OR:  write(w, dst, s1 | s2, 0); return;
    case Op::XOR: write(w, dst, s1 ^ s2, 0); return;
    case Op::SHL: { uint32_t c = s2 & 63; write(w, dst, c >= 32 ? 0 : s1 << c, 0); return; }
    case Op::SHR: { uint32_t c = s2 & 63; int32_t v = static_cast<int32_t>(s1); write(w, dst, static_cast<uint32_t>(c >= 32 ? (v < 0 ? -1 : 0) : v >> c), 0); return; }
    case Op::SHRU: { uint32_t c = s2 & 63; write(w, dst, c >= 32 ? 0 : s1 >> c, 0); return; }
    case Op::EXT: case Op::EXTU: case Op::SET: case Op::CLR: {
        uint32_t src = s1, csta, cstb;
        if (o.size() == 4) { csta = value(in, o[1]) & 31; cstb = value(in, o[2]) & 31; }
        else { csta = (s2 >> 5) & 31; cstb = s2 & 31; }
        uint32_t r;
        if (e->op == Op::EXT) r = static_cast<uint32_t>(static_cast<int32_t>(src << csta) >> cstb);
        else if (e->op == Op::EXTU) r = (src << csta) >> cstb;
        else {
            uint32_t mask = 0;
            if (csta <= cstb) mask = (cstb - csta == 31) ? 0xffffffffu : (((1u << (cstb - csta + 1)) - 1) << csta);
            r = e->op == Op::SET ? (src | mask) : (src & ~mask);
        }
        write(w, dst, r, 0);
        return;
    }
    case Op::CMPEQ:  write(w, dst, s1 == s2, 0); return;
    case Op::CMPLT:  write(w, dst, static_cast<int32_t>(s1) < static_cast<int32_t>(s2), 0); return;
    case Op::CMPGT:  write(w, dst, static_cast<int32_t>(s1) > static_cast<int32_t>(s2), 0); return;
    case Op::CMPLTU: write(w, dst, s1 < s2, 0); return;
    case Op::CMPGTU: write(w, dst, s1 > s2, 0); return;
    case Op::ADDAW: write(w, dst, s1 + (o[1].dpByte ? s2 : s2 * 4), 0); return;
    case Op::ADDAD: write(w, dst, s1 + s2 * 8, 0); return;
    case Op::ADDAH: write(w, dst, s1 + (o[1].dpByte ? s2 : s2 * 2), 0); return;
    case Op::ADDAB: write(w, dst, s1 + s2, 0); return;
    case Op::SUBAW: write(w, dst, s1 - s2 * 4, 0); return;
    case Op::MVC:
        // To ILC or RILC: visible to the loop buffer four cycles on (SPRUFE8 7.9.7); from one, now.
        if (dst >= ILC) write(w, dst, s1, 3); else write(w, dst, s1, 0);
        return;
    case Op::SPLOOP: case Op::SPLOOPD: case Op::SPLOOPW: case Op::SPKERNEL: case Op::SPKERNELR:
    case Op::SPMASK: case Op::SPMASKR:
        return;   // the loop buffer's: executePacket and loopCycle act on them

    case Op::MPY32:   // the 64-bit form, MPY32 src1, src2, dst_o:dst_e: a signed product in the pair
        if (dstPair) { writePair(w, dst, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s1)) * static_cast<int32_t>(s2)), d); return; }
        write(w, dst, s1 * s2, d); return;
    case Op::MPY32U:  writePair(w, dst, uint64_t(s1) * uint64_t(s2), d); return;
    case Op::MPY32SU: writePair(w, dst, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s1)) * static_cast<int64_t>(s2)), d); return;
    case Op::MPY32US: writePair(w, dst, static_cast<uint64_t>(static_cast<int64_t>(s1) * static_cast<int64_t>(static_cast<int32_t>(s2))), d); return;
    case Op::MPY:   write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1)) * static_cast<int32_t>(static_cast<int16_t>(s2))), d); return;
    case Op::MPYU:  write(w, dst, (s1 & 0xffff) * (s2 & 0xffff), d); return;
    case Op::MPYSU: write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1)) * static_cast<int32_t>(s2 & 0xffff)), d); return;
    case Op::MPYUS: write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(s1 & 0xffff) * static_cast<int32_t>(static_cast<int16_t>(s2))), d); return;
    case Op::MPYLH: write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1)) * static_cast<int32_t>(static_cast<int16_t>(s2 >> 16))), d); return;
    case Op::MPYHL: write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1 >> 16)) * static_cast<int32_t>(static_cast<int16_t>(s2))), d); return;
    case Op::MPYH:  write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s1 >> 16)) * static_cast<int32_t>(static_cast<int16_t>(s2 >> 16))), d); return;
    case Op::MPYHU: write(w, dst, (s1 >> 16) * (s2 >> 16), d); return;
    // MPYLI: src1's signed 16 LSBs by src2's signed 32 bits, the 48-bit product in a pair (SPRUFE8's example: 13,463 x 570,380,455)
    case Op::MPYLI: writePair(w, dst, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(s1)) * static_cast<int32_t>(s2)), d); return;

    case Op::LDB:  { uint32_t a = address(in, o[0], 1, w); write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(load8(a)))), d); return; }
    case Op::LDBU: { uint32_t a = address(in, o[0], 1, w); write(w, dst, load8(a), d); return; }
    case Op::LDH:  { uint32_t a = address(in, o[0], 2, w); write(w, dst, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(load16(a)))), d); return; }
    case Op::LDHU: { uint32_t a = address(in, o[0], 2, w); write(w, dst, load16(a), d); return; }
    case Op::LDW:  { uint32_t a = address(in, o[0], 4, w); write(w, dst, load32(a), d); return; }
    case Op::LDNW: { uint32_t a = address(in, o[0], 4, w); check(a, 4, false); write(w, dst, rd32(prog_.memory, a), d); return; }
    case Op::LDDW: { uint32_t a = address(in, o[0], 8, w); writePair(w, dst, load64(a), d); return; }
    case Op::LDNDW: { uint32_t a = address(in, o[0], 8, w); check(a, 8, false); writePair(w, dst, rd32(prog_.memory, a) | (uint64_t(rd32(prog_.memory, a + 4)) << 32), d); return; }
    case Op::STB:  { uint32_t a = address(in, o[1], 1, w); store8(a, static_cast<uint8_t>(s1)); return; }
    case Op::STH:  { uint32_t a = address(in, o[1], 2, w); store16(a, static_cast<uint16_t>(s1)); return; }
    case Op::STW:  { uint32_t a = address(in, o[1], 4, w); store32(a, s1); return; }
    case Op::STNW: { uint32_t a = address(in, o[1], 4, w); check(a, 4, false); wr32(prog_.memory, a, s1); return; }
    case Op::STDW: { uint32_t a = address(in, o[1], 8, w); store64(a, p1); return; }
    case Op::STNDW: { uint32_t a = address(in, o[1], 8, w); check(a, 8, false); wr32(prog_.memory, a, uint32_t(p1)); wr32(prog_.memory, a + 4, uint32_t(p1 >> 32)); return; }

    case Op::B: case Op::RET: case Op::CALL: case Op::BNOP: case Op::RETNOP:
        // CALL's return address is ADDKPC's to compute, and BNOP's NOPs are
        // the packet's concern - the branch itself is B either way.
        branched = true;
        target = s1;
        return;
    case Op::ADDKPC: write(w, o[1].reg, s1, 0); return;
    case Op::CALLP:
        // Protected call: B3 gets the return address - the packet after this one, cl6x writing
        // the CALLP beside an argument move - and the five delay slots are the packet's NOPs,
        // in which a load issued before the call lands, as cl6x's `LDW; CALLP` relies on.
        write(w, B3, packetEnd_, 0);
        branched = true;
        target = s1;
        return;

    // ---- single precision -------------------------------------------------
    case Op::ADDSP: write(w, dst, fromFloat(asFloat(s1) + asFloat(s2)), d); return;
    case Op::SUBSP: write(w, dst, fromFloat(asFloat(s1) - asFloat(s2)), d); return;
    case Op::MPYSP: write(w, dst, fromFloat(asFloat(s1) * asFloat(s2)), d); return;
    case Op::CMPEQSP: write(w, dst, asFloat(s1) == asFloat(s2), d); return;
    case Op::CMPLTSP: write(w, dst, asFloat(s1) < asFloat(s2), d); return;
    case Op::CMPGTSP: write(w, dst, asFloat(s1) > asFloat(s2), d); return;
    case Op::ABSSP: write(w, dst, s1 & 0x7fffffffu, d); return;
    case Op::INTSP:  write(w, dst, fromFloat(static_cast<float>(static_cast<int32_t>(s1))), d); return;
    case Op::INTSPU: write(w, dst, fromFloat(static_cast<float>(s1)), d); return;
    case Op::SPINT:   write(w, dst, static_cast<uint32_t>(roundToInt(asFloat(s1))), d); return;
    case Op::SPTRUNC: write(w, dst, static_cast<uint32_t>(truncToInt(asFloat(s1))), d); return;
    case Op::SPDP: writePairSplit(w, dst, fromDouble(static_cast<double>(asFloat(s1))), d); return;
    case Op::RCPSP: write(w, dst, fromFloat(1.0f / asFloat(s1)), d); return;

    // ---- double precision -------------------------------------------------
    case Op::ADDDP: writePairSplit(w, dst, fromDouble(asDouble(p1) + asDouble(p2)), d); return;
    case Op::SUBDP: writePairSplit(w, dst, fromDouble(asDouble(p1) - asDouble(p2)), d); return;
    case Op::MPYDP: writePairSplit(w, dst, fromDouble(asDouble(p1) * asDouble(p2)), d); return;
    case Op::CMPEQDP: write(w, dst, asDouble(p1) == asDouble(p2), d); return;
    case Op::CMPLTDP: write(w, dst, asDouble(p1) < asDouble(p2), d); return;
    case Op::CMPGTDP: write(w, dst, asDouble(p1) > asDouble(p2), d); return;
    case Op::ABSDP: writePairSplit(w, dst, p1 & 0x7fffffffffffffffULL, d); return;
    case Op::INTDP:  writePairSplit(w, dst, fromDouble(static_cast<double>(static_cast<int32_t>(s1))), d); return;
    case Op::INTDPU: writePairSplit(w, dst, fromDouble(static_cast<double>(s1)), d); return;
    case Op::DPINT:   write(w, dst, static_cast<uint32_t>(roundToInt(asDouble(p1))), d); return;
    case Op::DPTRUNC: write(w, dst, static_cast<uint32_t>(truncToInt(asDouble(p1))), d); return;
    case Op::DPSP: write(w, dst, fromFloat(static_cast<float>(asDouble(p1))), d); return;
    case Op::RCPDP: writePairSplit(w, dst, fromDouble(1.0 / asDouble(p1)), d); return;
    case Op::Count: break;
    }
    fault("'" + in.mnem + "' is not implemented");
}

void Cpu::executePacket() {
    std::map<uint32_t, Instr>::const_iterator it = prog_.code.find(pc_);
    if (it == prog_.code.end()) fault("no instruction here");
    // Gather the packet: this instruction and those marked || after it.
    std::vector<const Instr *> packet;
    packet.push_back(&it->second);
    uint32_t next = pc_ + 4;
    for (;;) {
        std::map<uint32_t, Instr>::const_iterator n = prog_.code.find(next);
        if (n == prog_.code.end() || !n->second.parallel) break;
        packet.push_back(&n->second);
        next += 4;
    }
    std::vector<Pending> writes;
    int nops = 1;
    packetEnd_ = next;
    const Instr *sploop = nullptr;
    for (const Instr *in : packet) {
        // Its predicate is SPLOOPW's continue condition or SPLOOP(D)'s reload condition, never a reason not to start.
        if (in->mnem.compare(0, 6, "SPLOOP") == 0) { sploop = in; continue; }
        if (trace_) {
            std::fprintf(stderr, "%8llu %s: %s", static_cast<unsigned long long>(cycle_),
                         where(pc_).c_str(), in->mnem.c_str());
            std::fprintf(stderr, "\n");
        }
        // The NOPs folded into BNOP, RETNOP and ADDKPC are unconditional: a
        // predicate that is off skips the branch, not the cycles.
        if (in->mnem == "NOP" && !in->ops.empty()) nops = static_cast<int>(in->ops[0].imm);
        else if (in->mnem == "BNOP" || in->mnem == "RETNOP" || in->mnem == "ADDKPC")
            nops = static_cast<int>(in->ops.back().imm) + 1;
        else if (in->mnem == "CALLP") nops = 6;
        if (in->pred >= 0) {
            bool on = r_[in->pred] != 0;
            if (in->predNeg) on = !on;
            if (!on) continue;
        }
        bool branched = false;
        uint32_t target = 0;
        execute(*in, writes, branched, target);
        if (branched) {
            Branch b; b.at = cycle_ + 6; b.target = target;
            branches_.push_back(b);
        }
    }
    if (sploop != nullptr) loopStart(*sploop);
    for (const Pending &p : writes) pending_.push_back(p);
    pc_ = next;
    packets_++;
    cycle_++;
    applyPending();
    completeDeferred();
    for (int k = 1; k < nops; k++) tick();
}

// ---- the software pipelined loop buffer ------------------------------------------------------------------

static bool isLoopControl(const std::string &m) {
    return m.compare(0, 6, "SPLOOP") == 0 || m.compare(0, 8, "SPKERNEL") == 0 || m.compare(0, 6, "SPMASK") == 0;
}
static bool isBranch(Op op) {
    return op == Op::B || op == Op::BNOP || op == Op::CALLP || op == Op::CALL || op == Op::RET || op == Op::RETNOP;
}

// The SPLOOP(D/W) packet: the buffer clears and the next cycle begins loading. An SPLOOP decrements ILC now, unless
// it is 0 - then the loop runs no iteration (SPRUFE8 7.9.1); SPLOOPD and SPLOOPW always run one.
void Cpu::loopStart(const Instr &in) {
    Loop &L = lp_;
    L = Loop();
    L.active = true;
    L.kind = in.mnem == "SPLOOP" ? 0 : in.mnem == "SPLOOPD" ? 1 : 2;
    L.ii = static_cast<int>(in.ops[0].imm);
    L.loading = true;
    L.start = cycle_;
    L.predReg = in.pred; L.predNeg = in.predNeg;
    if (L.kind == 2 && L.predReg < 0) fault("SPLOOPW must be predicated: its condition ends the loop");
    if (L.predReg >= 0) L.cond[cycle_] = (r_[L.predReg] != 0) != L.predNeg;
    if (L.kind == 0) { if (r_[ILC] == 0) L.initialTerm = true; else r_[ILC]--; }
}

// The buffer goes idle: program memory fetch is on, and a NOP the current packet still owes runs out.
void Cpu::loopIdle() {
    int left = lp_.progLeft;
    lp_ = Loop();
    for (int k = 0; k < left; k++) tick();
}

// A stage boundary of the running invocation, at the end of this cycle. True when the buffer goes idle now.
bool Cpu::loopBoundary(int &lbc, bool) {
    Loop &L = lp_;
    lbc = 0;
    const uint64_t body = cycle_ - L.start;
    if (L.kind == 2) {
        // SPLOOPW: its condition as it stood three cycles before the boundary; false ends the loop with no epilog -
        // at once, or at the last loading stage boundary if it is still loading (SPRUFE8 7.10, Figure 7-9).
        if (body <= 3) return false;
        std::map<uint64_t, bool>::const_iterator c = L.cond.find(cycle_ - 3);
        if (c != L.cond.end() && !c->second) L.termPending = true;
        return L.termPending && L.kernelDone;
    }
    if (L.initialTerm) return L.kernelDone;          // no iteration: idle at the last loading stage boundary
    if (L.draining) return false;
    if (L.kind == 1 && body <= 3) return false;      // SPLOOPD: ILC is not looked at for three cycles
    if (r_[ILC] != 0) { r_[ILC]--; return false; }
    // ILC is 0: the epilog begins next cycle; with SPLOOP(D)'s condition true four cycles ago, a reload follows.
    L.draining = true; L.drain = 0;
    if (L.reloading) fault("a reloaded loop ended before its reload completed - not modelled");
    if (L.prevActive) fault("a reloaded loop ended while the one before it was still draining");
    if (L.predReg >= 0) {
        std::map<uint64_t, bool>::const_iterator c = L.cond.find(cycle_ - 4);
        L.reloadArmed = c != L.cond.end() && c->second;
        if (L.reloadArmed && L.spkernelR) L.reloadStart = true;
    }
    return false;
}

// One cycle with the buffer active: its valid instructions at the current LBC beside whatever program memory
// supplies - the loading first iteration, or post-loop code over the epilog once SPKERNEL's delay has passed.
void Cpu::loopCycle() {
    Loop &L = lp_;
    // The running invocation finished draining: the buffer is idle - unless a reload is armed, when it stays active,
    // empty, until the SPMASKR in the outer loop's code starts the next invocation (SPRUFE8 7.7.3.2).
    if (L.draining && L.dynlen >= 0 && L.drain >= L.dynlen - L.ii) {
        if (!L.reloadArmed) { loopIdle(); return; }
        L.draining = false; L.drain = 0;
        for (LoopEntry &e : L.buf) e.valid = false;
    }
    if (L.prevActive && L.pdrain >= L.dynlen - L.ii) L.prevActive = false;
    if (L.predReg >= 0) {
        L.cond[cycle_] = (r_[L.predReg] != 0) != L.predNeg;
        while (!L.cond.empty() && L.cond.begin()->first + 8 < cycle_) L.cond.erase(L.cond.begin());
    }
    if (L.draining && L.kernelDone && !L.fetch && L.drain >= L.fetchDelay) L.fetch = true;

    // Program memory's packet for this cycle, while it is fetching; a NOP n holds it n cycles.
    std::vector<const Instr *> prog;
    int nops = 1;
    if (L.fetch) {
        if (L.progLeft > 0) L.progLeft--;
        else {
            std::map<uint32_t, Instr>::const_iterator it = prog_.code.find(pc_);
            if (it == prog_.code.end()) fault("no instruction here");
            prog.push_back(&it->second);
            uint32_t next = pc_ + 4;
            for (;;) {
                std::map<uint32_t, Instr>::const_iterator n = prog_.code.find(next);
                if (n == prog_.code.end() || !n->second.parallel) break;
                prog.push_back(&n->second);
                next += 4;
            }
            for (const Instr *in : prog) {
                if (in->mnem == "NOP" && !in->ops.empty()) nops = static_cast<int>(in->ops[0].imm);
                else if (in->mnem == "BNOP" || in->mnem == "RETNOP" || in->mnem == "ADDKPC")
                    nops = static_cast<int>(in->ops.back().imm) + 1;
                else if (in->mnem == "CALLP") nops = 6;
            }
            pc_ = next;
            packetEnd_ = next;
            L.progLeft = nops - 1;
        }
    }
    // SPMASK masks the units of its packet's ^ instructions and those it names: their instructions from the
    // buffer do not execute, and the program's are executed but not loaded.
    std::vector<std::string> masked;
    bool kernel = false, kernelR = false, spmaskr = false;
    long long fstg = 0, fcyc = 0;
    for (const Instr *in : prog) {
        if (in->mnem == "SPMASK" || in->mnem == "SPMASKR") {
            masked.insert(masked.end(), in->maskUnits.begin(), in->maskUnits.end());
            if (in->mnem == "SPMASKR") spmaskr = true;
        }
        if (in->mnem == "SPKERNEL" || in->mnem == "SPKERNELR") {
            kernel = true; kernelR = in->mnem == "SPKERNELR";
            if (in->ops.size() == 2) { fstg = in->ops[0].imm; fcyc = in->ops[1].imm; }
        }
        if (in->caret && !in->unit.empty()) masked.push_back(in->unit);
    }
    auto isMasked = [&](const Instr *in) {
        return !in->unit.empty() && std::find(masked.begin(), masked.end(), in->unit) != masked.end();
    };

    // The buffer's side of the cycle: drain, reload, then fetch what is valid.
    std::vector<const Instr *> run;
    if (L.prevActive) {
        for (LoopEntry &e : L.buf) if (e.slot == L.plbc && e.load == L.pdrain) e.next = false;
        for (LoopEntry &e : L.buf) if (e.slot == L.plbc && e.next && !isMasked(e.in)) run.push_back(e.in);
    }
    if (L.draining) for (LoopEntry &e : L.buf) if (e.slot == L.lbc && e.load == L.drain) e.valid = false;
    if (L.reloading) for (LoopEntry &e : L.buf) if (e.slot == L.lbc && e.load == L.rload) { e.valid = true; e.from = 0; }
    for (LoopEntry &e : L.buf)
        if (e.slot == L.lbc && e.valid && e.from <= cycle_ && !isMasked(e.in)) run.push_back(e.in);

    // The program's side: everything executes; while loading, what is not masked (nor a branch) is also stored.
    for (const Instr *in : prog) {
        if (isLoopControl(in->mnem)) continue;
        const IsaEntry *ie = isaLookup(in->mnem);
        bool store = L.loading && !in->caret && !isMasked(in) && !isBranch(ie->op);
        if (store) {
            LoopEntry e; e.in = in; e.slot = L.lbc; e.load = L.load; e.valid = !L.initialTerm; e.next = false; e.from = cycle_ + 1;
            L.buf.push_back(e);
            if (L.initialTerm) continue;              // a loop of no iterations: its body executes as NOPs
        }
        run.push_back(in);
    }

    std::vector<Pending> writes;
    for (const Instr *in : run) {
        if (trace_) std::fprintf(stderr, "%8llu %s: %s%s\n", static_cast<unsigned long long>(cycle_), where(pc_).c_str(),
                                 in->mnem.c_str(), std::find(prog.begin(), prog.end(), in) == prog.end() ? " (buffer)" : "");
        if (in->pred >= 0) {
            bool on = r_[in->pred] != 0;
            if (in->predNeg) on = !on;
            if (!on) continue;
        }
        bool branched = false;
        uint32_t target = 0;
        execute(*in, writes, branched, target);
        if (branched) { Branch b; b.at = cycle_ + 6; b.target = target; branches_.push_back(b); }
    }

    // SPKERNEL: the iteration is loaded; program fetch waits for the epilog's delay, fstg stages and fcyc cycles.
    if (kernel && L.loading) {
        L.loading = false; L.kernelDone = true;
        L.dynlen = L.load + 1;
        L.spkernelR = kernelR;
        L.fetchDelay = kernelR ? 0 : static_cast<int>(fstg * L.ii + fcyc);
        L.fetch = false; L.progLeft = 0;
    }
    if (spmaskr && L.reloadArmed) L.reloadStart = true;

    // The end of the cycle: counters, then the stage boundaries.
    if (L.loading) L.load++;
    if (L.prevActive) { L.pdrain++; if (++L.plbc == L.ii) L.plbc = 0; }
    if (L.draining) L.drain++;
    if (L.reloading && ++L.rload >= L.dynlen) {
        L.reloading = false;                          // every instruction is back; program fetch stops
        L.fetch = false; L.progLeft = 0;
    }
    bool idle = false;
    if (++L.lbc == L.ii) idle = loopBoundary(L.lbc, false);
    // A reload begins on the next cycle: the draining invocation moves to the previous slot, ILC takes RILC - 1.
    if (L.reloadStart) {
        L.reloadStart = false; L.reloadArmed = false;
        if (r_[RILC] == 0) fault("a reload with RILC 0 - a skipped invocation - is not modelled");
        for (LoopEntry &e : L.buf) { e.next = e.valid; e.valid = false; }
        L.prevActive = L.draining; L.plbc = L.lbc; L.pdrain = L.drain;
        L.lbc = 0; L.draining = false; L.drain = 0;
        L.reloading = true; L.rload = 0;
        r_[ILC] = r_[RILC] - 1;
    }

    for (const Pending &p : writes) pending_.push_back(p);
    packets_++;
    cycle_++;
    applyPending();
    completeDeferred();
    if (idle) loopIdle();
}

// One step: a packet, or a native stub below the text base. 0xFC is where
// main returns to and 0xF8 where a callback's callee does; a native name is
// answered by the runtime and returns through B3.
void Cpu::step() {
    if (!profiling_) { stepOne(); return; }
    const uint64_t c0 = cycle_, p0 = packets_;
    const uint32_t pc0 = pc_;
    lastNative_.clear();
    stepOne();
    if (!lastNative_.empty()) {
        ProfileRow &r = nativeRows_[lastNative_];
        r.name = lastNative_; r.native = true;
        r.cycles += cycle_ - c0; r.entries++;
        return;
    }
    const size_t f = functionAt(pc0);
    if (f >= fnRows_.size()) return;
    ProfileRow &r = fnRows_[f];
    r.cycles += cycle_ - c0;
    r.packets += packets_ - p0;
    if (pc0 == fnStart_[f]) r.entries++;
}

// The function holding pc: the last one tried first, since packets run in runs.
size_t Cpu::functionAt(uint32_t pc) {
    if (lastFn_ < fnStart_.size() && pc >= fnStart_[lastFn_] &&
        (lastFn_ + 1 == fnStart_.size() || pc < fnStart_[lastFn_ + 1]))
        return lastFn_;
    std::vector<uint32_t>::const_iterator i = std::upper_bound(fnStart_.begin(), fnStart_.end(), pc);
    if (i == fnStart_.begin()) return fnStart_.size();
    lastFn_ = static_cast<size_t>(i - fnStart_.begin()) - 1;
    return lastFn_;
}

// A name on code is a function unless it is a label: cpp11's L.<fn>.<kind><n> and L.return.<fn>,
// TI's $C$L<n>, and .L temporaries.
void Cpu::startProfile() {
    fnStart_.clear(); fnRows_.clear(); nativeRows_.clear(); lastFn_ = 0;
    for (std::map<uint32_t, std::string>::const_iterator n = prog_.names.begin(); n != prog_.names.end(); ++n) {
        const std::string &s = n->second;
        if (!prog_.code.count(n->first)) continue;
        if (s.compare(0, 2, "L.") == 0 || s.compare(0, 2, "L$") == 0 || s[0] == '$' || s[0] == '.') continue;
        fnStart_.push_back(n->first);
        ProfileRow r; r.name = s;
        fnRows_.push_back(r);
    }
    profiling_ = true;
}

std::vector<Cpu::ProfileRow> Cpu::profile() const {
    std::vector<ProfileRow> rows;
    for (const ProfileRow &r : fnRows_) if (r.cycles != 0) rows.push_back(r);
    for (std::map<std::string, ProfileRow>::const_iterator n = nativeRows_.begin(); n != nativeRows_.end(); ++n)
        rows.push_back(n->second);
    std::stable_sort(rows.begin(), rows.end(), [](const ProfileRow &a, const ProfileRow &b) { return a.cycles > b.cycles; });
    return rows;
}

void Cpu::stepOne() {
    if (lp_.active) { loopCycle(); return; }
    if (pc_ >= prog_.textBase) { executePacket(); return; }
    if (pc_ == 0xFC) { exitWith(static_cast<int>(r_[A4])); return; }
    std::string name;
    for (std::map<std::string, uint32_t>::const_iterator n = prog_.natives.begin(); n != prog_.natives.end(); ++n)
        if (n->second == pc_) name = n->first;
    if (name.empty()) fault("a branch below the text base");
    // Everything pending lands before the library runs, as it would have by
    // the time a real callee's first instruction read anything.
    completeDeferred();
    for (const Pending &p : pending_) r_[p.reg] = p.value;
    pending_.clear();
    branches_.clear();
    nativeCalls_++;
    if (profiling_) lastNative_ = name;
    if (!rt_.call(name, *this)) fault("'" + name + "' is not provided by the runtime");
    if (!running_) return;
    pc_ = r_[B3];
    cycle_++;
}

uint32_t Cpu::callback(uint32_t fn, uint32_t a4, uint32_t b4) {
    uint32_t savedPc = pc_, savedB3 = r_[B3];
    r_[A4] = a4; r_[B4] = b4;
    r_[B3] = 0xF8;                       // where the callee's return lands
    pc_ = fn;
    while (running_ && pc_ != 0xF8) step();
    completeDeferred();
    for (const Pending &p : pending_) r_[p.reg] = p.value;
    pending_.clear();
    uint32_t result = r_[A4];
    pc_ = savedPc; r_[B3] = savedB3;
    return result;
}

int Cpu::run(uint32_t entry, bool trace) {
    trace_ = trace;
    pc_ = entry;
    while (running_) step();
    return exitCode_;
}
