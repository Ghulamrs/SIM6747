#pragma once

// The C674x instruction set as data: the instruction formats of SPRUFE8's appendices C-H (fixed bits, mask,
// and where each field's bits sit) and, for every opcode, the format it uses, its fixed field values, its
// operands, and which field encodes which operand and how. C6xOpcodes.inc holds the entries; this file the shapes.

#include <cstdint>

namespace c6x {

// Instruction fields, by name: an operand or fixed value is "the src2 field of this format".
enum Field : uint8_t {
    F_baseR, F_cc, F_creg, F_cst, F_csta, F_cstb, F_dst, F_dstms, F_dw, F_fstgfcyc, F_h, F_ii, F_mask, F_mode,
    F_n, F_na, F_offsetR, F_op, F_p, F_ptr, F_r, F_s, F_sc, F_src, F_src1, F_src2, F_srcdst, F_srcms, F_sn,
    F_sz, F_unit, F_t, F_x, F_y, F_z, F_none
};

// How a field's bits stand for an operand.
enum Coding : uint8_t {
    C_ucst, C_scst, C_ucst_minus_one, C_scst_negate, C_ulcst_dpr_byte, C_ulcst_dpr_half, C_ulcst_dpr_word,
    C_lcst_low16, C_lcst_high16, C_pcrel, C_pcrel_half, C_pcrel_half_unsigned, C_reg, C_regpair_lsb,
    C_regpair_msb, C_areg, C_reg_ptr, C_crlo, C_crhi, C_reg_shift, C_mem_offset, C_mem_offset_noscale,
    C_mem_mode, C_scaled, C_fstg, C_fcyc, C_spmask, C_reg_unused, C_fu, C_data_fu, C_xpath, C_scst_l3i,
    C_cst_s3i, C_mem_offset_minus_one, C_mem_offset_minus_one_noscale, C_rside, C_none
};

// What an operand is, syntactically.
enum Form : uint8_t {
    O_asm_const, O_link_const, O_reg, O_reg_nors, O_reg_bside, O_reg_bside_nors, O_xreg, O_dreg, O_areg,
    O_b15reg, O_treg, O_zreg, O_retreg, O_regpair, O_xregpair, O_dregpair, O_tregpair, O_irp, O_nrp, O_ilc,
    O_ctrl, O_mem_short, O_mem_ndw, O_mem_long, O_mem_deref, O_func_unit, O_hw_m1, O_hw_0, O_hw_1, O_hw_5,
    O_hw_16, O_hw_24, O_hw_31, O_none
};

// The pipeline an instruction goes down: its timing class (SPRUFE8 chapter 4).
enum Pipe : uint8_t {
    P_nop, P_one, P_m1616, P_store, P_mul_ext, P_load, P_branch, P_dp2, P_four, P_intdp, P_dpcmp, P_addsubdp,
    P_mpyi, P_mpyid, P_mpydp, P_mpyspdp, P_mpysp2dp
};

enum Unit : uint8_t { U_D, U_L, U_M, U_S, U_N };
enum Rw : uint8_t { RW_none, RW_r, RW_w, RW_rw };

enum : uint32_t {
    K_MACRO = 0x1, K_MCNOP = 0x4, K_NO_MCNOP = 0x8, K_LOAD = 0x10, K_STORE = 0x20, K_UNALIGNED = 0x40,
    K_SIDE_B = 0x80, K_SIDE_T2 = 0x100, K_NO_CROSS = 0x200, K_CALL = 0x400, K_RETURN = 0x800,
    K_SPLOOP = 0x1000, K_SPKERNEL = 0x2000, K_SPMASK = 0x4000,
    K_SPRED = 0x100000, K_NORS = 0x200000, K_BSIDE = 0x400000, K_B15PTR = 0x800000
};
// A compact load or store's addressing mode, which its format implies rather than encodes.
#define KMODE(m) (uint32_t(m) << 16)
inline unsigned kmode(uint32_t flags) { return (flags >> 16) & 0xf; }

// Addressing modes, as the 32-bit mode field writes them.
enum MemMode : uint8_t {
    M_NEG = 0, M_POS = 1, M_REG_NEG = 4, M_REG_POS = 5, M_PREDEC = 8, M_PREINC = 9, M_POSTDEC = 10,
    M_POSTINC = 11, M_REG_PREDEC = 12, M_REG_PREINC = 13, M_REG_POSTDEC = 14, M_REG_POSTINC = 15
};

struct BitPiece { uint8_t lo, width, pos; };
struct FormatField { Field id; uint8_t pieces; BitPiece b[4]; };
struct Format {
    const char *name;
    uint8_t bits;          // 32, or 16 for a compact instruction
    uint32_t cst, mask;    // (word & mask) == cst; a compact word carries the header's SAT, BR and DSZ above bit 15
    uint8_t nfields;
    FormatField f[11];
};
struct Fixed { Field id; uint16_t min, max; };
struct OperandInfo { Form form; uint8_t size; Rw rw; uint8_t lowFirst, lowLast, highFirst, highLast; };
struct Encoding { Field id; Coding coding; uint8_t operand; };
// Every mnemonic the table has, in table order: what the CPU dispatches on.
enum Mnemonic : uint16_t {
#define C6X_MNEMONIC(n) MN_##n,
#define C6X_MNEMONICS
#include "C6xOpcodes.inc"
#undef C6X_MNEMONICS
#undef C6X_MNEMONIC
    MN_count
};

struct Opcode {
    Mnemonic mn;
    const char *name;
    Unit unit;
    uint8_t format;
    Pipe pipe;
    uint32_t flags;
    uint8_t nfixed; Fixed fixed[4];
    uint8_t noperands; OperandInfo op[4];
    uint8_t nenc; Encoding enc[7];
};

// Control registers: the crlo address, the crhi bits that must be clear, and whether MVC may read or write it.
struct ControlReg { const char *name; uint8_t crlo; uint8_t crhiMask; bool read, write; };

const Format &format(unsigned i);
unsigned formatCount();
const Opcode &opcode(unsigned i);
unsigned opcodeCount();
const ControlReg *controlRegs(unsigned &count);

} // namespace c6x
