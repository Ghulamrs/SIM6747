#include "Isa.h"
#include "Cpu.h"

#include <cstring>

static const IsaEntry kIsa[] = {
    { "MVK", Op::MVK, 0 }, { "MVKL", Op::MVKL, 0 }, { "MVKH", Op::MVKH, 0 }, { "MVKLH", Op::MVKH, 0 },
    { "MV", Op::MV, 0 }, { "ZERO", Op::ZERO, 0 },
    { "ADD", Op::ADD, 0 }, { "ADDU", Op::ADDU, 0 }, { "SUB", Op::SUB, 0 }, { "SUBU", Op::SUBU, 0 },
    { "NEG", Op::NEG, 0 }, { "NOT", Op::NOT, 0 }, { "AND", Op::AND, 0 }, { "OR", Op::OR, 0 }, { "XOR", Op::XOR, 0 },
    { "SHL", Op::SHL, 0 }, { "SHR", Op::SHR, 0 }, { "SHRU", Op::SHRU, 0 },
    { "EXT", Op::EXT, 0 }, { "EXTU", Op::EXTU, 0 }, { "SET", Op::SET, 0 }, { "CLR", Op::CLR, 0 }, { "ABS", Op::ABS, 0 },
    { "CMPEQ", Op::CMPEQ, 0 }, { "CMPLT", Op::CMPLT, 0 }, { "CMPGT", Op::CMPGT, 0 },
    { "CMPLTU", Op::CMPLTU, 0 }, { "CMPGTU", Op::CMPGTU, 0 },
    { "ADDAW", Op::ADDAW, 0 }, { "ADDAH", Op::ADDAH, 0 }, { "ADDAB", Op::ADDAB, 0 }, { "SUBAW", Op::SUBAW, 0 },
    { "ADDK", Op::ADDK, 0 }, { "MVC", Op::MVC, 0 }, { "ADDAD", Op::ADDAD, 0 }, { "ANDN", Op::ANDN, 0 },
    { "MPY32", Op::MPY32, 3 }, { "MPY32U", Op::MPY32U, 3 }, { "MPY32SU", Op::MPY32SU, 3 }, { "MPY32US", Op::MPY32US, 3 },
    { "MPY", Op::MPY, 1 }, { "MPYU", Op::MPYU, 1 }, { "MPYSU", Op::MPYSU, 1 }, { "MPYUS", Op::MPYUS, 1 },
    { "MPYLH", Op::MPYLH, 1 }, { "MPYHL", Op::MPYHL, 1 }, { "MPYH", Op::MPYH, 1 }, { "MPYHU", Op::MPYHU, 1 },
    { "MPYLI", Op::MPYLI, 3 },
    { "LDB", Op::LDB, 4 }, { "LDBU", Op::LDBU, 4 }, { "LDH", Op::LDH, 4 }, { "LDHU", Op::LDHU, 4 },
    { "LDW", Op::LDW, 4 }, { "LDDW", Op::LDDW, 4 }, { "LDNW", Op::LDNW, 4 }, { "LDNDW", Op::LDNDW, 4 },
    { "STB", Op::STB, 0 }, { "STH", Op::STH, 0 }, { "STW", Op::STW, 0 }, { "STDW", Op::STDW, 0 },
    { "STNW", Op::STNW, 0 }, { "STNDW", Op::STNDW, 0 },
    { "B", Op::B, 5 }, { "CALLP", Op::CALLP, 0 }, { "NOP", Op::NOP, 0 }, { "SWE", Op::SWE, 0 }, { "IDLE", Op::IDLE, 0 },
    { "DINT", Op::DINT, 0 }, { "RINT", Op::RINT, 0 }, { "MVD", Op::MVD, 3 },
    { "BNOP", Op::BNOP, 5 }, { "RETNOP", Op::RETNOP, 5 }, { "RET", Op::RET, 5 }, { "CALL", Op::CALL, 5 }, { "CALLRET", Op::B, 5 }, { "ADDKPC", Op::ADDKPC, 0 },
    { "SPLOOP", Op::SPLOOP, 0 }, { "SPLOOPD", Op::SPLOOPD, 0 }, { "SPLOOPW", Op::SPLOOPW, 0 },
    { "SPKERNEL", Op::SPKERNEL, 0 }, { "SPKERNELR", Op::SPKERNELR, 0 }, { "SPMASK", Op::SPMASK, 0 }, { "SPMASKR", Op::SPMASKR, 0 },
    { "ADDSP", Op::ADDSP, 3 }, { "SUBSP", Op::SUBSP, 3 }, { "MPYSP", Op::MPYSP, 3 },
    { "CMPEQSP", Op::CMPEQSP, 1 }, { "CMPLTSP", Op::CMPLTSP, 1 }, { "CMPGTSP", Op::CMPGTSP, 1 },
    { "ABSSP", Op::ABSSP, 1 }, { "INTSP", Op::INTSP, 3 }, { "INTSPU", Op::INTSPU, 3 },
    { "SPINT", Op::SPINT, 3 }, { "SPTRUNC", Op::SPTRUNC, 3 }, { "SPDP", Op::SPDP, 1 }, { "RCPSP", Op::RCPSP, 1 },
    { "ADDDP", Op::ADDDP, 6 }, { "SUBDP", Op::SUBDP, 6 }, { "MPYDP", Op::MPYDP, 9 },
    { "CMPEQDP", Op::CMPEQDP, 1 }, { "CMPLTDP", Op::CMPLTDP, 1 }, { "CMPGTDP", Op::CMPGTDP, 1 },
    { "ABSDP", Op::ABSDP, 1 }, { "INTDP", Op::INTDP, 4 }, { "INTDPU", Op::INTDPU, 4 },
    { "DPINT", Op::DPINT, 3 }, { "DPTRUNC", Op::DPTRUNC, 3 }, { "DPSP", Op::DPSP, 3 }, { "RCPDP", Op::RCPDP, 1 },
};

