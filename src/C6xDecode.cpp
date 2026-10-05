#include "C6xDecode.h"

#include <cstdio>

namespace c6x {

// ---- fields ------------------------------------------------------------------------------------------
static uint32_t fieldBits(uint32_t w, const FormatField &f) {
    uint32_t v = 0;
    for (unsigned i = 0; i < f.pieces; i++)
        v |= ((w >> f.b[i].lo) & ((1u << f.b[i].width) - 1)) << f.b[i].pos;
    return v;
}
static unsigned fieldWidth(const FormatField &f) {
    unsigned n = 0;
    for (unsigned i = 0; i < f.pieces; i++) n += f.b[i].width;
    return n;
}
static const FormatField *findField(const Format &fmt, Field id) {
    for (unsigned i = 0; i < fmt.nfields; i++) if (fmt.f[i].id == id) return &fmt.f[i];
    return nullptr;
}
static int32_t signExtend(uint32_t v, unsigned width) {
    uint32_t m = 1u << (width - 1);
    return static_cast<int32_t>((v ^ m) - m);
}

std::string regName(int r) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%c%d", r < 32 ? 'A' : 'B', r % 32);
    return buf;
}

// ---- the fetch packet header (SPRUFE8 3.10.1) ------------------------------------------------------------
struct Header {
    bool valid = false;
    uint32_t word = 0;
    bool compact[7] = {};     // layout: word i holds two 16-bit instructions
    bool prot = false, rs = false, br = false, sat = false;
    unsigned dsz = 0;
    bool p[14] = {};          // the p-bits of the compact halves
};
static Header readHeader(const CodeReader &mem, uint32_t fp) {
    Header h;
    uint32_t w;
    if (!mem.read32(fp + 28, w) || (w & 0xf0000000u) != 0xe0000000u) return h;
    h.valid = true;
    h.word = w;
    for (int i = 0; i < 7; i++) h.compact[i] = (w >> (21 + i)) & 1;
    h.prot = (w >> 20) & 1;
    h.rs = (w >> 19) & 1;
    h.dsz = (w >> 16) & 7;
    h.br = (w >> 15) & 1;
    h.sat = (w >> 14) & 1;
    for (int i = 0; i < 14; i++) h.p[i] = (w >> i) & 1;
    return h;
}

