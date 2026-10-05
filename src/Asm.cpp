#include "Asm.h"
#include "Cpu.h"
#include "Isa.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

enum Section { Text, Data, Const, Bss, Init, Exidx, SectionCount };

struct Sym { int section; uint32_t offset; bool defined = false; bool weak = false; };

struct Line {
    std::string label, pred, mnem, unit;
    bool predNeg = false, parallel = false, caret = false;
    std::vector<std::string> operands;
    std::string text;
    int number = 0;
};

// One file's state across the two passes.
struct Unit {
    std::string path;
    std::vector<Line> lines;
    std::map<std::string, Sym> locals;
    std::vector<std::string> exported;      // .global / .weak names
    std::vector<std::string> weak;
    std::map<std::string, std::string> asg; // .asg register, name - cl6x's FP, DP and SP
    uint32_t size[SectionCount] = { 0, 0, 0, 0, 0, 0 };
    uint32_t base[SectionCount] = { 0, 0, 0, 0, 0, 0 };
    // The largest alignment a section asked for. The first pass aligns offsets within the section
    // and the second aligns addresses, and the two agree only if the section's base is aligned at
    // least this much: a `.align 64` in a section placed at 8 put a label 16 bytes from its bytes, and the object read as zero.
    uint32_t align[SectionCount] = { 8, 8, 8, 8, 8, 8 };
};

struct Assembler {
    const Layout &layout;
    const std::vector<std::string> &nativeNames;
    Program &prog;
    std::vector<Unit> units;
    std::map<std::string, std::pair<int, Sym> > globals;   // name -> unit, sym
    std::map<std::string, uint32_t> natives;
    std::string error;
    int alignBytes = 4;

    Assembler(const Layout &l, const std::vector<std::string> &n, Program &p)
        : layout(l), nativeNames(n), prog(p) {}

    bool fail(const Unit &u, const Line &ln, const std::string &msg) {
        error = u.path + ":" + std::to_string(ln.number) + ": " + msg;
        return false;
    }

    static std::string trim(const std::string &s) {
        size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }
    static std::string upper(std::string s) {
        for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // Split operands at commas outside quotes, parentheses and brackets.
    static std::vector<std::string> splitOperands(const std::string &s) {
        std::vector<std::string> out;
        std::string cur;
        int depth = 0;
        bool quoted = false;
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            if (quoted) { cur += c; if (c == '"' && s[i - 1] != '\\') quoted = false; continue; }
            if (c == '"') { quoted = true; cur += c; continue; }
            if (c == '(' || c == '[') depth++;
            if (c == ')' || c == ']') depth--;
            if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
            cur += c;
        }
        if (!trim(cur).empty()) out.push_back(trim(cur));
        return out;
    }