const IsaEntry *isaLookup(const std::string &mnem) {
    for (const IsaEntry &e : kIsa) if (mnem == e.mnem) return &e;
    return nullptr;
}
bool isaKnown(const std::string &mnem) { return isaLookup(mnem) != nullptr; }

static bool isReg(const Operand &o) { return o.kind == Operand::Reg && o.reg2 < 0; }
static bool isPair(const Operand &o) { return o.kind == Operand::Reg && o.reg2 >= 0; }
static bool isImm(const Operand &o) { return o.kind == Operand::Imm; }
static bool isMem(const Operand &o) { return o.kind == Operand::Mem; }


static bool fits(long long v, long long lo, long long hi) { return v >= lo && v <= hi; }

// The constant fields, as asm6x enforces them (probed 2026-09-14 with one form per line): scst5
// first for the .L/.S forms, ucst5 second for .D's, ucst5 shift counts and fields, scst16 for
// MVK/ADDK, NOP 1-9, and a memory offset of ucst5 units of the access - ucst15 from B14 or B15.
static bool isaRanges(const Instr &in, std::string &why) {
    const IsaEntry *e = isaLookup(in.mnem);
    const std::vector<Operand> &o = in.ops;
    size_t n = o.size();
    bool ok = true;
    switch (e->op) {
    case Op::ADD: case Op::SUB: case Op::ADDU: case Op::SUBU:
        if (isImm(o[0])) ok = fits(o[0].imm, -16, 15);
        else if (isImm(o[1])) ok = fits(o[1].imm, 0, 31);
        break;
    case Op::AND: case Op::OR: case Op::XOR: case Op::CMPEQ: case Op::CMPLT: case Op::CMPGT:
        for (size_t i = 0; i < 2; i++) if (isImm(o[i])) ok = ok && fits(o[i].imm, -16, 15);
        break;
    case Op::CMPLTU: case Op::CMPGTU:
        for (size_t i = 0; i < 2; i++) if (isImm(o[i])) ok = ok && fits(o[i].imm, 0, 31);
        break;
    case Op::ADDAW: case Op::ADDAH: case Op::ADDAB:
        // ucst5, or the C64x+ form from DP or SP with a ucst15
        if (isImm(o[1])) ok = fits(o[1].imm, 0, isReg(o[0]) && (o[0].reg == Cpu::B + 14 || o[0].reg == Cpu::B15) ? 32767 : 31);
        if (isImm(o[1]) && o[1].dpByte) ok = fits(o[1].imm, 0, 32767 * (e->op == Op::ADDAW ? 4 : e->op == Op::ADDAH ? 2 : 1));
        break;
    case Op::SHL: case Op::SHR: case Op::SHRU: case Op::ADDAD: case Op::SUBAW:
        if (isImm(o[1])) ok = fits(o[1].imm, 0, 31);
        break;
    case Op::BNOP: case Op::RETNOP: ok = fits(o[1].imm, 0, 5); break;
    case Op::ADDKPC: ok = fits(o[2].imm, 0, 7); break;
    case Op::EXT: case Op::EXTU: case Op::SET: case Op::CLR:
        if (n == 4) ok = fits(o[1].imm, 0, 31) && fits(o[2].imm, 0, 31);
        break;
    case Op::MVK: case Op::ADDK: ok = fits(o[0].imm, -32768, 32767); break;
    case Op::MVKL: case Op::MVKH: ok = fits(o[0].imm, -2147483648LL, 4294967295LL); break;
    case Op::NOP: if (n == 1) ok = fits(o[0].imm, 1, 9); break;
    default: break;
    }
    if (!ok) { why = "a constant of '" + in.mnem + "' is out of range"; return false; }
    for (const Operand &m : o) {
        if (!isMem(m) || m.offReg >= 0) continue;
        long long width = e->op == Op::LDB || e->op == Op::LDBU || e->op == Op::STB ? 1
                        : e->op == Op::LDH || e->op == Op::LDHU || e->op == Op::STH ? 2
                        : e->op == Op::LDDW || e->op == Op::STDW || e->op == Op::LDNDW || e->op == Op::STNDW ? 8 : 4;
        long long off = m.off < 0 ? -m.off : m.off;
        long long units = m.base == Cpu::B + 14 || m.base == Cpu::B15 ? 32767 : 31;   // ucst15 from DP or SP
        if (off % width != 0 || off > units * width) {
            why = "the offset of '" + in.mnem + "' is not " + std::to_string(width) + "-byte units within " +
                  std::to_string(units) + " of the base";
            return false;
        }
    }
    return true;
}