// ---- one instruction ---------------------------------------------------------------------------------------
// Try every opcode whose format the word fits, in table order, as SPRUFE8's appendices are read: the first whose
// fixed fields and condition are valid and whose operands decode is the instruction.
static bool decodeWord(uint32_t word, unsigned bits, const Header &h, Insn &in) {
    for (unsigned oi = 0; oi < opcodeCount(); oi++) {
        const Opcode &op = opcode(oi);
        const Format &fmt = format(op.format);
        if (fmt.bits != bits || (word & fmt.mask) != fmt.cst) continue;

        int predReg = -1; bool predZero = false;
        if (const FormatField *creg = findField(fmt, F_creg)) {
            const FormatField *z = findField(fmt, F_z);
            unsigned c = fieldBits(word, *creg), zv = z ? fieldBits(word, *z) : 0;
            static const int kCreg[8] = { -1, 32, 33, 34, 1, 2, 0, -2 };   // none, B0, B1, B2, A1, A2, A0, reserved
            if (kCreg[c] == -2 || (c == 0 && zv)) continue;
            predReg = kCreg[c]; predZero = zv != 0;
        }
        if (op.flags & K_SPRED) {
            unsigned s = 0, zv = 0;
            if (const FormatField *cc = findField(fmt, F_cc)) { unsigned v = fieldBits(word, *cc); s = v >> 1; zv = v & 1; }
            else {
                const FormatField *sf = findField(fmt, F_s), *zf = findField(fmt, F_z);
                if (sf) s = fieldBits(word, *sf);
                if (zf) zv = fieldBits(word, *zf);
            }
            predReg = s ? 32 : 0; predZero = zv != 0;
        }
        bool fixedOk = true;
        for (unsigned i = 0; i < op.nfixed && fixedOk; i++) {
            const FormatField *f = findField(fmt, op.fixed[i].id);
            unsigned v = f ? fieldBits(word, *f) : 0;
            fixedOk = f && v >= op.fixed[i].min && v <= op.fixed[i].max;
        }
        if (!fixedOk) continue;

        // The unit's side, the data path's side, and the cross path.
        int side = (op.flags & K_SIDE_B) ? 2 : 0, dataSide = (op.flags & K_SIDE_T2) ? 2 : 0;
        bool cross = false, haveCross = false, haveAreg = false;
        unsigned tval = 0;
        for (unsigned e = 0; e < op.nenc; e++) {
            const FormatField *f = findField(fmt, op.enc[e].id);
            if (!f) continue;
            unsigned v = fieldBits(word, *f);
            switch (op.enc[e].coding) {
            case C_fu: side = v ? 2 : 1; break;
            case C_data_fu: dataSide = v ? 2 : 1; break;
            case C_xpath: cross = v != 0; haveCross = true; break;
            case C_rside: tval = v; dataSide = v ? 2 : 1; break;
            case C_areg: haveAreg = true; break;
            default: break;
            }
        }
        if (op.unit != U_N && side == 0) continue;
        if (haveAreg && dataSide == 0 && !haveCross) cross = side == 1;
        if ((op.flags & K_BSIDE) && side == 1) cross = true;
        const int sideBase = side == 2 ? 32 : 0;
        const int regBase = (bits == 16 && h.rs && !(op.flags & K_NORS)) ? 16 : 0;

        Insn out = in;
        out.opIndex = static_cast<int>(oi);
        out.word = word;
        out.predReg = predReg; out.predZero = predZero;
        out.unit = op.unit; out.side = side; out.dataSide = dataSide; out.cross = cross;
        out.noperands = op.noperands;
        bool ok = true;
        for (unsigned k = 0; k < op.noperands && ok; k++) {
            const OperandInfo &oinfo = op.op[k];
            Operand &o = out.op[k];
            o = Operand();
            o.size = oinfo.size;
            switch (oinfo.form) {
            case O_b15reg: o.kind = Operand::Reg; o.reg = 32 + 15; continue;
            case O_zreg: o.kind = Operand::Reg; o.reg = sideBase + 0; continue;
            case O_retreg: o.kind = Operand::Reg; o.reg = sideBase + 3; continue;
            case O_irp: o.kind = Operand::Text; o.text = "IRP"; continue;
            case O_nrp: o.kind = Operand::Text; o.text = "NRP"; continue;
            case O_ilc: o.kind = Operand::Text; o.text = "ILC"; continue;
            case O_hw_m1: o.kind = Operand::Const; o.value = -1; continue;
            case O_hw_0: o.kind = Operand::Const; o.value = 0; continue;
            case O_hw_1: o.kind = Operand::Const; o.value = 1; continue;
            case O_hw_5: o.kind = Operand::Const; o.value = 5; continue;
            case O_hw_16: o.kind = Operand::Const; o.value = 16; continue;
            case O_hw_24: o.kind = Operand::Const; o.value = 24; continue;
            case O_hw_31: o.kind = Operand::Const; o.value = 31; continue;
            default: break;
            }
            bool memBase = false, memBaseLong = false, memOff = false, memOffLong = false, memMode = false, memScaled = false;
            bool crlo = false, crhi = false, done = false, skipAll = false;
            unsigned crloV = 0, crhiV = 0;
            for (unsigned e = 0; e < op.nenc && !done && ok; e++) {
                const Encoding &enc = op.enc[e];
                if (enc.operand != k) continue;
                const FormatField *f = findField(fmt, enc.id);
                if (!f) { ok = false; break; }
                uint32_t v = fieldBits(word, *f);
                switch (enc.coding) {
                case C_cst_s3i:
                    if (v == 0) v = 16; else if (v == 7) v = 8;
                    // fall through
                case C_ucst: case C_ulcst_dpr_byte: case C_ulcst_dpr_half: case C_ulcst_dpr_word: case C_lcst_low16:
                    if (oinfo.form == O_mem_long) { o.offset = v; memOffLong = true; }
                    else { o.kind = Operand::Const; o.value = v; done = true; }
                    break;
                case C_lcst_high16: o.kind = Operand::Const; o.value = int64_t(v) << 16; done = true; break;
                case C_scst_l3i:
                    o.kind = Operand::Const; o.value = v == 0 ? 8 : signExtend(v, fieldWidth(*f)); done = true; break;
                case C_scst: o.kind = Operand::Const; o.value = signExtend(v, fieldWidth(*f)); done = true; break;
                case C_ucst_minus_one: o.kind = Operand::Const; o.value = int64_t(v) + 1; done = true; break;
                case C_pcrel: case C_pcrel_half: {
                    int32_t d = signExtend(v, fieldWidth(*f));
                    d *= (h.valid && enc.coding == C_pcrel_half) ? 2 : 4;
                    o.kind = Operand::Target; o.value = static_cast<uint32_t>(in.fetchPacket + d); done = true; break;
                }
                case C_pcrel_half_unsigned:
                    o.kind = Operand::Target; o.value = in.fetchPacket + 2 * v; done = true; break;
                case C_regpair_msb:
                    o.kind = Operand::Pair; o.reg = sideBase + int((v | 1) - 1); done = true; break;
                case C_reg_shift: v <<= 1;
                    // fall through
                case C_reg:
                    switch (oinfo.form) {
                    case O_treg: o.kind = Operand::Reg; o.reg = (tval ? 32 : 0) + regBase + int(v); done = true; break;
                    case O_reg: o.kind = Operand::Reg; o.reg = sideBase + regBase + int(v); done = true; break;
                    case O_reg_nors: o.kind = Operand::Reg; o.reg = sideBase + int(v); done = true; break;
                    case O_reg_bside: o.kind = Operand::Reg; o.reg = 32 + regBase + int(v); done = true; break;
                    case O_reg_bside_nors: o.kind = Operand::Reg; o.reg = 32 + int(v); done = true; break;
                    case O_xreg: o.kind = Operand::Reg; o.reg = (((side == 2) != cross) ? 32 : 0) + regBase + int(v); done = true; break;
                    case O_dreg: o.kind = Operand::Reg; o.reg = (dataSide == 2 ? 32 : 0) + regBase + int(v); done = true; break;
                    case O_regpair: case O_xregpair: case O_dregpair: case O_tregpair: {
                        if (v & 1) { ok = false; break; }
                        int b = oinfo.form == O_regpair ? sideBase
                              : oinfo.form == O_xregpair ? (((side == 2) != cross) ? 32 : 0)
                              : oinfo.form == O_dregpair ? (dataSide == 2 ? 32 : 0) : (tval ? 32 : 0);
                        o.kind = Operand::Pair; o.reg = b + regBase + int(v); done = true; break;
                    }
                    case O_mem_deref:
                        o.kind = Operand::Mem; o.base = sideBase + regBase + int(v); o.mode = M_POS; o.offset = 0; done = true; break;
                    case O_mem_short: case O_mem_ndw: o.base = sideBase + int(v); memBase = true; break;
                    default: ok = false; break;
                    }
                    break;
                case C_reg_ptr:
                    if (v > 3) { ok = false; break; }
                    o.base = sideBase + int(4 | v); memBase = true; break;
                case C_areg:
                    if (oinfo.form == O_areg) { o.kind = Operand::Reg; o.reg = 32 + (v ? 15 : 14); done = true; }
                    else { o.base = 32 + (v ? 15 : 14); memBaseLong = true; }
                    break;
                case C_mem_offset_minus_one_noscale: case C_mem_offset_minus_one: v += 1;
                    // fall through
                case C_mem_offset_noscale: case C_mem_offset:
                    o.offset = v; memOff = true;
                    if (bits == 16) {
                        o.mode = static_cast<uint8_t>(kmode(op.flags)); memMode = true;
                        o.scaled = !(enc.coding == C_mem_offset_noscale || enc.coding == C_mem_offset_minus_one_noscale);
                        memScaled = true;
                        if (op.flags & K_B15PTR) { o.base = sideBase + 15; memBase = true; }
                    }
                    break;
                case C_mem_mode: o.mode = static_cast<uint8_t>(v); memMode = true; break;
                case C_scaled: o.scaled = v != 0; memScaled = true; break;
                case C_crlo: crloV = v; crlo = true; break;
                case C_crhi: crhiV = v; crhi = true; break;
                case C_fstg: o.kind = Operand::Fstg; o.value = v; done = true; break;
                case C_fcyc: o.kind = Operand::Fcyc; o.value = v; done = true; break;
                case C_spmask:
                    if (v == 0) skipAll = true;
                    else { o.kind = Operand::Units; o.value = v; }
                    done = true; break;
                default: break;
                }
                if (memBaseLong && memOffLong) {
                    o.kind = Operand::Mem; o.mode = M_POS; o.scaled = true; done = true;
                }
                if (memBase && memOff && memMode && (memScaled || oinfo.form != O_mem_ndw)) {
                    if ((o.mode & 3) >= 2 && !(o.mode & 8)) { ok = false; break; }   // modes 2, 3, 6, 7: reserved
                    if (o.mode & 4) { o.offsetReg = sideBase + regBase + int(o.offset); }
                    if (oinfo.form != O_mem_ndw && !(bits == 16 && memScaled)) o.scaled = true;
                    o.kind = Operand::Mem; done = true;
                }
                if (crlo && crhi) {
                    unsigned n; const ControlReg *cr = controlRegs(n);
                    bool reading = oinfo.rw == RW_r;
                    int found = -1;
                    for (unsigned c = 0; c < n; c++)
                        if (cr[c].crlo == crloV && (crhiV & cr[c].crhiMask) == 0 && (reading ? cr[c].read : cr[c].write)) { found = int(c); break; }
                    if (found < 0) { ok = false; break; }
                    o.kind = Operand::Ctrl; o.value = found; done = true;
                }
            }
            if (skipAll) { out.noperands = 0; break; }
            if (ok && o.kind == Operand::None) ok = false;
        }
        if (!ok) continue;
        in = out;
        return true;
    }
    in.opIndex = -1;
    in.word = word;
    return false;
}

