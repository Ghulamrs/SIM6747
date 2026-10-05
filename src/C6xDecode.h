#pragma once

// C674x machine code read back into instructions: fetch packets (eight words at a 32-byte boundary), the
// compact-instruction header that may end one (SPRUFE8 3.10), and each 32- or 16-bit instruction matched
// against the opcode table and its operands decoded - registers, constants, branch targets, memory
// operands, control registers, unit masks. The same decoding serves the disassembler and the CPU.

#include "C6xIsa.h"

#include <cstdint>
#include <string>
#include <vector>

namespace c6x {

struct Operand {
    enum Kind : uint8_t { None, Reg, Pair, Const, Target, Mem, Ctrl, Units, Text, Fstg, Fcyc };
    Kind kind = None;
    int reg = -1;            // Reg: 0-31 A0-A31, 32-63 B0-B31; Pair: the even (low) register
    int64_t value = 0;       // Const; Target's address; Units' mask; Ctrl's index; Fstg/Fcyc's raw field
    // Mem: *base, with the mode's offset - a register or a constant - in units of `size` when scaled
    int base = -1, offsetReg = -1;
    uint32_t offset = 0;
    uint8_t mode = 0;        // MemMode
    bool scaled = true;
    uint8_t size = 0;        // the access: 1, 2, 4 or 8 bytes
    const char *text = nullptr;   // Text: IRP, NRP, ILC
};

struct Insn {
    uint32_t addr = 0;        // where it is
    uint32_t fetchPacket = 0; // its fetch packet, the base of PC-relative targets
    uint8_t size = 4;         // 4, or 2 for a compact instruction
    uint32_t word = 0;        // as matched: a compact one with the header's SAT, BR and DSZ above bit 15
    int opIndex = -1;         // into the opcode table, or -1: not an instruction
    bool header = false;      // this word is the fetch packet's compact header
    bool parallel = false;    // its p-bit: it executes in parallel with the next instruction
    bool prot = false;        // in a fetch packet whose header sets PROT
    int predReg = -1;         // condition register (A0-A2, B0-B2), or -1: unconditional
    bool predZero = false;    // [!reg]
    Unit unit = U_N;
    int side = 0;             // 1 or 2
    int dataSide = 0;         // a load or store's T1 or T2, else 0
    bool cross = false;
    uint8_t noperands = 0;
    Operand op[4];
    const Opcode *opc() const { return opIndex < 0 ? nullptr : &opcode(static_cast<unsigned>(opIndex)); }
};

// Memory as the decoder reads it: little-endian words, false where nothing is mapped.
struct CodeReader {
    virtual ~CodeReader() {}
    virtual bool read32(uint32_t addr, uint32_t &w) const = 0;
};

// The instructions of the fetch packet at fp (32-byte aligned), in address order; the header, if any, is
// last and marked. A word or half that matches no opcode has opIndex -1.
std::vector<Insn> decodeFetchPacket(const CodeReader &mem, uint32_t fp);

// The p-bit of the instruction just before the one at addr, which is what decides whether that one starts an
// execute packet; the previous fetch packet is read when addr starts one.
bool parallelWithPrevious(const CodeReader &mem, uint32_t addr);

// The text of one instruction, TI assembler style: "[!B0] ADD.L1X A3,B4,A5". The disassembler's parallel
// bars and address are the caller's.
std::string formatInsn(const CodeReader &mem, const Insn &in);

std::string regName(int r);

} // namespace c6x
