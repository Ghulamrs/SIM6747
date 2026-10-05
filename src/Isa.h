#pragma once

// The instructions the emulator knows: their spelling, their operand shape, and their delay slots - the cycles between
// issue and the result being visible to a later packet, which is what a NOP count in the emitted code has to cover.
// Every entry's delay is from the C674x CPU reference; where a count was doubted in TMS6747.md, this table is what the emulator holds the code to.

#include "Program.h"

#include <string>

enum class Op {
    // moves and integer arithmetic (0 delay slots)
    MVK, MVKL, MVKH, MV, ZERO, ADD, ADDU, SUB, SUBU, NEG, NOT, AND, OR, XOR,
    SHL, SHR, SHRU, EXT, EXTU, SET, CLR, ABS,
    CMPEQ, CMPLT, CMPGT, CMPLTU, CMPGTU, ADDAW, ADDAH, ADDAB, ADDAD, SUBAW, ADDK, MVC, ANDN,
    // multiplies (3 delay slots on the C64x+/C674x)
    MPY32, MPY32U, MPY32SU, MPY32US, MPY, MPYU, MPYSU, MPYUS, MPYLH, MPYHL, MPYH, MPYHU, MPYLI,
    // memory (4 delay slots for a load)
    LDB, LDBU, LDH, LDHU, LDW, LDDW, LDNW, LDNDW, STB, STH, STW, STDW, STNW, STNDW,
    // control
    B, CALLP, NOP, SWE, IDLE,
    // interrupts off and back on around a loop that must not be interrupted - nothing interrupts here
    DINT, RINT,
    // a move through the .M unit: the value lands as a multiply's would
    MVD,
    // cl6x's spellings: a branch with its NOPs folded in, and the return address computed after the branch instead of by it
    BNOP, RETNOP, RET, CALL, ADDKPC,
    // the software pipelined loop buffer (SPRUFE8 chapter 7): cl6x's -O2 loops
    SPLOOP, SPLOOPD, SPLOOPW, SPKERNEL, SPKERNELR, SPMASK, SPMASKR,
    // single precision
    ADDSP, SUBSP, MPYSP, CMPEQSP, CMPLTSP, CMPGTSP, ABSSP, INTSP, INTSPU, SPINT, SPTRUNC, SPDP, RCPSP,
    // double precision
    ADDDP, SUBDP, MPYDP, CMPEQDP, CMPLTDP, CMPGTDP, ABSDP, INTDP, INTDPU, DPINT, DPTRUNC, DPSP, RCPDP,
    Count
};

struct IsaEntry {
    const char *mnem;
    Op op;
    int delaySlots;
};

bool isaKnown(const std::string &mnem);
const IsaEntry *isaLookup(const std::string &mnem);
// Operand shapes, checked at assembly so a bad line is reported by line.
bool isaCheck(const Instr &in, std::string &why);