std::vector<Insn> decodeFetchPacket(const CodeReader &mem, uint32_t fp) {
    std::vector<Insn> v;
    Header h = readHeader(mem, fp);
    for (int w = 0; w < 8; w++) {
        uint32_t word = 0;
        bool have = mem.read32(fp + 4 * w, word);
        if (h.valid && w == 7) {
            Insn in; in.addr = fp + 28; in.fetchPacket = fp; in.size = 4; in.word = word; in.header = true;
            v.push_back(in);
            break;
        }
        if (!have) continue;
        if (h.valid && h.compact[w]) {
            for (int half = 0; half < 2; half++) {
                Insn in; in.addr = fp + 4 * w + 2 * half; in.fetchPacket = fp; in.size = 2; in.prot = h.prot;
                uint32_t op16 = (word >> (16 * half)) & 0xffff;
                op16 |= uint32_t(h.sat) << 16 | uint32_t(h.br) << 17 | uint32_t(h.dsz) << 18;
                decodeWord(op16, 16, h, in);
                in.parallel = h.p[2 * w + half];
                v.push_back(in);
            }
        } else {
            Insn in; in.addr = fp + 4 * w; in.fetchPacket = fp; in.size = 4; in.prot = h.prot;
            decodeWord(word, 32, h, in);
            in.parallel = word & 1;
            v.push_back(in);
        }
    }
    return v;
}