    // label: [pred] [||] mnem[.unit] operands ; comment
    bool parseLine(Unit &u, const std::string &raw, int number) {
        Line ln;
        ln.number = number;
        std::string s = raw;
        // Strip the comment: ';' anywhere, and '*' or '//' at the start.
        {
            bool quoted = false;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == '"') quoted = !quoted;
                if (!quoted && s[i] == ';') { s = s.substr(0, i); break; }
                if (!quoted && s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') { s = s.substr(0, i); break; }
            }
        }
        s = trim(s);
        if (s.empty() || s[0] == '*') return true;
        // A label ends the first word with ':'; TI also allows a bare name at
        // column 0 - taken as a label when what follows is empty or is not a
        // known mnemonic or directive.
        size_t sp = s.find_first_of(" \t");
        std::string first = s.substr(0, sp);
        if (first.size() > 1 && first.back() == ':') {
            ln.label = first.substr(0, first.size() - 1);
            s = sp == std::string::npos ? std::string() : trim(s.substr(sp));
        } else if (!raw.empty() && !std::isspace(static_cast<unsigned char>(raw[0])) &&
                   first[0] != '.' && first[0] != '[' && first.compare(0, 2, "||") != 0 && first[0] != '^' &&
                   !isaKnown(upper(first))) {
            ln.label = first;
            s = sp == std::string::npos ? std::string() : trim(s.substr(sp));
        }
        if (!s.empty() && s[0] == '[') {
            size_t close = s.find(']');
            if (close == std::string::npos) return fail(u, ln, "unclosed predicate");
            std::string p = trim(s.substr(1, close - 1));
            if (!p.empty() && p[0] == '!') { ln.predNeg = true; p = trim(p.substr(1)); }
            ln.pred = upper(p);
            s = trim(s.substr(close + 1));
        }
        if (s.compare(0, 2, "||") == 0) { ln.parallel = true; s = trim(s.substr(2)); }
        if (!s.empty() && s[0] == '^') { ln.caret = true; s = trim(s.substr(1)); }
        if (!s.empty() && s[0] == '[') {                 // || [A1] B ... is written both ways
            size_t close = s.find(']');
            std::string p = trim(s.substr(1, close - 1));
            if (!p.empty() && p[0] == '!') { ln.predNeg = true; p = trim(p.substr(1)); }
            ln.pred = upper(p);
            s = trim(s.substr(close + 1));
        }
        if (!s.empty()) {
            sp = s.find_first_of(" \t");
            ln.mnem = s.substr(0, sp);
            std::string rest = sp == std::string::npos ? std::string() : trim(s.substr(sp));
            // A unit specifier: .L1 .S2X .D1T2 .M1 - kept as its unit and side, which SPMASK masks by.
            if (ln.mnem[0] != '.') {
                size_t dot = ln.mnem.find('.');
                std::string spec;
                if (dot != std::string::npos) { spec = ln.mnem.substr(dot + 1); ln.mnem = ln.mnem.substr(0, dot); }
                else if (!rest.empty() && rest[0] == '.') {
                    size_t e = rest.find_first_of(" \t");
                    spec = rest.substr(1, e == std::string::npos ? std::string::npos : e - 1);
                    rest = e == std::string::npos ? std::string() : trim(rest.substr(e));
                }
                spec = upper(spec);
                if (spec.size() >= 2 && std::strchr("LSDM", spec[0]) && (spec[1] == '1' || spec[1] == '2'))
                    ln.unit = spec.substr(0, 2);
                ln.mnem = upper(ln.mnem);
            } else {
                ln.mnem = "." + lowerOf(ln.mnem.substr(1));
            }
            ln.operands = splitOperands(rest);
            ln.text = rest;
        }
        u.lines.push_back(ln);
        return true;
    }
    static std::string lowerOf(std::string s) {
        for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // ---- expressions: numbers, symbols, + and - ---------------------------
    struct Term { bool isSym = false; std::string sym; long long value = 0; };
    static bool number(const std::string &t, long long &v) {
        if (t.empty()) return false;
        if (t.size() >= 3 && t[0] == '\'' && t.back() == '\'') { v = static_cast<unsigned char>(t[1]); return true; }
        if (!std::isdigit(static_cast<unsigned char>(t[0]))) return false;   // a symbol
        char *end = nullptr;
        v = std::strtoll(t.c_str(), &end, 0);
        if (*end == '\0') return true;
        // TI writes hex as 0FFh too: a digit first, hex digits, then h.
        if ((*end == 'h' || *end == 'H') && end[1] == '\0') {
            v = std::strtoll(t.c_str(), &end, 16);
            return *end == 'h' || *end == 'H';
        }
        return false;
    }
    // Evaluate in pass two; in pass one only the shape is checked.
    // TI's relocation operators - $EXIDX_FUNC(f), $EXIDX_EXTAB("t"), $EXTAB_LP(l), $EXTAB_RTTI(t),
    // $EXTAB_SCOPE(l) - are the symbol's address here, where nothing is relative: the name inside, unquoted.
    static std::string tiOperator(const std::string &term) {
        if (term.empty() || term[0] != '$' || term.back() != ')') return term;
        size_t open = term.find('(');
        if (open == std::string::npos) return term;
        std::string inner = term.substr(open + 1, term.size() - open - 2);
        if (inner.size() >= 2 && inner[0] == '"' && inner.back() == '"') inner = inner.substr(1, inner.size() - 2);
        return inner;
    }

    bool evaluate(const Unit &u, const Line &ln, const std::string &expr,
                  bool resolve, long long &value, bool *hadSymbol = nullptr) {
        std::string s = trim(expr);
        value = 0;
        int sign = 1;
        size_t i = 0;
        bool any = false;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
            if (i >= s.size()) break;
            if (s[i] == '+') { sign = 1; i++; continue; }
            if (s[i] == '-') { sign = -1; i++; continue; }
            size_t j = i;
            if (s[j] == '\'') { j = s.find('\'', j + 1); j = j == std::string::npos ? s.size() : j + 1; }
            else while (j < s.size() && s[j] != '+' && s[j] != '-' && !std::isspace(static_cast<unsigned char>(s[j]))) j++;
            std::string term = tiOperator(s.substr(i, j - i));
            long long v;
            if (number(term, v)) {
                value += sign * v;
            } else {
                if (hadSymbol) *hadSymbol = true;
                if (resolve) {
                    uint32_t a;
                    if (!lookup(u, term, a)) return fail(u, ln, "undefined symbol '" + term + "'");
                    value += sign * static_cast<long long>(a);
                }
            }
            any = true;
            sign = 1;
            i = j;
        }
        if (!any) return fail(u, ln, "an expression is missing");
        return true;
    }

    bool lookup(const Unit &u, const std::string &name, uint32_t &addr) {
        std::map<std::string, Sym>::const_iterator l = u.locals.find(name);
        if (l != u.locals.end() && l->second.defined) {          // a .set value has no section
            addr = l->second.section < 0 ? l->second.offset : units[&u - &units[0]].base[l->second.section] + l->second.offset;
            return true;
        }
        std::map<std::string, std::pair<int, Sym> >::const_iterator g = globals.find(name);
        if (g != globals.end()) {
            const Sym &gs = g->second.second;
            addr = gs.section < 0 ? gs.offset : units[g->second.first].base[gs.section] + gs.offset;
            return true;
        }
        std::map<std::string, uint32_t>::const_iterator n = natives.find(name);
        if (n != natives.end()) { addr = n->second; return true; }
        for (const std::string &nn : nativeNames)
            if (nn == name) {
                addr = 0x100 + 4 * static_cast<uint32_t>(natives.size());
                natives[name] = addr;
                return true;
            }
        return false;
    }

    // ---- registers and memory operands --------------------------------------
    static bool regNumber(const Unit &u, const std::string &t, int &r) {
        std::string s = upper(trim(t));
        // An .asg name stands for its register in this file alone: cl6x
        // opens every file with .asg A15, FP / B14, DP / B15, SP, and c90
        // has a plain symbol called fp.
        std::map<std::string, std::string>::const_iterator a = u.asg.find(s);
        if (a != u.asg.end()) s = a->second;
        if (s == "ILC") { r = Cpu::ILC; return true; }      // the loop buffer's control registers, for MVC
        if (s == "RILC") { r = Cpu::RILC; return true; }
        if (s.size() < 2 || (s[0] != 'A' && s[0] != 'B')) return false;
        for (size_t i = 1; i < s.size(); i++) if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        int n = std::atoi(s.c_str() + 1);
        if (n > 31) return false;
        r = (s[0] == 'B' ? Cpu::B : Cpu::A) + n;
        return true;
    }
    bool operand(const Unit &u, const Line &ln, const std::string &t, bool resolve, Operand &op) {
        std::string s = trim(t);
        int r;
        size_t colon = s.find(':');
        if (colon != std::string::npos && s[0] != '*') {
            int hi = 0, lo = 0;
            if (!regNumber(u, s.substr(0, colon), hi) || !regNumber(u, s.substr(colon + 1), lo))
                return fail(u, ln, "bad register pair '" + s + "'");
            if (hi != lo + 1 || (lo & 1)) return fail(u, ln, "a register pair is odd:even, '" + s + "' is not");
            op.kind = Operand::Reg; op.reg = lo; op.reg2 = hi;
            return true;
        }
        if (regNumber(u, s, r)) { op.kind = Operand::Reg; op.reg = r; return true; }
        if (s[0] == '*') return memOperand(u, ln, s, resolve, op);
        op.kind = Operand::Imm;
        // cl6x's `ADDAW DP,(sym),B5`: the symbol's displacement from DP in bytes, the relocation
        // doing the instruction's scaling - so here the symbol's address, added as it stands.
        if (s.size() > 2 && s[0] == '(' && s.back() == ')') { op.dpByte = true; s = trim(s.substr(1, s.size() - 2)); }
        bool hadSym = false;
        long long v;
        if (!evaluate(u, ln, s, resolve, v, &hadSym)) return false;
        op.imm = v;
        if (hadSym && !resolve) op.symbol = s;
        return true;
    }
    // *R  *+R(n)  *-R(n)  *+R[n]  *R++(n)  *R--(n)  *++R(n)  *--R(n)  *+R[Rn]
    bool memOperand(const Unit &u, const Line &ln, const std::string &s, bool resolve, Operand &op) {
        op.kind = Operand::Mem;
        std::string body = s.substr(1);
        // pre-increment / decrement
        if (body.compare(0, 2, "++") == 0) { op.mode = 1; body = body.substr(2); }
        else if (body.compare(0, 2, "--") == 0) { op.mode = 2; body = body.substr(2); }
        else if (body[0] == '+') { body = body.substr(1); }
        else if (body[0] == '-') { op.negative = true; body = body.substr(1); }
        size_t e = 0;
        while (e < body.size() && std::isalnum(static_cast<unsigned char>(body[e]))) e++;
        if (!regNumber(u, body.substr(0, e), op.base)) return fail(u, ln, "bad address '" + s + "'");
        std::string rest = trim(body.substr(e));
        if (rest.compare(0, 2, "++") == 0) { op.mode = 3; rest = trim(rest.substr(2)); }
        else if (rest.compare(0, 2, "--") == 0) { op.mode = 4; rest = trim(rest.substr(2)); }
        if (rest.empty()) { if (op.mode == 3 || op.mode == 4) { op.off = 1; op.scaled = true; } return true; }
        char open = rest[0], close = open == '(' ? ')' : ']';
        if ((open != '(' && open != '[') || rest.back() != close)
            return fail(u, ln, "bad offset in '" + s + "'");
        op.scaled = open == '[';
        std::string inner = trim(rest.substr(1, rest.size() - 2));
        int r;
        if (regNumber(u, inner, r)) { op.offReg = r; return true; }
        long long v;
        if (!evaluate(u, ln, inner, resolve, v)) return false;
        op.off = v;
        return true;
    }

    // ---- pass one: sizes and labels ------------------------------------------
    uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

    // A .sect name, with or without quotes, and with the subsection cl6x
    // appends after a colon (".text:main", ".fardata:box"); a second operand
    // such as RW is left to the caller to ignore.
    static bool sectionOf(std::string n, int &sec) {
        if (n.size() >= 2 && n[0] == '"') n = n.substr(1, n.size() - 2);
        std::string head = n.substr(0, n.find(':'));
        if (head == ".text") sec = Text;
        else if (head == ".data" || head == ".fardata" || head == ".neardata") sec = Data;
        else if (head == ".const" || head == ".rodata" || head == ".switch" || head.compare(0, 6, ".const") == 0) sec = Const;
        else if (head == ".bss" || head == ".far") sec = Bss;
        else if (head == ".init_array") sec = Init;
        else if (head.compare(0, 13, ".c6xabi.exidx") == 0) sec = Exidx;
        else if (head.compare(0, 13, ".c6xabi.extab") == 0) sec = Const;
        else if (head.compare(0, 5, ".text") == 0) sec = Text;
        else return false;
        return true;
    }

    // .bits value, n and .field value, n: n bits of data, whole bytes only
    // here (cl6x writes a char array as .bits x, 8, a vtable slot as
    // .field f, 32 and padding as .bits 0, 24); .field aligns to its width.
    bool bitsWidth(const Unit &u, const Line &ln, int &bytes) {
        long long n;
        if (ln.operands.size() != 2 || !evaluate(u, ln, ln.operands[1], false, n))
            return fail(u, ln, ln.mnem + " needs a value and a width");
        if (n <= 0 || n % 8 != 0 || n > 64) return fail(u, ln, ln.mnem + " of " + std::to_string(n) + " bits is not whole bytes");
        bytes = static_cast<int>(n / 8);
        return true;
    }

    bool passOne(Unit &u) {
        int sec = Text;
        for (const Line &ln : u.lines) {
            if (!ln.label.empty()) {
                if (u.locals.count(ln.label) && u.locals[ln.label].defined)
                    return fail(u, ln, "label '" + ln.label + "' defined twice");
                Sym s; s.section = sec; s.offset = u.size[sec]; s.defined = true;
                u.locals[ln.label] = s;
            }
            if (ln.mnem.empty()) continue;
            const std::string &m = ln.mnem;
            if (m[0] == '.') {
                if (m == ".text") sec = Text;
                else if (m == ".data") sec = Data;
                else if (m == ".sect") {
                    std::string n = ln.operands.empty() ? "" : ln.operands[0];
                    if (!sectionOf(n, sec)) return fail(u, ln, "unknown section '" + n + "'");
                } else if (m == ".asg") {
                    int r;
                    if (ln.operands.size() != 2 || !regNumber(u, ln.operands[0], r))
                        return fail(u, ln, ".asg names a register here, nothing else");
                    u.asg[upper(trim(ln.operands[1]))] = upper(trim(ln.operands[0]));
                } else if (m == ".group") {
                    // cl6x's COMDAT group, named for its key symbol: one
                    // definition is kept across units, as for .weak.
                    std::string n = ln.operands.empty() ? "" : ln.operands[0];
                    if (n.size() >= 2 && n[0] == '"') n = n.substr(1, n.size() - 2);
                    u.weak.push_back(n);
                } else if (m == ".global" || m == ".globl" || m == ".def" || m == ".ref") {
                    for (const std::string &n : ln.operands) u.exported.push_back(n);
                } else if (m == ".weak") {
                    for (const std::string &n : ln.operands) { u.exported.push_back(n); u.weak.push_back(n); }
                } else if (m == ".bss" || m == ".usect") {
                    // .bss sym, size[, align] and .usect "name", size[, align]: the size second in both, and
                    // the symbol - .bss's first operand, .usect's label - names the reserved block (cl6x's `comp: .usect`).
                    if (ln.operands.size() < 2) return fail(u, ln, m + " needs a size");
                    long long size, align = 4;
                    if (!evaluate(u, ln, ln.operands[1], false, size)) return false;
                    if (ln.operands.size() > 2 && !evaluate(u, ln, ln.operands[2], false, align)) return false;
                    if (align <= 0) align = 1;
                    if (static_cast<uint32_t>(align) > u.align[Bss]) u.align[Bss] = static_cast<uint32_t>(align);
                    u.size[Bss] = alignUp(u.size[Bss], static_cast<uint32_t>(align));
                    const std::string name = m == ".bss" ? ln.operands[0] : ln.label;
                    if (!name.empty()) {
                        Sym s; s.section = Bss; s.offset = u.size[Bss]; s.defined = true;
                        u.locals[name] = s;
                    }
                    u.size[Bss] += static_cast<uint32_t>(size);
                } else if (m == ".align") {
                    long long a = 4;
                    if (!ln.operands.empty() && !evaluate(u, ln, ln.operands[0], false, a)) return false;
                    if (a > 0 && static_cast<uint32_t>(a) > u.align[sec]) u.align[sec] = static_cast<uint32_t>(a);
                    u.size[sec] = alignUp(u.size[sec], static_cast<uint32_t>(a));
                    if (!ln.label.empty()) u.locals[ln.label].offset = u.size[sec];
                } else if (m == ".bits" || m == ".field") {
                    int w;
                    if (!bitsWidth(u, ln, w)) return false;
                    if (m == ".field" && w <= 4) u.size[sec] = alignUp(u.size[sec], static_cast<uint32_t>(w));
                    u.size[sec] += static_cast<uint32_t>(w);
                } else if (m == ".byte" || m == ".char") u.size[sec] += static_cast<uint32_t>(ln.operands.size());
                else if (m == ".short" || m == ".half" || m == ".uhalf") u.size[sec] += 2 * static_cast<uint32_t>(ln.operands.size());
                else if (m == ".word" || m == ".long" || m == ".int" || m == ".ulong") u.size[sec] += 4 * static_cast<uint32_t>(ln.operands.size());
                else if (m == ".space" || m == ".bes") {
                    long long n;
                    if (ln.operands.empty() || !evaluate(u, ln, ln.operands[0], false, n)) return fail(u, ln, m + " needs a count");
                    u.size[sec] += static_cast<uint32_t>(n);
                } else if (m == ".string" || m == ".cstring") {
                    for (const std::string &o : ln.operands) {
                        std::string bytes;
                        if (!stringBytes(u, ln, o, bytes)) return false;
                        u.size[sec] += static_cast<uint32_t>(bytes.size()) + (m == ".cstring" ? 1 : 0);
                    }
                } else if (m == ".set" || m == ".equ") {
                    // `.set name, value`, or TI's `name .set value` with the name in the label column.
                    std::string name = ln.operands.size() == 2 ? ln.operands[0] : ln.label;
                    if (name.empty() || ln.operands.empty() || ln.operands.size() > 2) return fail(u, ln, m + " needs a name and a value");
                    long long v;
                    if (!evaluate(u, ln, ln.operands.back(), false, v)) return false;
                    Sym s; s.section = -1; s.offset = static_cast<uint32_t>(v); s.defined = true;
                    u.locals[name] = s;
                } else if (m == ".end" || m == ".file" || m == ".clink" || m == ".nocmp" || m == ".symdepend" ||
                           m == ".compiler_opts" || m == ".retain" ||
                           m == ".ident" || m == ".p2align" || m == ".size" || m == ".type" ||
                           m == ".battr" || m == ".elfsym" || m == ".hidden" || m == ".gmember" || m == ".endgroup") {
                    // nothing
                } else return fail(u, ln, "unknown directive '" + m + "'");
                continue;
            }
            if (sec != Text) return fail(u, ln, "an instruction outside .text");
            if (!isaKnown(m)) return fail(u, ln, "unknown instruction '" + m + "'");
            u.size[Text] += 4;
        }
        return true;
    }

    // A .string operand: quoted text, or a byte value - TI writes a
    // newline-terminated string as "text",10,0.
    bool stringBytes(const Unit &u, const Line &ln, const std::string &lit, std::string &out) {
        if (lit.empty() || lit[0] != '"') {
            long long v;
            if (!evaluate(u, ln, lit, true, v)) return false;
            out += static_cast<char>(v);
            return true;
        }
        if (lit.size() < 2 || lit.back() != '"') return fail(u, ln, "an unclosed string");
        for (size_t i = 1; i + 1 < lit.size(); i++) {
            char c = lit[i];
            if (c != '\\') { out += c; continue; }
            char n = lit[++i];
            switch (n) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            default: out += n;
            }
        }
        return true;
    }

    // ---- pass two: bytes and instructions -------------------------------------
    bool passTwo(Unit &u) {
        int sec = Text;
        uint32_t at[SectionCount];
        for (int k = 0; k < SectionCount; k++) at[k] = u.base[k];
        std::vector<uint8_t> &m = prog.memory;
        for (const Line &ln : u.lines) {
            if (ln.mnem.empty()) continue;
            const std::string &mn = ln.mnem;
            if (mn[0] == '.') {
                if (mn == ".text") sec = Text;
                else if (mn == ".data") sec = Data;
                else if (mn == ".sect") sectionOf(ln.operands[0], sec);
                else if (mn == ".bits" || mn == ".field") {
                    int w; bitsWidth(u, ln, w);
                    if (mn == ".field" && w <= 4) at[sec] = alignUp(at[sec], static_cast<uint32_t>(w));
                    long long v; evaluate(u, ln, ln.operands[0], true, v);
                    for (int k = 0; k < w; k++) m[at[sec] + k] = static_cast<uint8_t>(v >> (8 * k));
                    at[sec] += w;
                } else if (mn == ".align") {
                    long long a = 4;
                    if (!ln.operands.empty()) evaluate(u, ln, ln.operands[0], true, a);
                    at[sec] = alignUp(at[sec], static_cast<uint32_t>(a));
                } else if (mn == ".byte" || mn == ".char" || mn == ".short" || mn == ".half" || mn == ".uhalf" ||
                           mn == ".word" || mn == ".long" || mn == ".int" || mn == ".ulong") {
                    int w = (mn == ".byte" || mn == ".char") ? 1 : (mn == ".short" || mn == ".half" || mn == ".uhalf") ? 2 : 4;
                    for (const std::string &o : ln.operands) {
                        long long v;
                        if (!evaluate(u, ln, o, true, v)) return false;
                        if (at[sec] + w > m.size()) return fail(u, ln, "data past the end of memory");
                        if (w == 1) m[at[sec]] = static_cast<uint8_t>(v);
                        else if (w == 2) wr16(m, at[sec], static_cast<uint16_t>(v));
                        else wr32(m, at[sec], static_cast<uint32_t>(v));
                        at[sec] += w;
                    }
                } else if (mn == ".space" || mn == ".bes") {
                    long long n; evaluate(u, ln, ln.operands[0], true, n);
                    at[sec] += static_cast<uint32_t>(n);
                } else if (mn == ".string" || mn == ".cstring") {
                    for (const std::string &o : ln.operands) {
                        std::string bytes; stringBytes(u, ln, o, bytes);
                        if (mn == ".cstring") bytes.push_back('\0');
                        for (char c : bytes) m[at[sec]++] = static_cast<uint8_t>(c);
                    }
                } else if (mn == ".bss" || mn == ".usect") {
                    long long size, align = 4;
                    evaluate(u, ln, ln.operands[1], true, size);
                    if (ln.operands.size() > 2) evaluate(u, ln, ln.operands[2], true, align);
                    at[Bss] = alignUp(at[Bss], static_cast<uint32_t>(align)) + static_cast<uint32_t>(size);
                }
                continue;
            }
            Instr in;
            in.mnem = mn;
            in.parallel = ln.parallel;
            in.caret = ln.caret;
            in.unit = ln.unit;
            in.file = u.path;
            in.line = ln.number;
            if (!ln.pred.empty()) {
                int r;
                if (!regNumber(u, ln.pred, r)) return fail(u, ln, "bad predicate '" + ln.pred + "'");
                if ((r % Cpu::B) > 2) return fail(u, ln, "only A0-A2 and B0-B2 can predicate; '" + ln.pred + "' cannot");
                in.pred = r; in.predNeg = ln.predNeg;
            }
            // SPMASK(R)'s operands are units, not values: D1, L2 ...
            if (mn == "SPMASK" || mn == "SPMASKR") {
                for (const std::string &o : ln.operands) in.maskUnits.push_back(upper(o));
            } else for (const std::string &o : ln.operands) {
                Operand op;
                if (!operand(u, ln, o, true, op)) return false;
                in.ops.push_back(op);
            }
            std::string why;
            if (!isaCheck(in, why)) return fail(u, ln, why);
            if (at[Text] & 3) return fail(u, ln, "an instruction at an unaligned address");
            prog.code[at[Text]] = in;
            at[Text] += 4;
        }
        return true;
    }

    bool run(const std::vector<std::string> &files) {
        prog.memory.assign(layout.memoryBytes, 0);
        if (!layout.prelude.empty()) {
            Unit u; u.path = "<runtime>";
            std::istringstream in(layout.prelude);
            std::string line;
            int n = 0;
            while (std::getline(in, line)) if (!parseLine(u, line, ++n)) return false;
            units.push_back(u);
        }
        for (const std::string &f : files) {
            Unit u; u.path = f;
            std::ifstream in(f.c_str());
            if (!in) { error = f + ": cannot open"; return false; }
            std::string line;
            int n = 0;
            while (std::getline(in, line)) if (!parseLine(u, line, ++n)) return false;
            units.push_back(u);
        }
        for (Unit &u : units) if (!passOne(u)) return false;
        // Globals: a name a unit exports and defines. Weak ones may repeat.
        for (size_t i = 0; i < units.size(); i++) {
            Unit &u = units[i];
            for (const std::string &n : u.exported) {
                std::map<std::string, Sym>::iterator s = u.locals.find(n);
                if (s == u.locals.end() || !s->second.defined) continue;   // .ref or an extern .global
                bool weak = false;
                for (const std::string &w : u.weak) if (w == n) weak = true;
                s->second.weak = weak;
                std::map<std::string, std::pair<int, Sym> >::iterator g = globals.find(n);
                if (g != globals.end()) {
                    if (weak || g->second.second.weak) continue;          // one definition kept
                    error = u.path + ": '" + n + "' is also defined in " + units[g->second.first].path;
                    return false;
                }
                globals[n] = std::make_pair(static_cast<int>(i), s->second);
            }
        }
        // Layout: every unit's text, then data, const and bss, in file order.
        uint32_t cursor = layout.textBase;
        for (int sec = 0; sec < SectionCount; sec++) {
            for (Unit &u : units) {
                cursor = alignUp(cursor, u.align[sec]);
                u.base[sec] = cursor;
                cursor += u.size[sec];
            }
        }
        prog.textBase = layout.textBase;
        prog.dataEnd = alignUp(cursor, 8);
        for (const Unit &u : units) {
            prog.initArray.push_back(std::make_pair(u.base[Init], u.size[Init]));
            prog.exidxTables.push_back(std::make_pair(u.base[Exidx], u.size[Exidx]));
        }
        if (prog.dataEnd > layout.memoryBytes / 2) { error = "the program does not fit in memory"; return false; }
        for (Unit &u : units) if (!passTwo(u)) return false;
        for (std::map<std::string, std::pair<int, Sym> >::const_iterator g = globals.begin(); g != globals.end(); ++g) {
            uint32_t a = units[g->second.first].base[g->second.second.section] + g->second.second.offset;
            prog.symbols[g->first] = a;
            prog.names[a] = g->first;
        }
        for (const Unit &u : units)
            for (std::map<std::string, Sym>::const_iterator s = u.locals.begin(); s != u.locals.end(); ++s)
                if (s->second.defined && s->second.section >= 0 && !prog.names.count(u.base[s->second.section] + s->second.offset))
                    prog.names[u.base[s->second.section] + s->second.offset] = s->first;
        prog.natives = natives;
        return true;
    }
};

} // namespace

bool assemble(const std::vector<std::string> &files, const Layout &layout,
              const std::vector<std::string> &nativeNames, Program &out,
              std::string &error) {
    Assembler a(layout, nativeNames, out);
    if (!a.run(files)) { error = a.error; return false; }
    return true;
}
