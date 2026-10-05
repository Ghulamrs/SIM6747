// What each C674x instruction does (SPRUFE8 chapter 3, one entry per mnemonic), on the operands the decoder
// read: sources as the pipeline has them in their read cycles, results scheduled into the cycles their
// delay slots end. 40-bit longs live in a register pair, their eight high bits in the odd register's low byte.

#include "C6xCpu.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace c6x {

// ---- lanes and saturation ---------------------------------------------------------------------------------
static inline int32_t h0(uint32_t v) { return int16_t(v & 0xffff); }
static inline int32_t h1(uint32_t v) { return int16_t(v >> 16); }
static inline uint32_t u0(uint32_t v) { return v & 0xffff; }
static inline uint32_t u1(uint32_t v) { return v >> 16; }
static inline uint32_t ub(uint32_t v, int i) { return (v >> (8 * i)) & 0xff; }
static inline int32_t sb(uint32_t v, int i) { return int8_t((v >> (8 * i)) & 0xff); }
static inline uint32_t pack16(int32_t hi, int32_t lo) { return (uint32_t(hi) << 16) | (uint32_t(lo) & 0xffff); }
static inline int32_t sat16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : int32_t(v); }
static inline uint32_t satu16(int64_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : uint32_t(v); }
static inline uint32_t satu8(int64_t v) { return v > 255 ? 255 : v < 0 ? 0 : uint32_t(v); }
static inline int64_t sext40(uint64_t v) { return int64_t(v << 24) >> 24; }
static const int64_t kMax40 = (int64_t(1) << 39) - 1, kMin40 = -(int64_t(1) << 39);

// ---- C67x floating point (SPRUFE8 2.8.8, 4.3): denormal sources read as zero, denormal results flush to zero,
// a NaN result is the quiet NaN 7FFF FFFFh (7FFF FFFF FFFF FFFFh in double precision) ----------------------------
static inline float f32(uint32_t v) {
    if ((v & 0x7f800000u) == 0) v &= 0x80000000u;
    float f; std::memcpy(&f, &v, 4); return f;
}
static inline uint32_t bits32(float f) {
    uint32_t v; std::memcpy(&v, &f, 4);
    if ((v & 0x7f800000u) == 0x7f800000u && (v & 0x007fffffu)) return 0x7fffffffu;
    if ((v & 0x7f800000u) == 0) v &= 0x80000000u;
    return v;
}
static inline double f64(uint64_t v) {
    if ((v & 0x7ff0000000000000ull) == 0) v &= 0x8000000000000000ull;
    double d; std::memcpy(&d, &v, 8); return d;
}
static inline uint64_t bits64(double d) {
    uint64_t v; std::memcpy(&v, &d, 8);
    if ((v & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (v & 0x000fffffffffffffull)) return 0x7fffffffffffffffull;
    if ((v & 0x7ff0000000000000ull) == 0) v &= 0x8000000000000000ull;
    return v;
}
static int32_t toInt(double d, bool truncate) {
    if (d != d) return int32_t(0x80000000u);            // SPINT/DPINT of a NaN: 8000 0000h (SPRUFE8, SPINT)
    double r = truncate ? std::trunc(d) : std::nearbyint(d);
    if (r >= 2147483647.0) return 0x7fffffff;
    if (r <= -2147483648.0) return int32_t(0x80000000u);
    return int32_t(r);
}
// RCPSP/RCPDP and RSQRSP/RSQRDP give eight correct bits of the mantissa (SPRUFE8 3.2): here the exact value with
// its mantissa cut to eight bits, until the simulator's own table is measured.
static uint32_t approxSp(double v) {
    uint32_t b = bits32(float(v));
    if ((b & 0x7f800000u) != 0x7f800000u) b &= 0xffff8000u;
    return b;
}
static uint64_t approxDp(double v) {
    uint64_t b = bits64(v);
    if ((b & 0x7ff0000000000000ull) != 0x7ff0000000000000ull) b &= 0xffffe00000000000ull;
    return b;
}

// Carry-less multiply, reduced by the polynomial when one is given (GMPY4, GMPY, XORMPY).
static uint32_t gfMul8(uint32_t a, uint32_t b, uint32_t poly, unsigned size) {
    uint32_t r = 0;
    for (int i = 7; i >= 0; i--) {
        r <<= 1;
        if (r & (1u << (size + 1))) r ^= poly | (1u << (size + 1));
        if (b & (1u << i)) r ^= a;
    }
    return r & 0xff;
}
static uint64_t clmul(uint32_t a, uint32_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 32; i++) if (b & (1u << i)) r ^= uint64_t(a) << i;
    return r;
}

static uint32_t popcount8(uint32_t v) { uint32_t n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static uint32_t lmbd(uint32_t which, uint32_t v) {
    if (!(which & 1)) v = ~v;
    for (int i = 31; i >= 0; i--) if (v & (1u << i)) return 31u - uint32_t(i);
    return 32;
}
static uint32_t norm32(uint32_t v) {
    uint32_t s = v >> 31, n = 0;
    for (int i = 30; i >= 0 && ((v >> i) & 1) == s; i--) n++;
    return n;
}
static uint32_t norm40(int64_t v) {
    uint64_t u = uint64_t(v) & 0xffffffffffull;
    uint32_t s = uint32_t(u >> 39) & 1, n = 0;
    for (int i = 38; i >= 0 && ((u >> i) & 1) == s; i--) n++;
    return n;
}
static uint32_t deal(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 16; i++) { r |= ((v >> (2 * i)) & 1) << i; r |= ((v >> (2 * i + 1)) & 1) << (16 + i); }
    return r;
}
static uint32_t shfl(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 16; i++) { r |= ((v >> i) & 1) << (2 * i); r |= ((v >> (16 + i)) & 1) << (2 * i + 1); }
    return r;
}