bool parallelWithPrevious(const CodeReader &mem, uint32_t addr) {
    uint32_t fp = addr & ~31u;
    if (addr == fp) {
        if (fp < 32) return false;
        std::vector<Insn> prev = decodeFetchPacket(mem, fp - 32);
        for (size_t i = prev.size(); i-- > 0; ) if (!prev[i].header) return prev[i].parallel;
        return false;
    }
    std::vector<Insn> cur = decodeFetchPacket(mem, fp);
    for (size_t i = 0; i < cur.size(); i++)
        if (cur[i].addr == addr) return i > 0 && cur[i - 1].parallel;
    return false;
}

// ---- text ------------------------------------------------------------------------------------------------
// The loop's ii, which SPKERNEL's fstg/fcyc field is split by: the nearest SPLOOP-family instruction before it,
// searched back up to 48 execute packets as SPRUFE8 7.2 bounds a loop.
static int sploopIi(const CodeReader &mem, uint32_t addr) {
    uint32_t fp = addr & ~31u;
    for (int n = 0; n < 48 && fp + 32 > fp; n++, fp -= 32) {
        std::vector<Insn> v = decodeFetchPacket(mem, fp);
        for (size_t i = v.size(); i-- > 0; ) {
            if (v[i].addr >= addr || v[i].opIndex < 0) continue;
            const Opcode *o = v[i].opc();
            if ((o->flags & K_SPLOOP) && v[i].noperands > 0 && v[i].op[0].kind == Operand::Const)
                return static_cast<int>(v[i].op[0].value);
        }
        if (fp == 0) break;
    }
    return 0;
}
static unsigned fcycBits(int ii) {
    return ii <= 1 ? 0 : ii <= 2 ? 1 : ii <= 4 ? 2 : ii <= 8 ? 3 : 4;
}

