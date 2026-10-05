#pragma once

// What the assembler produces and the CPU runs: a flat byte memory holding the data sections,
// and the instructions - kept as parsed operands rather than encoded, since nothing here reads
// C6000 machine code - laid out at real addresses four bytes apart, so that a label's value, a function pointer and the PC are ordinary numbers.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Operand {
    enum Kind { Reg, Imm, Mem };
    Kind kind = Imm;
    int reg = -1;          // 0-15 A0-A15, 16-31 B0-B15
    int reg2 = -1;         // the high register of a pair, or -1
    long long imm = 0;     // an immediate, or a resolved symbol's value
    std::string symbol;    // an unresolved symbol, before pass two
    long long addend = 0;
    // A memory operand: *base, *+base(off), *+base[off], *base++(off) ...
    int base = -1;
    int offReg = -1;       // an offset register, or -1
    long long off = 0;     // a constant offset (in bytes once scaled)
    bool scaled = false;   // [n] rather than (n): scaled by the access size
    int mode = 0;          // 0 none, 1 pre-inc, 2 pre-dec, 3 post-inc, 4 post-dec
    bool negative = false; // *-base(off)
    bool dpByte = false;   // (sym): cl6x's byte displacement from DP, which an address add takes unscaled
};

struct Instr {
    std::string mnem;
    int pred = -1;         // predicate register, or -1
    bool predNeg = false;
    bool parallel = false; // || with the previous instruction: same packet
    bool caret = false;    // ^: masked by the packet's SPMASK - executed once, not loaded into the loop buffer
    std::string unit;      // the functional unit written, L1 S2 D1 M2 (sides and cross paths dropped), or empty
    std::vector<std::string> maskUnits;   // SPMASK(R)'s explicit unit list
    std::vector<Operand> ops;
    std::string file;
    int line = 0;
};

struct Program {
    std::vector<uint8_t> memory;             // the whole address space
    std::map<uint32_t, Instr> code;          // instructions by address
    std::map<std::string, uint32_t> symbols; // globals and whatever main needs
    std::map<uint32_t, std::string> names;   // address -> a name, for traces
    uint32_t textBase = 0, dataEnd = 0;
    // Symbols the assembler could not define: candidates for the runtime.
    std::map<std::string, uint32_t> natives; // name -> stub address
    std::vector<std::pair<uint32_t, uint32_t> > initArray;   // each unit's .init_array: base, bytes
    std::vector<std::pair<uint32_t, uint32_t> > exidxTables; // each unit's .c6xabi.exidx: base, bytes
};

// Little-endian access to the flat memory, bounds-checked by the caller.
inline uint32_t rd32(const std::vector<uint8_t> &m, uint32_t a) {
    return m[a] | (m[a + 1] << 8) | (m[a + 2] << 16) | (uint32_t(m[a + 3]) << 24);
}
inline void wr32(std::vector<uint8_t> &m, uint32_t a, uint32_t v) {
    m[a] = v & 0xff; m[a + 1] = (v >> 8) & 0xff; m[a + 2] = (v >> 16) & 0xff; m[a + 3] = v >> 24;
}
inline uint16_t rd16(const std::vector<uint8_t> &m, uint32_t a) {
    return static_cast<uint16_t>(m[a] | (m[a + 1] << 8));
}
inline void wr16(std::vector<uint8_t> &m, uint32_t a, uint16_t v) {
    m[a] = v & 0xff; m[a + 1] = v >> 8;
}