// ---- results -----------------------------------------------------------------------------------------------
void Cpu6x::scheduleWrite(const Insn &in, unsigned k, uint64_t iss, uint64_t value) {
    const Operand &o = in.op[k];
    const OperandInfo &oi = in.opc()->op[k];
    uint64_t lo = iss + (oi.lowLast ? oi.lowLast : 1) - 1, hi = iss + (oi.highLast ? oi.highLast : oi.lowLast ? oi.lowLast : 1) - 1;
    switch (o.kind) {
    case Operand::Reg: writeReg(o.reg, uint32_t(value), lo, &in); break;
    case Operand::Pair:
        writeReg(o.reg, uint32_t(value), lo, &in);
        writeReg(o.reg + 1, oi.size == 5 ? uint32_t(value >> 32) & 0xff : uint32_t(value >> 32), hi, &in);
        break;
    default: fault("a result to an operand that is not a register", &in); break;
    }
}

// The address of a memory operand, with the base register's new value when the mode changes it. Circular
// addressing (AMR, SPRUFE8 2.8.3) applies to A4-A7 and B4-B7 as AMR sets them.
uint32_t Cpu6x::effectiveAddress(const Exec &x, const Operand &o, uint32_t &newBase, bool &updates) const {
    const uint32_t base = valueAt(o.base, x.issue);
    uint32_t off = o.offsetReg >= 0 ? valueAt(o.offsetReg, x.issue) : o.offset;
    if (o.scaled) off *= o.size;
    uint32_t block = 0;
    int r = o.base & 31;
    if (r >= 4 && r <= 7) {
        unsigned field = unsigned(r - 4) + (o.base >= 32 ? 4 : 0);
        unsigned mode = (cr_[CR_AMR] >> (2 * field)) & 3;
        if (mode == 1) block = 2u << ((cr_[CR_AMR] >> 16) & 31);
        else if (mode == 2) block = 2u << ((cr_[CR_AMR] >> 21) & 31);
    }
    auto add = [&](uint32_t b, uint32_t d, bool minus) -> uint32_t {
        uint32_t v = minus ? b - d : b + d;
        if (block == 0) return v;
        return (b & ~(block - 1)) | (v & (block - 1));
    };
    updates = false;
    switch (o.mode & ~4u) {
    case M_NEG: return add(base, off, true);
    case M_POS: return add(base, off, false);
    case M_PREDEC: newBase = add(base, off, true); updates = true; return newBase;
    case M_PREINC: newBase = add(base, off, false); updates = true; return newBase;
    case M_POSTDEC: newBase = add(base, off, true); updates = true; return base;
    default: newBase = add(base, off, false); updates = true; return base;
    }
}