bool isaCheck(const Instr &in, std::string &why) {
    const IsaEntry *e = isaLookup(in.mnem);
    if (e == nullptr) { why = "unknown instruction '" + in.mnem + "'"; return false; }
    const std::vector<Operand> &o = in.ops;
    size_t n = o.size();
    std::string m = in.mnem;
    bool ok = true;
    switch (e->op) {
    case Op::NOP: ok = n <= 1 && (n == 0 || isImm(o[0])); break;
    case Op::SWE: case Op::IDLE: case Op::DINT: case Op::RINT: ok = n == 0; break;
    case Op::MVD: ok = n == 2 && isReg(o[0]) && isReg(o[1]); break;
    case Op::B: case Op::CALLP: case Op::RET: case Op::CALL: ok = n >= 1 && (isImm(o[0]) || isReg(o[0])); break;
    case Op::BNOP: case Op::RETNOP: ok = n == 2 && (isImm(o[0]) || isReg(o[0])) && isImm(o[1]); break;
    case Op::ADDKPC: ok = n == 3 && isImm(o[0]) && isReg(o[1]) && isImm(o[2]); break;
    case Op::MVK: case Op::MVKL: case Op::MVKH: ok = n == 2 && isImm(o[0]) && isReg(o[1]); break;
    case Op::MV: case Op::NEG: case Op::NOT: case Op::ABS:
        ok = n == 2 && (isReg(o[0]) || isPair(o[0])) && (isReg(o[1]) || isPair(o[1])); break;
    case Op::ZERO: ok = n == 1 && (isReg(o[0]) || isPair(o[0])); break;
    case Op::MVC: ok = n == 2 && isReg(o[0]) && isReg(o[1]) &&
                       (o[0].reg >= Cpu::ILC || o[1].reg >= Cpu::ILC); break;   // to or from ILC or RILC
    case Op::SPLOOP: case Op::SPLOOPD: case Op::SPLOOPW: ok = n == 1 && isImm(o[0]) && fits(o[0].imm, 1, 14); break;
    case Op::SPKERNEL: ok = n == 0 || (n == 2 && isImm(o[0]) && isImm(o[1])); break;
    case Op::SPKERNELR: case Op::SPMASK: case Op::SPMASKR: ok = n == 0; break;
    case Op::ADDK: ok = n == 2 && isImm(o[0]) && isReg(o[1]); break;
    case Op::EXT: case Op::EXTU: case Op::SET: case Op::CLR:
        ok = (n == 4 && isReg(o[0]) && isImm(o[1]) && isImm(o[2]) && isReg(o[3])) ||
             (n == 3 && isReg(o[0]) && isReg(o[1]) && isReg(o[2]));
        break;
    case Op::LDB: case Op::LDBU: case Op::LDH: case Op::LDHU: case Op::LDW: case Op::LDNW:
        ok = n == 2 && isMem(o[0]) && isReg(o[1]); break;
    case Op::LDDW: case Op::LDNDW: ok = n == 2 && isMem(o[0]) && isPair(o[1]); break;
    case Op::STB: case Op::STH: case Op::STW: case Op::STNW:
        ok = n == 2 && isReg(o[0]) && isMem(o[1]); break;
    case Op::STDW: case Op::STNDW: ok = n == 2 && isPair(o[0]) && isMem(o[1]); break;
    case Op::MPY32U: case Op::MPY32SU: case Op::MPY32US: case Op::MPYLI:
        ok = n == 3 && isReg(o[0]) && isReg(o[1]) && isPair(o[2]); break;
    case Op::ADDU: case Op::SUBU:
        ok = n == 3 && (isReg(o[0]) || isImm(o[0]) || isPair(o[0])) && (isReg(o[1]) || isImm(o[1]) || isPair(o[1])) && isPair(o[2]); break;
    case Op::ADDSP: case Op::SUBSP: case Op::MPYSP: case Op::CMPEQSP: case Op::CMPLTSP: case Op::CMPGTSP:
        ok = n == 3 && isReg(o[0]) && isReg(o[1]) && isReg(o[2]); break;
    case Op::ADDDP: case Op::SUBDP: case Op::MPYDP:
        ok = n == 3 && isPair(o[0]) && isPair(o[1]) && isPair(o[2]); break;
    case Op::CMPEQDP: case Op::CMPLTDP: case Op::CMPGTDP:
        ok = n == 3 && isPair(o[0]) && isPair(o[1]) && isReg(o[2]); break;
    case Op::ABSSP: case Op::SPINT: case Op::SPTRUNC: case Op::INTSP: case Op::INTSPU: case Op::RCPSP:
        ok = n == 2 && isReg(o[0]) && isReg(o[1]); break;
    case Op::SPDP: case Op::INTDP: case Op::INTDPU: ok = n == 2 && isReg(o[0]) && isPair(o[1]); break;
    case Op::ABSDP: case Op::RCPDP: ok = n == 2 && isPair(o[0]) && isPair(o[1]); break;
    case Op::DPINT: case Op::DPTRUNC: case Op::DPSP: ok = n == 2 && isPair(o[0]) && isReg(o[1]); break;
    default:
        // Three-operand integer forms: two sources, either may be a constant
        // (both for SUB's two shapes), and a register destination. The 40-bit
        // forms with a pair destination are accepted for ADD and SUB.
        ok = n == 3 && (isReg(o[0]) || isImm(o[0])) && (isReg(o[1]) || isImm(o[1])) &&
             (isReg(o[2]) || isPair(o[2])) && !(isImm(o[0]) && isImm(o[1]));
        if (ok && (e->op == Op::SHL || e->op == Op::SHR || e->op == Op::SHRU) && isImm(o[0]) && !isImm(o[1]))
            ok = false;   // the value shifted is a register, the count may be a constant
        break;
    }
    if (!ok) { why = "'" + m + "' does not take these operands"; return false; }
    return isaRanges(in, why);
}