static std::string hex(int64_t v) {
    char b[24];
    if (v < 0) std::snprintf(b, sizeof b, "-0x%llx", static_cast<unsigned long long>(-v));
    else std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string formatInsn(const CodeReader &mem, const Insn &in) {
    if (in.header) {
        char b[48]; std::snprintf(b, sizeof b, ".fphead 0x%08x", in.word); return b;
    }
    if (in.opIndex < 0) {
        char b[48]; std::snprintf(b, sizeof b, in.size == 2 ? ".half 0x%04x" : ".word 0x%08x", in.word & (in.size == 2 ? 0xffff : 0xffffffffu));
        return b;
    }
    const Opcode &op = *in.opc();
    std::string s;
    if (in.predReg >= 0) s += std::string("[") + (in.predZero ? "!" : "") + regName(in.predReg) + "] ";
    s += op.name;
    if (op.unit != U_N) {
        s += '.';
        s += "DLMS"[op.unit];
        s += char('0' + in.side);
        if (in.cross) s += 'X';
        if (in.dataSide) s += in.dataSide == 2 ? "T2" : "T1";
    }
    int ii = -1;
    for (unsigned k = 0; k < in.noperands; k++) {
        const Operand &o = in.op[k];
        s += k == 0 ? " " : ",";
        switch (o.kind) {
        case Operand::Reg: s += regName(o.reg); break;
        case Operand::Pair: s += regName(o.reg + 1) + ":" + regName(o.reg); break;
        case Operand::Const: s += std::to_string(o.value); break;
        case Operand::Target: s += hex(o.value); break;
        case Operand::Text: s += o.text; break;
        case Operand::Ctrl: { unsigned n; s += controlRegs(n)[o.value].name; break; }
        case Operand::Units: {
            std::string u;
            for (int i = 0; i < 8; i++) if ((o.value >> i) & 1) { u += u.empty() ? "" : ","; u += "LSDM"[i / 2]; u += char('1' + (i & 1)); }
            s += u; break;
        }
        case Operand::Fstg: case Operand::Fcyc: {
            if (ii < 0) ii = sploopIi(mem, in.addr);
            unsigned fb = fcycBits(ii);
            unsigned v = static_cast<unsigned>(o.value), t = 0;
            if (o.kind == Operand::Fstg) { for (unsigned i = fb; i < 6; i++) t = (t << 1) | ((v >> i) & 1); }
            else t = v & ((1u << fb) - 1);
            s += std::to_string(t); break;
        }
        case Operand::Mem: {
            std::string base = regName(o.base);
            std::string off;
            if (o.offsetReg >= 0) off = (o.scaled ? "[" : "(") + regName(o.offsetReg) + (o.scaled ? "]" : ")");
            else if (o.scaled && op.op[k].form == O_mem_ndw) off = "[" + std::to_string(o.offset) + "]";
            else off = "(" + std::to_string(o.scaled ? o.offset * o.size : o.offset) + ")";
            if (op.op[k].form == O_mem_deref) { s += "*" + base; break; }
            switch (o.mode & ~4u) {
            case M_NEG: s += "*-" + base + off; break;
            case M_POS: s += "*+" + base + off; break;
            case M_PREDEC: s += "*--" + base + off; break;
            case M_PREINC: s += "*++" + base + off; break;
            case M_POSTDEC: s += "*" + base + "--" + off; break;
            default: s += "*" + base + "++" + off; break;
            }
            break;
        }
        default: s += "?"; break;
        }
    }
    return s;
}

} // namespace c6x