// ---- one instruction ------------------------------------------------------------------------------------------
void Cpu6x::execute(const Exec &x) {
    const Insn &in = *x.in;
    const Opcode &op = *in.opc();
    const unsigned n = in.noperands, d = n ? n - 1 : 0;
    auto S = [&](unsigned k) -> uint32_t { return uint32_t(readOperand(x, k)); };
    auto L = [&](unsigned k) -> uint64_t { return readOperand(x, k); };
    // signed and unsigned values of an operand as wide as the instruction computes
    auto sv = [&](unsigned k) -> int64_t {
        const Operand &o = in.op[k];
        if (o.kind == Operand::Pair) return op.op[k].size == 5 ? sext40(L(k)) : int64_t(L(k));
        if (o.kind == Operand::Const) return o.value;
        return int32_t(S(k));
    };
    auto uv = [&](unsigned k) -> uint64_t {
        const Operand &o = in.op[k];
        if (o.kind == Operand::Pair) return op.op[k].size == 5 ? (L(k) & 0xffffffffffull) : L(k);
        if (o.kind == Operand::Const) return uint64_t(o.value) & 0xffffffffu;
        return S(k);
    };
    auto W = [&](uint64_t v) { scheduleWrite(in, d, x.issue, v); };
    auto W32 = [&](uint32_t v) { scheduleWrite(in, d, x.issue, v); };
    const bool longDst = n > 0 && op.op[d].size == 5;
    auto branch = [&](uint32_t target) { Branch b; b.at = x.issue + 5; b.target = target; branches_.push_back(b); };
    auto memop = [&](bool store, const Operand &m, unsigned dataOp, bool signExt) {
        uint32_t nb = 0; bool upd = false;
        uint32_t a = effectiveAddress(x, m, nb, upd);
        const bool aligned = !(op.flags & K_UNALIGNED);
        if (aligned) a &= ~uint32_t(m.size - 1);
        if (!mem_.mapped(a) || !mem_.mapped(a + m.size - 1)) {
            char b[96];
            std::snprintf(b, sizeof b, "a %u-byte %s at 0x%08x, where the C6747 has no memory", m.size, store ? "store" : "load", a);
            fault(b, &in);
            return;
        }
        if (upd) writeReg(m.base, nb, x.issue, &in);
        MemOp mo; mo.at = x.issue + 2; mo.store = store; mo.addr = a; mo.size = m.size; mo.value = 0; mo.by = &in;
        mo.signExt = signExt;
        const Operand &dop = in.op[dataOp];
        mo.reg = dop.reg; mo.pair = dop.kind == Operand::Pair;
        // A store's data is read in E1, with its address (SPRUFE8 4.2.3): only the write to memory waits for E3.
        if (store) mo.value = mo.pair ? (uint64_t(valueAt(mo.reg + 1, x.issue)) << 32 | valueAt(mo.reg, x.issue)) : valueAt(mo.reg, x.issue);
        const OperandInfo &doi = op.op[dataOp];
        mo.landAt = x.issue + (doi.lowLast ? doi.lowLast : 5) - 1;
        memops_.push_back(mo);
    };

    switch (op.mn) {
    // ---- moves and constants
    case MN_MV: case MN_MVD: W32(S(0)); break;
    case MN_MVK: W32(uint32_t(int32_t(in.op[0].value))); break;
    case MN_MVKH: W32((uint32_t(in.op[0].value) & 0xffff0000u) | (valueAt(in.op[d].reg, x.issue) & 0xffffu)); break;
    case MN_ADDK: W32(S(1) + uint32_t(int32_t(in.op[0].value))); break;
    case MN_ADDKPC: scheduleWrite(in, 1, x.issue, uint32_t(in.op[0].value)); break;
    case MN_DMV: W(uint64_t(S(0)) << 32 | S(1)); break;

    // ---- integer arithmetic, 32 or 40 bits as the destination is
    case MN_ADD:
        if (longDst) W(uint64_t(sv(0) + sv(1)) & 0xffffffffffull); else W32(S(0) + S(1));
        break;
    case MN_ADDU: W((uv(0) + uv(1)) & 0xffffffffffull); break;
    case MN_SUB:
        if (longDst) W(uint64_t(sv(0) - sv(1)) & 0xffffffffffull); else W32(S(0) - S(1));
        break;
    case MN_SUBU: W((uv(0) - uv(1)) & 0xffffffffffull); break;
    case MN_SADD: case MN_SSUB: {
        int64_t r = op.mn == MN_SADD ? sv(0) + sv(1) : sv(0) - sv(1);
        if (longDst) { if (r > kMax40) { r = kMax40; setSat(); } else if (r < kMin40) { r = kMin40; setSat(); } W(uint64_t(r) & 0xffffffffffull); }
        else { if (r > 0x7fffffff) { r = 0x7fffffff; setSat(); } else if (r < -0x7fffffffll - 1) { r = -0x7fffffffll - 1; setSat(); } W32(uint32_t(r)); }
        break;
    }
    case MN_ABS:
        if (longDst) { int64_t v = sext40(L(0)); v = v < 0 ? (v == kMin40 ? kMax40 : -v) : v; W(uint64_t(v) & 0xffffffffffull); }
        else { int32_t v = int32_t(S(0)); W32(v == int32_t(0x80000000u) ? 0x7fffffffu : uint32_t(v < 0 ? -v : v)); }
        break;
    case MN_SAT: { int64_t v = sext40(L(0)); if (v > 0x7fffffff) { v = 0x7fffffff; setSat(); } else if (v < -0x7fffffffll - 1) { v = -0x7fffffffll - 1; setSat(); } W32(uint32_t(v)); break; }
    case MN_NORM: W32(op.op[0].size == 5 ? norm40(sext40(L(0))) : norm32(S(0))); break;
    case MN_LMBD: W32(lmbd(uint32_t(sv(0)), S(1))); break;
    case MN_SUBC: {
        uint32_t a = S(0), b = S(1);
        W32(a >= b ? ((a - b) << 1) + 1 : a << 1);
        break;
    }

    // ---- compares
    case MN_CMPEQ: W32(sv(0) == sv(1)); break;
    case MN_CMPGT: W32(sv(0) > sv(1)); break;
    case MN_CMPLT: W32(sv(0) < sv(1)); break;
    case MN_CMPGTU: W32(uv(0) > uv(1)); break;
    case MN_CMPLTU: W32(uv(0) < uv(1)); break;
    case MN_CMPEQ2: { uint32_t a = S(0), b = S(1); W32((u0(a) == u0(b)) | (u1(a) == u1(b)) << 1); break; }
    case MN_CMPGT2: { uint32_t a = S(0), b = S(1); W32((h0(a) > h0(b)) | (h1(a) > h1(b)) << 1); break; }
    case MN_CMPEQ4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= uint32_t(ub(a, i) == ub(b, i)) << i; W32(r); break; }
    case MN_CMPGTU4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= uint32_t(ub(a, i) > ub(b, i)) << i; W32(r); break; }

    // ---- logic, shifts, fields
    case MN_AND: W32(S(0) & S(1)); break;
    case MN_ANDN: W32(S(0) & ~S(1)); break;
    case MN_OR: W32(S(0) | S(1)); break;
    case MN_XOR: W32(S(0) ^ S(1)); break;
    case MN_SHL: {
        unsigned amt = in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : (S(1) & 0x3f);
        uint64_t v = op.op[0].size == 5 ? (L(0) & 0xffffffffffull) : S(0);
        if (longDst) W(amt > 39 ? 0 : (v << amt) & 0xffffffffffull); else W32(amt > 31 ? 0 : uint32_t(v << amt));
        break;
    }
    case MN_SHR: {
        unsigned amt = in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : (S(1) & 0x3f);
        if (longDst) { int64_t v = sext40(L(0)); W(uint64_t(v >> (amt > 39 ? 39 : amt)) & 0xffffffffffull); }
        else W32(uint32_t(int32_t(S(0)) >> (amt > 31 ? 31 : amt)));
        break;
    }
    case MN_SHRU: {
        unsigned amt = in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : (S(1) & 0x3f);
        if (longDst) { uint64_t v = L(0) & 0xffffffffffull; W(amt > 39 ? 0 : v >> amt); }
        else W32(amt > 31 ? 0 : S(0) >> amt);
        break;
    }
    case MN_SSHL: {
        unsigned amt = in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : (S(1) & 0x1f);
        int64_t v = int64_t(int32_t(S(0))) << amt;
        if (v > 0x7fffffff) { v = 0x7fffffff; setSat(); } else if (v < -0x7fffffffll - 1) { v = -0x7fffffffll - 1; setSat(); }
        W32(uint32_t(v));
        break;
    }
    case MN_SSHVL: case MN_SSHVR: {
        int32_t amt = int32_t(S(1));
        if (amt > 31) amt = 31; else if (amt < -31) amt = -31;
        if (op.mn == MN_SSHVR) amt = -amt;
        int64_t v = int32_t(S(0));
        if (amt >= 0) { v <<= amt; if (v > 0x7fffffff) { v = 0x7fffffff; setSat(); } else if (v < -0x7fffffffll - 1) { v = -0x7fffffffll - 1; setSat(); } }
        else v >>= -amt;
        W32(uint32_t(v));
        break;
    }
    case MN_ROTL: { unsigned amt = (in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : S(1)) & 31; uint32_t v = S(0); W32(amt ? (v << amt) | (v >> (32 - amt)) : v); break; }
    case MN_SHR2: { unsigned amt = (in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : S(1)) & 31; if (amt > 15) amt = 15; uint32_t v = S(0); W32(pack16(h1(v) >> amt, h0(v) >> amt)); break; }
    case MN_SHRU2: { unsigned amt = (in.op[1].kind == Operand::Const ? unsigned(in.op[1].value) : S(1)) & 31; uint32_t v = S(0); W32(amt > 15 ? 0 : pack16(int32_t(u1(v) >> amt), int32_t(u0(v) >> amt))); break; }
    case MN_SHLMB: W32((S(1) << 8) | (S(0) >> 24)); break;
    case MN_SHRMB: W32((S(1) >> 8) | (S(0) << 24)); break;
    case MN_EXT: case MN_EXTU: case MN_SET: case MN_CLR: {
        uint32_t v = S(0), csta, cstb;
        if (n == 4) { csta = uint32_t(in.op[1].value) & 31; cstb = uint32_t(in.op[2].value) & 31; }
        else { uint32_t c = S(1); csta = (c >> 5) & 31; cstb = c & 31; }
        uint32_t r;
        if (op.mn == MN_EXT) r = uint32_t(int32_t(v << csta) >> cstb);
        else if (op.mn == MN_EXTU) r = (v << csta) >> cstb;
        else {
            uint32_t m = 0;
            for (uint32_t i = csta; i <= cstb && i < 32; i++) m |= 1u << i;
            r = op.mn == MN_SET ? (v | m) : (v & ~m);
        }
        W32(r);
        break;
    }

    // ---- SIMD
    case MN_ADD2: { uint32_t a = S(0), b = S(1); W32(pack16(int32_t(u1(a) + u1(b)), int32_t(u0(a) + u0(b)))); break; }
    case MN_SUB2: { uint32_t a = S(0), b = S(1); W32(pack16(int32_t(u1(a) - u1(b)), int32_t(u0(a) - u0(b)))); break; }
    case MN_ADD4: case MN_SUB4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= ((op.mn == MN_ADD4 ? ub(a, i) + ub(b, i) : ub(a, i) - ub(b, i)) & 0xff) << (8 * i); W32(r); break; }
    case MN_SADD2: { uint32_t a = S(0), b = S(1); W32(pack16(sat16(h1(a) + h1(b)), sat16(h0(a) + h0(b)))); break; }
    case MN_SSUB2: { uint32_t a = S(0), b = S(1); W32(pack16(sat16(h1(a) - h1(b)), sat16(h0(a) - h0(b)))); break; }
    case MN_SADDU4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= satu8(ub(a, i) + ub(b, i)) << (8 * i); W32(r); break; }
    case MN_SADDUS2: { uint32_t a = S(0), b = S(1); W32(satu16(int64_t(u1(a)) + h1(b)) << 16 | satu16(int64_t(u0(a)) + h0(b))); break; }
    case MN_ADDSUB: { uint32_t a = S(0), b = S(1); W(uint64_t(a + b) << 32 | uint32_t(a - b)); break; }
    case MN_ADDSUB2: { uint32_t a = S(0), b = S(1); W(uint64_t(pack16(int32_t(u1(a) + u1(b)), int32_t(u0(a) + u0(b)))) << 32 | pack16(int32_t(u1(a) - u1(b)), int32_t(u0(a) - u0(b)))); break; }
    case MN_SADDSUB: {
        int64_t a = int32_t(S(0)), b = int32_t(S(1)), s = a + b, t = a - b;
        auto c32 = [&](int64_t v) -> uint32_t { if (v > 0x7fffffff) { setSat(); return 0x7fffffff; } if (v < -0x7fffffffll - 1) { setSat(); return 0x80000000u; } return uint32_t(v); };
        W(uint64_t(c32(s)) << 32 | c32(t));
        break;
    }
    case MN_SADDSUB2: { uint32_t a = S(0), b = S(1); W(uint64_t(pack16(sat16(h1(a) + h1(b)), sat16(h0(a) + h0(b)))) << 32 | pack16(sat16(h1(a) - h1(b)), sat16(h0(a) - h0(b)))); break; }
    case MN_AVG2: { uint32_t a = S(0), b = S(1); W32(pack16((h1(a) + h1(b) + 1) >> 1, (h0(a) + h0(b) + 1) >> 1)); break; }
    case MN_AVGU4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= ((ub(a, i) + ub(b, i) + 1) >> 1) << (8 * i); W32(r); break; }
    case MN_ABS2: { uint32_t a = S(0); auto ab = [](int32_t v) { return v == -32768 ? 32767 : (v < 0 ? -v : v); }; W32(pack16(ab(h1(a)), ab(h0(a)))); break; }
    case MN_MAX2: { uint32_t a = S(0), b = S(1); W32(pack16(std::max(h1(a), h1(b)), std::max(h0(a), h0(b)))); break; }
    case MN_MIN2: { uint32_t a = S(0), b = S(1); W32(pack16(std::min(h1(a), h1(b)), std::min(h0(a), h0(b)))); break; }
    case MN_MAXU4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= std::max(ub(a, i), ub(b, i)) << (8 * i); W32(r); break; }
    case MN_MINU4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= std::min(ub(a, i), ub(b, i)) << (8 * i); W32(r); break; }
    case MN_SUBABS4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r |= uint32_t(std::abs(int(ub(a, i)) - int(ub(b, i)))) << (8 * i); W32(r); break; }
    case MN_PACK2: W32(pack16(int32_t(u0(S(0))), int32_t(u0(S(1))))); break;
    case MN_PACKH2: W32(pack16(int32_t(u1(S(0))), int32_t(u1(S(1))))); break;
    case MN_PACKHL2: W32(pack16(int32_t(u1(S(0))), int32_t(u0(S(1))))); break;
    case MN_PACKLH2: W32(pack16(int32_t(u0(S(0))), int32_t(u1(S(1))))); break;
    case MN_PACKH4: { uint32_t a = S(0), b = S(1); W32(ub(a, 3) << 24 | ub(a, 1) << 16 | ub(b, 3) << 8 | ub(b, 1)); break; }
    case MN_PACKL4: { uint32_t a = S(0), b = S(1); W32(ub(a, 2) << 24 | ub(a, 0) << 16 | ub(b, 2) << 8 | ub(b, 0)); break; }
    case MN_DPACK2: { uint32_t a = S(0), b = S(1); W(uint64_t(pack16(int32_t(u1(a)), int32_t(u1(b)))) << 32 | pack16(int32_t(u0(a)), int32_t(u0(b)))); break; }
    case MN_DPACKX2: { uint32_t a = S(0), b = S(1); W(uint64_t(pack16(int32_t(u0(b)), int32_t(u1(a)))) << 32 | pack16(int32_t(u0(a)), int32_t(u1(b)))); break; }
    case MN_SPACK2: W32(pack16(sat16(int32_t(S(0))), sat16(int32_t(S(1))))); break;
    case MN_SPACKU4: { uint32_t a = S(0), b = S(1); W32(satu8(h1(a)) << 24 | satu8(h0(a)) << 16 | satu8(h1(b)) << 8 | satu8(h0(b))); break; }
    case MN_RPACK2: {
        auto r = [&](uint32_t v) -> uint32_t { int64_t s = int64_t(int32_t(v)) << 1; if (s > 0x7fffffff) s = 0x7fffffff; else if (s < -0x7fffffffll - 1) s = -0x7fffffffll - 1; return uint32_t(s) >> 16; };
        W32(r(S(0)) << 16 | r(S(1)));
        break;
    }
    case MN_SWAP4: { uint32_t a = S(0); W32(ub(a, 2) << 24 | ub(a, 3) << 16 | ub(a, 0) << 8 | ub(a, 1)); break; }
    case MN_UNPKHU4: { uint32_t a = S(0); W32(ub(a, 3) << 16 | ub(a, 2)); break; }
    case MN_UNPKLU4: { uint32_t a = S(0); W32(ub(a, 1) << 16 | ub(a, 0)); break; }
    case MN_XPND2: { uint32_t a = S(0); W32(((a & 2) ? 0xffff0000u : 0) | ((a & 1) ? 0xffffu : 0)); break; }
    case MN_XPND4: { uint32_t a = S(0), r = 0; for (int i = 0; i < 4; i++) if (a & (1u << i)) r |= 0xffu << (8 * i); W32(r); break; }
    case MN_BITC4: { uint32_t a = S(0), r = 0; for (int i = 0; i < 4; i++) r |= popcount8(ub(a, i)) << (8 * i); W32(r); break; }
    case MN_BITR: { uint32_t a = S(0), r = 0; for (int i = 0; i < 32; i++) if (a & (1u << i)) r |= 1u << (31 - i); W32(r); break; }
    case MN_DEAL: W32(deal(S(0))); break;
    case MN_SHFL: W32(shfl(S(0))); break;
    case MN_SHFL3: {
        // unverified: the three 16-bit values src1.hi, src1.lo, src2.lo interleaved into 48 bits
        uint32_t a = S(0), b = S(1);
        uint64_t r = 0;
        for (int i = 0; i < 16; i++) {
            r |= uint64_t((b >> i) & 1) << (3 * i);
            r |= uint64_t((a >> i) & 1) << (3 * i + 1);
            r |= uint64_t((a >> (16 + i)) & 1) << (3 * i + 2);
        }
        W(r);
        break;
    }

    // ---- multiplies, 16 x 16
    case MN_MPY: W32(uint32_t(int32_t(int16_t(sv(0))) * h0(S(1)))); break;
    case MN_MPYU: W32(u0(S(0)) * u0(S(1))); break;
    case MN_MPYSU: W32(uint32_t(int32_t(int16_t(sv(0))) * int32_t(u0(S(1))))); break;
    case MN_MPYUS: W32(uint32_t(int32_t(u0(S(0))) * h0(S(1)))); break;
    case MN_MPYH: W32(uint32_t(h1(S(0)) * h1(S(1)))); break;
    case MN_MPYHU: W32(u1(S(0)) * u1(S(1))); break;
    case MN_MPYHSU: W32(uint32_t(h1(S(0)) * int32_t(u1(S(1))))); break;
    case MN_MPYHUS: W32(uint32_t(int32_t(u1(S(0))) * h1(S(1)))); break;
    case MN_MPYHL: W32(uint32_t(h1(S(0)) * h0(S(1)))); break;
    case MN_MPYHLU: W32(u1(S(0)) * u0(S(1))); break;
    case MN_MPYHSLU: W32(uint32_t(h1(S(0)) * int32_t(u0(S(1))))); break;
    case MN_MPYHULS: W32(uint32_t(int32_t(u1(S(0))) * h0(S(1)))); break;
    case MN_MPYLH: W32(uint32_t(h0(S(0)) * h1(S(1)))); break;
    case MN_MPYLHU: W32(u0(S(0)) * u1(S(1))); break;
    case MN_MPYLSHU: W32(uint32_t(h0(S(0)) * int32_t(u1(S(1))))); break;
    case MN_MPYLUHS: W32(uint32_t(int32_t(u0(S(0))) * h1(S(1)))); break;
    case MN_SMPY: case MN_SMPYH: case MN_SMPYHL: case MN_SMPYLH: {
        uint32_t a = S(0), b = S(1);
        int32_t x1 = op.mn == MN_SMPY || op.mn == MN_SMPYLH ? h0(a) : h1(a);
        int32_t x2 = op.mn == MN_SMPY || op.mn == MN_SMPYHL ? h0(b) : h1(b);
        uint32_t r = uint32_t(x1 * x2) << 1;
        if (r == 0x80000000u) { r = 0x7fffffffu; setSat(); }
        W32(r);
        break;
    }
    case MN_MPY2: { uint32_t a = S(0), b = S(1); W(uint64_t(uint32_t(h1(a) * h1(b))) << 32 | uint32_t(h0(a) * h0(b))); break; }
    case MN_SMPY2: {
        uint32_t a = S(0), b = S(1);
        auto s = [&](int32_t p, int32_t q) -> uint32_t { uint32_t r = uint32_t(p * q) << 1; if (r == 0x80000000u) { r = 0x7fffffffu; setSat(); } return r; };
        W(uint64_t(s(h1(a), h1(b))) << 32 | s(h0(a), h0(b)));
        break;
    }
    case MN_MPYSU4: case MN_MPYU4: {
        uint32_t a = S(0), b = S(1), p[4];
        for (int i = 0; i < 4; i++) p[i] = uint32_t(op.mn == MN_MPYSU4 ? sb(a, i) * int32_t(ub(b, i)) : int32_t(ub(a, i) * ub(b, i))) & 0xffff;
        W(uint64_t(p[3] << 16 | p[2]) << 32 | (p[1] << 16 | p[0]));
        break;
    }
    case MN_MPYHI: W(uint64_t(int64_t(h1(S(0))) * int32_t(S(1)))); break;
    case MN_MPYLI: W(uint64_t(int64_t(h0(S(0))) * int32_t(S(1)))); break;
    case MN_MPYHIR: W32(uint32_t((int64_t(h1(S(0))) * int32_t(S(1)) + 0x4000) >> 15)); break;
    case MN_MPYLIR: W32(uint32_t((int64_t(h0(S(0))) * int32_t(S(1)) + 0x4000) >> 15)); break;
    case MN_MPY2IR: {
        uint32_t a = S(0); int64_t b = int32_t(S(1));
        auto r = [&](int32_t h) -> uint32_t { int64_t v = (h * b + 0x4000) >> 15; if (v > 0x7fffffff) { v = 0x7fffffff; setSat(); } return uint32_t(v); };
        W(uint64_t(r(h1(a))) << 32 | r(h0(a)));
        break;
    }
    case MN_DOTP2:
        if (op.op[d].size == 8) W(uint64_t(int64_t(h1(S(0))) * h1(S(1)) + int64_t(h0(S(0))) * h0(S(1))));
        else W32(uint32_t(h1(S(0)) * h1(S(1)) + h0(S(0)) * h0(S(1))));
        break;
    case MN_DOTPN2: W32(uint32_t(h1(S(0)) * h1(S(1)) - h0(S(0)) * h0(S(1)))); break;
    case MN_DOTPRSU2: W32(uint32_t((int64_t(h1(S(0))) * int32_t(u1(S(1))) + int64_t(h0(S(0))) * int32_t(u0(S(1))) + 0x8000) >> 16)); break;
    case MN_DOTPNRSU2: W32(uint32_t((int64_t(h1(S(0))) * int32_t(u1(S(1))) - int64_t(h0(S(0))) * int32_t(u0(S(1))) + 0x8000) >> 16)); break;
    case MN_DOTPU4: { uint32_t a = S(0), b = S(1), r = 0; for (int i = 0; i < 4; i++) r += ub(a, i) * ub(b, i); W32(r); break; }
    case MN_DOTPSU4: { uint32_t a = S(0), b = S(1); int32_t r = 0; for (int i = 0; i < 4; i++) r += sb(a, i) * int32_t(ub(b, i)); W32(uint32_t(r)); break; }
    case MN_DDOTP4: {
        // unverified against the simulator: two dot products of src1's 16-bit halves with src2's bytes
        uint32_t a = S(0), b = S(1);
        int32_t hi = h1(a) * sb(b, 3) + h0(a) * sb(b, 2), lo = h1(a) * sb(b, 1) + h0(a) * sb(b, 0);
        W(uint64_t(uint32_t(hi)) << 32 | uint32_t(lo));
        break;
    }
    case MN_DDOTPH2: case MN_DDOTPL2: case MN_DDOTPH2R: case MN_DDOTPL2R: {
        // unverified: src1 a pair of four 16-bit values, src2 two
        uint64_t a = L(0); uint32_t b = S(1);
        uint32_t ae = uint32_t(a), ao = uint32_t(a >> 32);
        int64_t hi, lo;
        if (op.mn == MN_DDOTPH2 || op.mn == MN_DDOTPH2R) { hi = int64_t(h1(ao)) * h1(b) + int64_t(h0(ao)) * h0(b); lo = int64_t(h0(ao)) * h1(b) + int64_t(h1(ae)) * h0(b); }
        else { hi = int64_t(h0(ao)) * h1(b) + int64_t(h1(ae)) * h0(b); lo = int64_t(h1(ae)) * h1(b) + int64_t(h0(ae)) * h0(b); }
        if (op.mn == MN_DDOTPH2R || op.mn == MN_DDOTPL2R) W32(pack16(sat16((hi + 0x8000) >> 16), sat16((lo + 0x8000) >> 16)));
        else W(uint64_t(uint32_t(hi)) << 32 | uint32_t(lo));
        break;
    }
    case MN_CMPY: case MN_CMPYR: case MN_CMPYR1: {
        uint32_t a = S(0), b = S(1);
        int64_t re = int64_t(h1(a)) * h1(b) - int64_t(h0(a)) * h0(b), im = int64_t(h1(a)) * h0(b) + int64_t(h0(a)) * h1(b);
        auto s32 = [&](int64_t v) -> uint32_t { if (v > 0x7fffffff) { setSat(); return 0x7fffffff; } if (v < -0x7fffffffll - 1) { setSat(); return 0x80000000u; } return uint32_t(v); };
        if (op.mn == MN_CMPY) W(uint64_t(s32(re)) << 32 | s32(im));
        else {
            int sh = op.mn == MN_CMPYR ? 16 : 15;
            int64_t rnd = int64_t(1) << (sh - 1);
            W32(pack16(sat16((re + rnd) >> sh), sat16((im + rnd) >> sh)));
        }
        break;
    }
    case MN_GMPY4: {
        uint32_t a = S(0), b = S(1), g = cr_[CR_GFPGFR], r = 0;
        for (int i = 0; i < 4; i++) r |= gfMul8(ub(a, i), ub(b, i), g & 0xff, (g >> 24) & 7) << (8 * i);
        W32(r);
        break;
    }
    case MN_GMPY: {
        uint32_t poly = in.side == 2 ? cr_[CR_GPLYB] : cr_[CR_GPLYA];
        uint64_t p = clmul(S(0), S(1));
        for (int i = 63; i >= 32; i--) if (p & (uint64_t(1) << i)) p ^= (uint64_t(poly) | (uint64_t(1) << 32)) << (i - 32);
        W32(uint32_t(p));
        break;
    }
    case MN_XORMPY: W32(uint32_t(clmul(S(0), S(1)))); break;

    // ---- multiplies, 32 x 32
    case MN_MPY32:
        if (op.op[d].size == 8) W(uint64_t(int64_t(int32_t(S(0))) * int32_t(S(1))));
        else W32(S(0) * S(1));
        break;
    case MN_MPY32U: W(uint64_t(S(0)) * S(1)); break;
    case MN_MPY32SU: W(uint64_t(int64_t(int32_t(S(0))) * int64_t(S(1)))); break;
    case MN_MPY32US: W(uint64_t(int64_t(S(0)) * int64_t(int32_t(S(1))))); break;
    case MN_SMPY32: {
        int64_t p = int64_t(int32_t(S(0))) * int32_t(S(1));
        if (p == (int64_t(1) << 62)) { setSat(); W32(0x7fffffffu); }
        else W32(uint32_t(uint64_t(p << 1) >> 32));
        break;
    }
    case MN_MPYI: W32(S(0) * S(1)); break;
    case MN_MPYID: W(uint64_t(int64_t(int32_t(S(0))) * int32_t(S(1)))); break;

    // ---- floating point
    case MN_ADDSP: W32(bits32(f32(S(0)) + f32(S(1)))); break;
    case MN_SUBSP: W32(bits32(f32(S(0)) - f32(S(1)))); break;
    case MN_MPYSP: W32(bits32(f32(S(0)) * f32(S(1)))); break;
    case MN_ADDDP: W(bits64(f64(L(0)) + f64(L(1)))); break;
    case MN_SUBDP: W(bits64(f64(L(0)) - f64(L(1)))); break;
    case MN_MPYDP: W(bits64(f64(L(0)) * f64(L(1)))); break;
    case MN_MPYSPDP: W(bits64(double(f32(S(0))) * f64(L(1)))); break;
    case MN_MPYSP2DP: W(bits64(double(f32(S(0))) * double(f32(S(1))))); break;
    case MN_ABSSP: { uint32_t v = S(0); W32((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffff) ? 0x7fffffffu : bits32(f32(v & 0x7fffffffu))); break; }
    case MN_ABSDP: { uint64_t v = L(0); W((v & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (v & 0xfffffffffffffull) ? 0x7fffffffffffffffull : bits64(f64(v & 0x7fffffffffffffffull))); break; }
    case MN_INTSP: W32(bits32(float(int32_t(S(0))))); break;
    case MN_INTSPU: W32(bits32(float(S(0)))); break;
    case MN_INTDP: W(bits64(double(int32_t(S(0))))); break;
    case MN_INTDPU: W(bits64(double(S(0)))); break;
    case MN_SPINT: W32(uint32_t(toInt(f32(S(0)), false))); break;
    case MN_SPTRUNC: W32(uint32_t(toInt(f32(S(0)), true))); break;
    case MN_DPINT: W32(uint32_t(toInt(f64(L(0)), false))); break;
    case MN_DPTRUNC: W32(uint32_t(toInt(f64(L(0)), true))); break;
    case MN_SPDP: W(bits64(double(f32(S(0))))); break;
    case MN_DPSP: W32(bits32(float(f64(L(0))))); break;
    case MN_RCPSP: W32(approxSp(1.0 / double(f32(S(0))))); break;
    case MN_RSQRSP: W32(approxSp(1.0 / std::sqrt(double(f32(S(0)))))); break;
    case MN_RCPDP: W(approxDp(1.0 / f64(L(0)))); break;
    case MN_RSQRDP: W(approxDp(1.0 / std::sqrt(f64(L(0))))); break;
    case MN_CMPEQSP: W32(f32(S(0)) == f32(S(1))); break;
    case MN_CMPGTSP: W32(f32(S(0)) > f32(S(1))); break;
    case MN_CMPLTSP: W32(f32(S(0)) < f32(S(1))); break;
    case MN_CMPEQDP: W32(f64(L(0)) == f64(L(1))); break;
    case MN_CMPGTDP: W32(f64(L(0)) > f64(L(1))); break;
    case MN_CMPLTDP: W32(f64(L(0)) < f64(L(1))); break;

    // ---- address arithmetic (circular as AMR says, like a memory operand's)
    case MN_ADDAB: case MN_ADDAH: case MN_ADDAW: case MN_ADDAD: case MN_SUBAB: case MN_SUBAH: case MN_SUBAW: {
        unsigned sh = op.mn == MN_ADDAB || op.mn == MN_SUBAB ? 0 : op.mn == MN_ADDAH || op.mn == MN_SUBAH ? 1 : op.mn == MN_ADDAD ? 3 : 2;
        bool minus = op.mn == MN_SUBAB || op.mn == MN_SUBAH || op.mn == MN_SUBAW;
        uint32_t off = (in.op[1].kind == Operand::Const ? uint32_t(in.op[1].value) : S(1)) << sh;
        if (in.op[0].kind == Operand::Reg && op.unit == U_D) {
            Operand m; m.kind = Operand::Mem; m.base = in.op[0].reg; m.offset = off; m.scaled = false; m.size = 1;
            m.mode = minus ? M_NEG : M_POS;
            uint32_t nb = 0; bool upd = false;
            W32(effectiveAddress(x, m, nb, upd));
        } else W32(minus ? S(0) - off : S(0) + off);
        break;
    }

    // ---- loads and stores
    case MN_LDB: memop(false, in.op[0], 1, true); break;
    case MN_LDBU: memop(false, in.op[0], 1, false); break;
    case MN_LDH: memop(false, in.op[0], 1, true); break;
    case MN_LDHU: memop(false, in.op[0], 1, false); break;
    case MN_LDW: case MN_LDNW: case MN_LDDW: case MN_LDNDW: case MN_LL: memop(false, in.op[0], 1, false); break;
    case MN_STB: case MN_STH: case MN_STW: case MN_STNW: case MN_STDW: case MN_STNDW: case MN_SL: memop(true, in.op[1], 0, false); break;
    case MN_CMTL: memop(false, in.op[0], 1, false); break;

    // ---- control
    case MN_B:
        if (in.op[0].kind == Operand::Text) {
            bool nrp = std::strcmp(in.op[0].text, "NRP") == 0;
            branch(nrp ? cr_[CR_NRP] : cr_[CR_IRP]);
            if (!nrp) cr_[CR_CSR] = (cr_[CR_CSR] & ~1u) | ((cr_[CR_CSR] >> 1) & 1);   // B IRP: GIE = PGIE
        } else branch(in.op[0].kind == Operand::Reg ? S(0) : uint32_t(in.op[0].value));
        break;
    case MN_BNOP: branch(in.op[0].kind == Operand::Reg ? S(0) : uint32_t(in.op[0].value)); break;
    case MN_CALLP: branch(uint32_t(in.op[0].value)); scheduleWrite(in, 1, x.issue, x.nextPc); break;
    case MN_BDEC: {
        int32_t v = int32_t(S(1));
        if (v >= 0) { branch(uint32_t(in.op[0].value)); scheduleWrite(in, 1, x.issue, uint32_t(v - 1)); }
        break;
    }
    case MN_BPOS: if (int32_t(S(1)) >= 0) branch(uint32_t(in.op[0].value)); break;
    case MN_MVC: {
        // To a control register: MVC's 32-bit form names it by its crlo/crhi fields, the compact Sx1 form
        // (rts6740's writemsg: SPLOOPD 1 || MVC B6,ILC) by the ILC operand itself.
        const bool toIlc = in.op[1].kind == Operand::Text && std::strcmp(in.op[1].text, "ILC") == 0;
        if (in.op[1].kind == Operand::Ctrl || toIlc) {
            int cr = toIlc ? int(CR_ILC) : ctrlOf(in.op[1].value);
            if (cr < 0) { fault("MVC to a control register this CPU does not have", &in); break; }
            uint32_t v = S(0);
            uint64_t at = x.issue;
            if (cr == CR_ILC || cr == CR_RILC) at = x.issue + 3;       // seen by the loop buffer four cycles on (SPRUFE8 7.9.7)
            if (cr == CR_ISR) { cr_[CR_IFR] |= v; break; }
            if (cr == CR_ICR) { cr_[CR_IFR] &= ~v; break; }
            if (cr == CR_CSR) v = (cr_[CR_CSR] & 0xffff0000u) | (v & 0xffffu);
            writeReg(64 + cr, v, at, &in);
        } else {
            int cr = ctrlOf(in.op[0].value);
            W32(cr < 0 ? 0 : readCtrl(cr, x.issue));
        }
        break;
    }
    case MN_DINT: { uint32_t gie = cr_[CR_CSR] & 1; cr_[CR_TSR] = (cr_[CR_TSR] & ~4u) | (gie << 2); cr_[CR_CSR] &= ~1u; cr_[CR_TSR] &= ~1u; break; }
    case MN_RINT: { uint32_t sgie = (cr_[CR_TSR] >> 2) & 1; cr_[CR_CSR] = (cr_[CR_CSR] & ~1u) | sgie; cr_[CR_TSR] = (cr_[CR_TSR] & ~1u) | sgie; break; }
    case MN_SWE: case MN_SWENR: fault(std::string(op.name) + ": a software exception, and the exception machinery is not modelled", &in); break;
    case MN_NOP: case MN_IDLE: break;
    default: fault(std::string(op.name) + " is not implemented", &in); break;
    }
}

} // namespace c6x
