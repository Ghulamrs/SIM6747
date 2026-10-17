// The debugger session of Debug.h: gdb's commands in, gdb's answers out. Every stop is reported the
// way gdb reports it - "Breakpoint 1, main () at t.c:6" then "6\t<the line>", a step that stays in
// its function as the echo alone, a step that lands in another as "f () at t.c:3" first, the end as
// "[Inferior 1 (process 1) exited normally]" or "... with code 03" (octal, as gdb) - because that is
// what RIDE's debugger.cpp parses for gdb, and one parser serving three engines is the whole point.
//
// A call stack the machine never keeps is kept here: a frame is pushed when the program counter
// reaches a function's first instruction, with B3 - the return address, landed by then - beside
// it, and popped when the program counter reaches that return address. That is what `next`
// (the same depth or less), `step` (any depth), `finish` (one less) and `backtrace` read.

#include "Debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace dbg {

namespace {

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string leafOf(const std::string &path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

bool allDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// A register's number from its name - A0..A31, B0..B31, pc, sp (B15), fp (A15) - or -1.
int registerNamed(const std::string &n) {
    if (n == "sp") return 32 + 15;
    if (n == "fp") return 15;
    if (n == "pc") return 64;
    if (n.size() < 2 || n.size() > 3 || (n[0] != 'A' && n[0] != 'B')) return -1;
    if (!allDigits(n.substr(1))) return -1;
    int k = std::atoi(n.c_str() + 1);
    if (k > 31) return -1;
    return (n[0] == 'A' ? 0 : 32) + k;
}

std::string registerName(int r) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%c%d", r < 32 ? 'A' : 'B', r & 31);
    return buf;
}

struct Breakpoint { int id = 0; uint32_t addr = 0; std::string file; int line = 0; std::string function; };
struct Frame { uint32_t entry = 0; uint32_t returnPc = 0; };

class Session {
public:
    explicit Session(Machine &m) : m_(m) {}
    int run();

private:
    Machine &m_;
    std::vector<Breakpoint> breaks_;
    int nextBreak_ = 1;
    std::vector<Frame> frames_;
    bool started_ = false, alive_ = true;
    int values_ = 0;
    size_t frame_ = 0;            // the frame `frame N` selected, 0 the innermost
    uint32_t lastEntry_ = 0;      // the function the last stop was in: a stop elsewhere names its function
    Position entryPos_;           // the position of the last function entered: its own line, which a step passes

    enum Mode { Continue, Next, Step, Finish };
    void go(Mode mode);
    void track(uint32_t pc);
    const Breakpoint *breakAt(uint32_t pc) const;
    void stopped(const char *head, bool announceFunction);
    void exited();
    std::string place(uint32_t pc, bool withAddress);
    uint32_t framePc(size_t k) const;
    void say(const std::string &s) { std::fputs(s.c_str(), stdout); }
    void line(const std::string &s) { std::fputs(s.c_str(), stdout); std::fputc('\n', stdout); }

    void cmdBreak(const std::string &arg);
    void cmdDelete(const std::string &arg);
    void cmdPrint(const std::string &arg, bool typeOnly);
    void cmdSet(const std::string &arg);
    void cmdBacktrace();
    void cmdFrame(const std::string &arg);
    void cmdRegisters();
    void cmdInfoBreak();
    void cmdInfoVariables(bool params);
    void cmdExamine(const std::string &verb, const std::string &arg);
    static const char *skipSpace(const char *&p);
    bool arithmetic(const char *&p, double &v, bool &real, std::string &bad);
    bool product(const char *&p, double &v, bool &real, std::string &bad);
    bool unary(const char *&p, double &v, bool &real, std::string &bad);
    bool evaluate(const std::string &expr, uint32_t &value, bool &isRegister, int &reg);
    // -g: the selected frame's variables, where one lives, and its value as gdb prints it
    const std::vector<Variable> *frameVariables() const;
    const Variable *variableNamed(const std::string &name) const;
    bool inScope(const Variable &v) const;
    bool addressOf(const Variable &v, uint32_t &addr) const;
    std::string valueOf(const Variable &v) const;
    std::string formatAt(const TypeInfo &t, uint32_t addr, int depth) const;
    bool lvalueOf(const std::string &expr, std::string &type, uint32_t &addr);
    uint32_t sizeOfType(const std::string &type) const;
    std::string scalarAt(const TypeInfo &t, uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;
};

const Breakpoint *Session::breakAt(uint32_t pc) const {
    for (const Breakpoint &b : breaks_) if (b.addr == pc) return &b;
    return nullptr;
}

// A function's first packet pushes a frame; the program counter reaching a frame's return address
// pops it, and every frame above it - a frame a tail call or a goto left behind goes with the return.
void Session::track(uint32_t pc) {
    for (size_t k = frames_.size(); k-- > 0;)
        if (frames_[k].returnPc == pc) { frames_.resize(k); break; }
    if (m_.isFunctionEntry(pc)) {
        Frame f; f.entry = pc; f.returnPc = m_.reg(32 + 3); frames_.push_back(f);
        if (!m_.positionAt(pc, entryPos_)) entryPos_ = Position();
    }
}

uint32_t Session::framePc(size_t k) const {
    if (k == 0) return m_.pc();
    if (k >= frames_.size()) return 0;
    return frames_[frames_.size() - k].returnPc;
}

// "main () at t.c:6", or with the address in front as gdb writes a stop that is not at a line's start.
std::string Session::place(uint32_t pc, bool withAddress) {
    std::string fn, out;
    uint32_t entry = 0;
    if (!m_.functionAt(pc, fn, entry)) fn = "??";
    char buf[32];
    if (withAddress) { std::snprintf(buf, sizeof buf, "0x%08x in ", pc); out += buf; }
    out += fn + " ()";
    Position p;
    if (m_.positionAt(pc, p) && p.known()) out += " at " + p.file + ":" + std::to_string(p.line);
    return out;
}

void Session::stopped(const char *head, bool announceFunction) {
    uint32_t pc = m_.pc();
    std::string fn;
    uint32_t entry = 0;
    m_.functionAt(pc, fn, entry);
    Position p;
    bool here = m_.positionAt(pc, p) && p.known();
    if (head != nullptr && std::strcmp(head, "Run till exit done: ") == 0) line(place(pc, true));
    else if (head != nullptr) line(std::string(head) + place(pc, false));
    else if (announceFunction || entry != lastEntry_ || !here) line(place(pc, !here));
    if (here) line(std::to_string(p.line) + "\t" + m_.sourceLine(p.file, p.line));
    lastEntry_ = entry;
    frame_ = 0;
}

void Session::exited() {
    m_.finish();
    alive_ = false;
    int st = m_.status() & 0xff;
    char buf[64];
    if (st == 0) std::snprintf(buf, sizeof buf, "[Inferior 1 (process 1) exited normally]");
    else std::snprintf(buf, sizeof buf, "[Inferior 1 (process 1) exited with code %02o]", st);
    line(buf);
}

void Session::go(Mode mode) {
    const size_t depth0 = frames_.size();
    Position pos0;
    m_.positionAt(m_.pc(), pos0);
    bool first = true;
    for (;;) {
        uint32_t pc = m_.pc();
        if (!first) {
            track(pc);
            if (const Breakpoint *b = breakAt(pc)) {
                stopped(("\nBreakpoint " + std::to_string(b->id) + ", ").c_str(), true);
                return;
            }
            Position p;
            bool here = m_.positionAt(pc, p) && p.known();
            size_t depth = frames_.size();
            if (mode == Next && ((here && depth <= depth0 && p != pos0) || depth < depth0)) { stopped(nullptr, false); return; }
            // A step into a callee passes its entry position - the function's own line, over the prologue - and
            // stops on its first statement, where gdb stops; one that returns stops in the caller wherever it lands.
            // A function written on one line has its first statement on that same line: a new row there, after
            // the entry, is the statement, and the step stops on it.
            const bool prologue = depth > depth0 && p == entryPos_ &&
                                  !(m_.startsRow(pc) && !frames_.empty() && pc != frames_.back().entry);
            if (mode == Step && ((here && p != pos0 && !prologue) || depth < depth0)) { stopped(nullptr, false); return; }
            if (mode == Finish && depth < depth0) { stopped("Run till exit done: ", false); return; }
        }
        first = false;
        m_.step();
        std::fflush(stdout);
        if (m_.ended()) { exited(); return; }
    }
}

// break FILE:LINE, break LINE (the file the program counter is in), break FUNCTION.
void Session::cmdBreak(const std::string &arg) {
    std::string a = trim(arg);
    if (a.empty()) { line("No default breakpoint address now."); return; }
    Breakpoint b;
    b.id = nextBreak_;
    size_t colon = a.find_last_of(':');
    std::string file = colon == std::string::npos ? std::string() : a.substr(0, colon);
    std::string tail = colon == std::string::npos ? a : a.substr(colon + 1);
    if (allDigits(tail)) {
        if (file.empty()) {
            Position p;
            if (m_.positionAt(m_.pc(), p) && p.known()) file = p.file;
        }
        if (!m_.hasLines()) { line("No line " + tail + " in file \"" + file + "\": " + m_.noLinesWhy()); return; }
        int used = 0;
        if (file.empty() || !m_.addressOfLine(leafOf(file), std::atoi(tail.c_str()), b.addr, used)) {
            line("No line " + tail + " in file \"" + file + "\".");
            return;
        }
        b.file = file; b.line = used;
    } else {
        if (!m_.addressOfFunction(a, b.addr)) { line("Function \"" + a + "\" not defined."); return; }
        b.function = a;
        Position p;
        if (m_.positionAt(b.addr, p) && p.known()) { b.file = p.file; b.line = p.line; }
    }
    nextBreak_++;
    breaks_.push_back(b);
    char buf[48];
    std::snprintf(buf, sizeof buf, "Breakpoint %d at 0x%x", b.id, b.addr);
    std::string said = buf;
    if (b.line > 0) said += ": file " + b.file + ", line " + std::to_string(b.line) + ".";
    else said += ".";
    line(said);
}

void Session::cmdDelete(const std::string &arg) {
    std::string a = trim(arg);
    if (a.empty()) { breaks_.clear(); return; }
    int id = std::atoi(a.c_str());
    for (size_t k = 0; k < breaks_.size(); k++)
        if (breaks_[k].id == id) { breaks_.erase(breaks_.begin() + static_cast<long>(k)); return; }
    line("No breakpoint number " + a + ".");
}

void Session::cmdInfoBreak() {
    if (breaks_.empty()) { line("No breakpoints or watchpoints."); return; }
    line("Num     Type           Disp Enb Address    What");
    for (const Breakpoint &b : breaks_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%-7d breakpoint     keep y   0x%08x in ", b.id, b.addr);
        std::string fn; uint32_t entry = 0;
        if (!m_.functionAt(b.addr, fn, entry)) fn = "??";
        line(buf + fn + " at " + b.file + ":" + std::to_string(b.line));
    }
}

// A register ($A4, $pc, $sp), a number (decimal or 0x), or nothing this machine knows a name for.
bool Session::evaluate(const std::string &expr, uint32_t &value, bool &isRegister, int &reg) {
    std::string e = trim(expr);
    isRegister = false; reg = -1;
    if (e.empty()) return false;
    if (e[0] == '$') {
        reg = registerNamed(e.substr(1));
        if (reg < 0) return false;
        isRegister = true;
        value = reg == 64 ? m_.pc() : m_.reg(reg);
        return true;
    }
    char *end = nullptr;
    long long v = std::strtoll(e.c_str(), &end, 0);
    if (end != nullptr && *end == '\0') { value = static_cast<uint32_t>(v); return true; }
    return false;
}

const char *Session::skipSpace(const char *&p) { while (*p == ' ' || *p == '\t') p++; return p; }

// An expression in gdb's C: sums of products of unary terms - a number, a scalar variable, a register,
// or one in parentheses. A real anywhere makes the result real; an integer division stays integer.
bool Session::arithmetic(const char *&p, double &v, bool &real, std::string &bad) {
    if (!product(p, v, real, bad)) return false;
    for (;;) {
        char op = *skipSpace(p);
        if (op != '+' && op != '-') return true;
        p++;
        double r = 0; bool rr = false;
        if (!product(p, r, rr, bad)) return false;
        real = real || rr;
        v = op == '+' ? v + r : v - r;
    }
}

bool Session::product(const char *&p, double &v, bool &real, std::string &bad) {
    if (!unary(p, v, real, bad)) return false;
    for (;;) {
        char op = *skipSpace(p);
        if (op != '*' && op != '/' && op != '%') return true;
        p++;
        double r = 0; bool rr = false;
        if (!unary(p, r, rr, bad)) return false;
        real = real || rr;
        if ((op == '/' || op == '%') && r == 0) { bad = "Division by zero"; return false; }
        if (op == '*') v *= r;
        else if (op == '/') v = real ? v / r : static_cast<double>(static_cast<long long>(v) / static_cast<long long>(r));
        else v = static_cast<double>(static_cast<long long>(v) % static_cast<long long>(r));
    }
}

bool Session::unary(const char *&p, double &v, bool &real, std::string &bad) {
    char c = *skipSpace(p);
    if (c == '-') { p++; if (!unary(p, v, real, bad)) return false; v = -v; return true; }
    if (c == '(') {
        p++;
        if (!arithmetic(p, v, real, bad) || *skipSpace(p) != ')') return false;
        p++;
        return true;
    }
    if ((c >= '0' && c <= '9') || c == '.') {
        char *end = nullptr;
        bool hex = c == '0' && (p[1] == 'x' || p[1] == 'X');
        if (hex) v = static_cast<double>(std::strtoull(p, &end, 16));
        else { v = std::strtod(p, &end); real = std::string(p, static_cast<const char *>(end)).find_first_of(".eE") != std::string::npos; }
        p = end;
        return true;
    }
    if (c == '$' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        const char *s = p;
        if (*p == '$') p++;
        while (*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) p++;
        std::string name(s, p);
        if (const Variable *var = variableNamed(name)) {
            std::string shown = valueOf(*var);
            char *end = nullptr;
            v = std::strtod(shown.c_str(), &end);
            if (end == shown.c_str() || *end != '\0') { bad = name; return false; }   // an aggregate or a pointer
            real = real || shown.find_first_of(".eE") != std::string::npos || var->type.find("double") != std::string::npos ||
                   var->type.find("float") != std::string::npos || var->type == "real";
            return true;
        }
        int reg = registerNamed(name[0] == '$' ? name.substr(1) : name);
        if (reg >= 0 && reg < 64) { v = static_cast<int32_t>(m_.reg(reg)); return true; }
        bad = name;
        return false;
    }
    return false;
}

void Session::cmdPrint(const std::string &arg, bool typeOnly) {
    uint32_t v = 0; bool isReg = false; int reg = -1;
    std::string e = trim(arg);
    if (!evaluate(e, v, isReg, reg)) {
        std::string name = e;
        bool address = !name.empty() && name[0] == '&';
        if (address) name = trim(name.substr(1));
        if (name.size() > 1 && name[0] == '(' && name[name.size() - 1] == ')') name = trim(name.substr(1, name.size() - 2));
        // -g: a variable of the selected frame - print n, print &(n), whatis n
        if (const Variable *var = variableNamed(name)) {
            if (typeOnly) { line("type = " + var->type + (address ? " *" : "")); return; }
            values_++;
            if (address) {
                uint32_t at = 0;
                if (!addressOf(*var, at)) { line("Can't take address of \"" + name + "\" which isn't an lvalue."); return; }
                char buf[96];
                std::snprintf(buf, sizeof buf, "$%d = (%s *) 0x%x", values_, var->type.c_str(), at);
                line(buf);
            } else line("$" + std::to_string(values_) + " = " + valueOf(*var));
            return;
        }
        // a member, an element, what a pointer points to: p.x, primes[2], b.corner[1].y, *name
        std::string ltype; uint32_t laddr = 0;
        if (lvalueOf(name, ltype, laddr)) {
            if (typeOnly) { line("type = " + ltype + (address ? " *" : "")); return; }
            values_++;
            char buf[160];
            if (address) { std::snprintf(buf, sizeof buf, "$%d = (%s *) 0x%x", values_, ltype.c_str(), laddr); line(buf); return; }
            const TypeInfo *ti = m_.typeNamed(ltype);
            std::string shown = ti == nullptr ? "<unknown type " + ltype + ">"
                              : ti->kind == TypeInfo::Pointer ? "(" + ltype + ") " + scalarAt(*ti, laddr) : formatAt(*ti, laddr, 0);
            line("$" + std::to_string(values_) + " = " + shown);
            return;
        }
        // arithmetic over the frame's scalar variables, registers and numbers: n*2, (sum + i) / 2, -n
        double number = 0; bool real = false; std::string bad;
        const char *p = e.c_str();
        if (arithmetic(p, number, real, bad) && *skipSpace(p) == '\0') {
            if (typeOnly) { line(real ? "type = double" : "type = int"); return; }
            values_++;
            char buf[64];
            if (real) std::snprintf(buf, sizeof buf, "$%d = %g", values_, number);
            else std::snprintf(buf, sizeof buf, "$%d = %lld", values_, static_cast<long long>(number));
            line(buf);
            return;
        }
        if (bad == "Division by zero") line(bad);
        else {
            // a variable there is, but the access is not: an index out of range, a member it does not have
            size_t lead = name.find_first_not_of("* \t");
            size_t end = lead == std::string::npos ? lead : name.find_first_not_of("_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", lead);
            const std::string base = lead == std::string::npos ? std::string() : name.substr(lead, end == std::string::npos ? std::string::npos : end - lead);
            if (!base.empty() && base != name && variableNamed(base) != nullptr)
                line(name.find('[') != std::string::npos ? "Index out of range in \"" + name + "\"." : "There is no member named that in \"" + name + "\".");
            else line("No symbol \"" + (bad.empty() ? name : bad) + "\" in current context.");
        }
        return;
    }
    if (typeOnly) { line(reg == 64 ? "type = void (*)()" : "type = int"); return; }
    values_++;
    char buf[48];
    if (reg == 64) std::snprintf(buf, sizeof buf, "$%d = (void (*)()) 0x%x", values_, v);
    else std::snprintf(buf, sizeof buf, "$%d = %d", values_, static_cast<int>(v));
    line(buf);
}

// set variable $A4 = 5 / set var ...; any other `set` (confirm, pagination, width ...) is taken silently.
void Session::cmdSet(const std::string &arg) {
    std::string a = trim(arg);
    bool var = a.compare(0, 9, "variable ") == 0 || a.compare(0, 4, "var ") == 0;
    if (!var) return;
    a = trim(a.substr(a.find(' ') + 1));
    size_t eq = a.find('=');
    if (eq == std::string::npos) { line("A syntax error in expression, near `'."); return; }
    std::string lhs = trim(a.substr(0, eq)), rhs = trim(a.substr(eq + 1));
    // -g: a variable of the selected frame, or a member or element of one (p.x = 5, a[2] = 7) - its bytes written
    // as its type stores them
    std::string ltype; uint32_t laddr = 0;
    if (lvalueOf(lhs, ltype, laddr)) {
        std::string t = ltype;
        for (const char *q : { "const ", "volatile " })
            for (size_t at; (at = t.find(q)) != std::string::npos;) t.erase(at, std::strlen(q));
        t = trim(t);
        uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        size_t size = 0;
        char *end = nullptr;
        if (t == "double" || t == "long double" || t == "real") {
            double d = std::strtod(rhs.c_str(), &end); std::memcpy(b, &d, 8); size = 8;
        } else if (t == "float") {
            float f = static_cast<float>(std::strtod(rhs.c_str(), &end)); std::memcpy(b, &f, 4); size = 4;
        } else if (!t.empty() && t.find('[') == std::string::npos && t.compare(0, 7, "struct ") != 0 &&
                   t.compare(0, 6, "class ") != 0 && t.compare(0, 6, "union ") != 0) {
            long long n = rhs == "true" ? 1 : rhs == "false" ? 0 : std::strtoll(rhs.c_str(), &end, 0);
            if (rhs == "true" || rhs == "false") end = const_cast<char *>(rhs.c_str() + rhs.size());
            size = t.back() == '*' ? 4 : t.find("char") != std::string::npos || t == "bool" || t == "_Bool" ? 1
                 : t.find("short") != std::string::npos ? 2 : t.find("long long") != std::string::npos ? 8 : 4;
            for (size_t k = 0; k < size; k++) b[k] = static_cast<uint8_t>(static_cast<unsigned long long>(n) >> (8 * k));
        }
        if (size == 0) { line("Invalid cast."); return; }
        if (end == nullptr || trim(end).size() != 0) { line("No symbol \"" + rhs + "\" in current context."); return; }
        if (!m_.write(laddr, b, size)) line("Cannot access memory at address " + std::to_string(laddr));
        return;
    }
    uint32_t v = 0; bool isReg = false; int reg = -1;
    if (!evaluate(lhs, v, isReg, reg) || !isReg || reg == 64) {
        line("No symbol \"" + (lhs.empty() || lhs[0] != '$' ? lhs : lhs.substr(1)) + "\" in current context.");
        return;
    }
    uint32_t value = 0; bool r2 = false; int reg2 = -1;
    if (!evaluate(rhs, value, r2, reg2)) { line("No symbol \"" + rhs + "\" in current context."); return; }
    m_.setReg(reg, value);
}

void Session::cmdBacktrace() {
    if (!alive_ || !started_) { line("No stack."); return; }
    size_t n = frames_.empty() ? 1 : frames_.size();
    for (size_t k = 0; k < n; k++) {
        uint32_t pc = framePc(k);
        if (k > 0 && pc == 0) break;          // the outermost frame's return address, never set: the start-up's
        line("#" + std::to_string(k) + "  " + place(pc, k > 0));
    }
}

void Session::cmdFrame(const std::string &arg) {
    std::string a = trim(arg);
    if (!alive_ || !started_) { line("No stack."); return; }
    size_t k = a.empty() ? frame_ : static_cast<size_t>(std::atoi(a.c_str()));
    size_t n = frames_.empty() ? 1 : frames_.size();
    if (k >= n) { line("No frame at level " + a + "."); return; }
    frame_ = k;
    uint32_t pc = framePc(k);
    line("#" + std::to_string(k) + "  " + place(pc, k > 0));
    Position p;
    if (m_.positionAt(pc, p) && p.known()) line(std::to_string(p.line) + "\t" + m_.sourceLine(p.file, p.line));
}

uint32_t Session::read32(uint32_t addr) const {
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (!m_.read(addr, b, 4)) return 0;
    return b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

// The selected frame's function's variables, from the tags the compilers wrote (-g).
const std::vector<Variable> *Session::frameVariables() const {
    if (!alive_ || !started_) return nullptr;
    std::string fn;
    uint32_t entry = 0;
    if (!m_.functionAt(framePc(frame_), fn, entry)) return nullptr;
    return m_.variablesAt(entry);
}

// A variable of an inner block is there only while the selected frame is inside the block's code: frame 0 at
// its pc, an outer frame at the call it made (the return address less one, which is still the call's).
bool Session::inScope(const Variable &v) const {
    if (!v.scoped) return true;
    uint32_t pc = framePc(frame_);
    if (frame_ > 0 && pc > 0) pc -= 1;
    return pc >= v.lo && pc < v.hi;
}

const Variable *Session::variableNamed(const std::string &name) const {
    const std::vector<Variable> *vs = frameVariables();
    if (vs == nullptr) return nullptr;
    // of the ones in scope, the last of that name: a block's own variable is declared after the one it hides
    const Variable *found = nullptr;
    for (const Variable &v : *vs) if (v.name == name && inScope(v)) found = &v;
    return found;
}

// A frame variable is at the frame's A15 plus its offset. Every function the compilers write keeps the
// caller's A15 at the word its own A15 points to (STW A15, *B15 then MV B15, A15), so an outer frame's
// A15 is read down that chain, one word a frame.
bool Session::addressOf(const Variable &v, uint32_t &addr) const {
    if (v.global) {
        if (!m_.addressOfFunction(v.symbol, addr)) return false;
    } else {
        uint32_t a15 = m_.reg(15);
        for (size_t k = 0; k < frame_; k++) a15 = read32(a15);
        addr = a15 + static_cast<uint32_t>(v.offset);
    }
    if (v.deref) addr = read32(addr);
    return true;
}

// The value as gdb prints it: an integer in decimal, a real as %g, a pointer in hex, an aggregate as {...}.
// A base type's or a pointer's value at addr, as gdb prints it; a char pointer with the string it points to.
std::string Session::scalarAt(const TypeInfo &t, uint32_t addr) const {
    char buf[64];
    uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const size_t size = t.size > 0 && t.size <= 8 ? static_cast<size_t>(t.size) : 4;
    if (!m_.read(addr, b, size)) return "<unreadable>";
    uint64_t u = 0;
    for (size_t k = size; k-- > 0;) u = (u << 8) | b[k];
    if (t.kind == TypeInfo::Pointer) {
        std::snprintf(buf, sizeof buf, "0x%x", static_cast<uint32_t>(u));
        std::string out = buf;
        const TypeInfo *to = m_.typeNamed(t.element);
        if (to != nullptr && to->size == 1 && to->encoding.find("char") != std::string::npos && u != 0) {
            std::string s;
            for (uint32_t k = 0; k < 200; k++) {
                uint8_t c = 0;
                if (!m_.read(static_cast<uint32_t>(u) + k, &c, 1) || c == 0) break;
                s += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
            }
            out += " \"" + s + "\"";
        }
        return out;
    }
    if (t.encoding == "DW_ATE_float") {
        if (size == 8) { double d; std::memcpy(&d, b, 8); std::snprintf(buf, sizeof buf, "%g", d); }
        else { float f; std::memcpy(&f, b, 4); std::snprintf(buf, sizeof buf, "%g", f); }
        return buf;
    }
    if (t.encoding == "DW_ATE_boolean") return u ? "true" : "false";
    const bool isSigned = t.encoding == "DW_ATE_signed" || t.encoding == "DW_ATE_signed_char";
    long long s = isSigned && size < 8 ? static_cast<long long>(u << (64 - 8 * size)) >> (64 - 8 * size) : static_cast<long long>(u);
    if (isSigned) std::snprintf(buf, sizeof buf, "%lld", s);
    else std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(u));
    std::string out = buf;
    if (t.encoding.find("char") != std::string::npos) {   // gdb: 65 'A'
        unsigned char c = static_cast<unsigned char>(u);
        if (c >= 32 && c < 127) out += std::string(" '") + static_cast<char>(c) + "'";
    }
    return out;
}

// gdb's shape for a value: {1, 2, 3} for an array, "text" for a char array, {x = 1, y = 2} for a struct;
// at most 100 elements an array, nested four deep.
std::string Session::formatAt(const TypeInfo &t, uint32_t addr, int depth) const {
    if (depth > 4) return "{...}";
    if (t.kind == TypeInfo::Base || t.kind == TypeInfo::Pointer) return scalarAt(t, addr);
    if (t.kind == TypeInfo::Struct || t.kind == TypeInfo::Union) {
        std::string out = "{";
        for (size_t k = 0; k < t.members.size(); k++) {
            const TypeInfo *mt = m_.typeNamed(t.members[k].type);
            out += (k ? ", " : "") + t.members[k].name + " = " +
                   (mt ? formatAt(*mt, addr + static_cast<uint32_t>(t.members[k].offset), depth + 1) : std::string("{...}"));
        }
        return out + "}";
    }
    const TypeInfo *et = m_.typeNamed(t.element);
    if (et == nullptr) return "{...}";
    uint32_t data = addr;
    long count = t.count;
    if (t.kind == TypeInfo::RuntimeArray) {
        // the variable holds a pointer to Shalimar's {count, element size, data}
        const uint32_t array = read32(addr);
        if (array == 0) return "0x0";
        count = static_cast<long>(static_cast<int32_t>(read32(array)));
        data = read32(array + 8);
    }
    if (count < 0) count = 0;
    const uint32_t step = et->kind == TypeInfo::RuntimeArray ? 4u : static_cast<uint32_t>(et->size > 0 ? et->size : 4);
    if (et->kind == TypeInfo::Base && et->size == 1 && et->encoding.find("char") != std::string::npos) {
        std::string s;
        for (long k = 0; k < count && k < 200; k++) {
            uint8_t c = 0;
            if (!m_.read(data + static_cast<uint32_t>(k), &c, 1) || c == 0) break;
            s += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
        }
        return "\"" + s + "\"";
    }
    std::string out = "{";
    for (long k = 0; k < count && k < 100; k++) out += (k ? ", " : "") + formatAt(*et, data + step * static_cast<uint32_t>(k), depth + 1);
    if (count > 100) out += "...";
    return out + "}";
}

uint32_t Session::sizeOfType(const std::string &type) const {
    const TypeInfo *t = m_.typeNamed(type);
    if (t == nullptr) return 4;
    if (t->kind == TypeInfo::RuntimeArray) return 4;   // a variable of it holds a pointer
    return t->size > 0 ? static_cast<uint32_t>(t->size) : 4;
}

// An lvalue in C's postfix syntax over the frame's variables: name, name.member, ptr->member, a[i] (i any
// arithmetic), *ptr, and any chain of them - b.corner[1].y. Its type and address; false for anything else.
bool Session::lvalueOf(const std::string &expr, std::string &type, uint32_t &addr) {
    const char *p = expr.c_str();
    int derefs = 0;
    while (*skipSpace(p) == '*') { p++; derefs++; }
    skipSpace(p);
    const char *s = p;
    while (*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) p++;
    const Variable *var = variableNamed(std::string(s, p));
    if (var == nullptr || !addressOf(*var, addr)) return false;
    type = var->type;
    for (;;) {
        const char c = *skipSpace(p);
        const TypeInfo *t = m_.typeNamed(type);
        if (c == '.' || (c == '-' && p[1] == '>')) {
            if (t == nullptr) return false;
            if (c == '-') {
                if (t->kind != TypeInfo::Pointer) return false;
                addr = read32(addr);
                t = m_.typeNamed(t->element);
                if (t == nullptr) return false;
                p++;
            }
            p++;
            skipSpace(p);
            const char *m = p;
            while (*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) p++;
            const std::string member(m, p);
            if (t->kind != TypeInfo::Struct && t->kind != TypeInfo::Union) return false;
            bool found = false;
            for (const TypeInfo::Member &mb : t->members)
                if (mb.name == member) { addr += static_cast<uint32_t>(mb.offset); type = mb.type; found = true; break; }
            if (!found) return false;
        } else if (c == '[') {
            if (t == nullptr) return false;
            p++;
            double index = 0; bool real = false; std::string bad;
            if (!arithmetic(p, index, real, bad) || *skipSpace(p) != ']') return false;
            p++;
            const long n = static_cast<long>(index);
            if (t->kind == TypeInfo::Array) {
                if (n < 0 || n >= t->count) return false;
                addr += static_cast<uint32_t>(n) * sizeOfType(t->element);
            } else if (t->kind == TypeInfo::Pointer) {
                addr = read32(addr) + static_cast<uint32_t>(n) * sizeOfType(t->element);
            } else if (t->kind == TypeInfo::RuntimeArray) {
                // Shalimar's {count, element, data}, the variable a pointer to it
                const uint32_t record = read32(addr);
                if (record == 0 || n < 0 || n >= static_cast<long>(static_cast<int32_t>(read32(record)))) return false;
                addr = read32(record + 8) + static_cast<uint32_t>(n) * sizeOfType(t->element);
            } else return false;
            type = t->element;
        } else break;
    }
    if (*skipSpace(p) != '\0') return false;
    for (int k = 0; k < derefs; k++) {
        const TypeInfo *t = m_.typeNamed(type);
        if (t == nullptr || t->kind != TypeInfo::Pointer) return false;
        addr = read32(addr);
        type = t->element;
    }
    return true;
}

std::string Session::valueOf(const Variable &v) const {
    uint32_t addr = 0;
    if (!addressOf(v, addr)) return "<optimized out>";
    if (const TypeInfo *ti = m_.typeNamed(v.type)) {
        if (ti->kind == TypeInfo::Pointer) return "(" + v.type + ") " + scalarAt(*ti, addr);
        return formatAt(*ti, addr, 0);
    }
    std::string t = v.type;
    for (const char *q : { "const ", "volatile " })
        for (size_t at; (at = t.find(q)) != std::string::npos;) t.erase(at, std::strlen(q));
    t = trim(t);
    char buf[64];
    if (!t.empty() && (t.back() == '*' || t.find("(*") != std::string::npos)) {
        std::snprintf(buf, sizeof buf, "(%s) 0x%x", v.type.c_str(), read32(addr));
        return buf;
    }
    if (t.empty() || t.find('[') != std::string::npos || t.compare(0, 7, "struct ") == 0 || t.compare(0, 6, "class ") == 0 ||
        t.compare(0, 6, "union ") == 0)
        return "{...}";
    uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (t == "double" || t == "long double" || t == "real") {
        if (!m_.read(addr, b, 8)) return "<unreadable>";
        double d; std::memcpy(&d, b, 8);
        std::snprintf(buf, sizeof buf, "%g", d);
        return buf;
    }
    if (t == "float") {
        if (!m_.read(addr, b, 4)) return "<unreadable>";
        float f; std::memcpy(&f, b, 4);
        std::snprintf(buf, sizeof buf, "%g", f);
        return buf;
    }
    bool isUnsigned = t.compare(0, 9, "unsigned ") == 0 || t == "unsigned" || t == "bool" || t == "_Bool";
    size_t size = 4;
    if (t.find("char") != std::string::npos || t == "bool" || t == "_Bool") size = 1;
    else if (t.find("short") != std::string::npos) size = 2;
    else if (t.find("long long") != std::string::npos) size = 8;
    if (!m_.read(addr, b, size)) return "<unreadable>";
    uint64_t u = 0;
    for (size_t k = size; k-- > 0;) u = (u << 8) | b[k];
    if (t == "bool" || t == "_Bool") return u ? "true" : "false";
    if (isUnsigned) { std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(u)); return buf; }
    long long s = size == 8 ? static_cast<long long>(u) : static_cast<long long>(u << (64 - 8 * size)) >> (64 - 8 * size);
    std::snprintf(buf, sizeof buf, "%lld", s);
    return buf;
}

// info args / info locals: "name = value" a line, as gdb answers. Only the blocks the stop is inside: a name an
// inner block reuses is listed twice there, each with its own value, as gdb lists them; the same variable named
// twice (two loops' i in one slot) once.
void Session::cmdInfoVariables(bool params) {
    if (!alive_ || !started_) { line("No frame selected."); return; }
    const std::vector<Variable> *vs = frameVariables();
    if (vs == nullptr) { line("No symbol table info available."); return; }
    std::vector<const Variable *> shown;
    for (const Variable &v : *vs) {
        if (v.param != params || !inScope(v)) continue;
        bool again = false;
        for (const Variable *s : shown)
            if (s->name == v.name && s->offset == v.offset && s->global == v.global && s->symbol == v.symbol) again = true;
        if (again) continue;
        shown.push_back(&v);
        line(v.name + " = " + valueOf(v));
    }
    if (shown.empty()) line(params ? "No arguments." : "No locals.");
}

// x/NNxb ADDR - NN bytes from ADDR (a number, a register, &variable or a variable's name for its address),
// eight to a line as gdb prints them: "0xc0013b6c:\t0x00\t0x00 ..."
void Session::cmdExamine(const std::string &verb, const std::string &arg) {
    if (!alive_ || !started_) { line("The program has no registers now."); return; }
    std::string fmt = verb.size() > 2 ? verb.substr(2) : "1xb";
    long count = std::strtol(fmt.c_str(), nullptr, 10);
    if (count <= 0) count = 1;
    if (count > 4096) count = 4096;
    std::string e = trim(arg);
    if (!e.empty() && e[0] == '&') e = trim(e.substr(1));
    if (e.size() > 1 && e[0] == '(' && e[e.size() - 1] == ')') e = trim(e.substr(1, e.size() - 2));
    uint32_t addr = 0; bool isReg = false; int reg = -1;
    std::string ltype;
    if (lvalueOf(e, ltype, addr)) { }   // a variable, a member or an element: from where it lives
    else if (!evaluate(e, addr, isReg, reg)) { line("No symbol \"" + e + "\" in current context."); return; }
    std::vector<uint8_t> b(static_cast<size_t>(count));
    if (!m_.read(addr, b.data(), b.size())) {
        char buf[64]; std::snprintf(buf, sizeof buf, "Cannot access memory at address 0x%x", addr); line(buf); return;
    }
    for (long k = 0; k < count; k += 8) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%08x:", addr + static_cast<uint32_t>(k));
        std::string out = buf;
        for (long j = k; j < k + 8 && j < count; j++) { std::snprintf(buf, sizeof buf, "\t0x%02x", b[static_cast<size_t>(j)]); out += buf; }
        line(out);
    }
}

void Session::cmdRegisters() {
    for (int r = 0; r < 64; r++) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%-15s0x%-18x%d", registerName(r).c_str(), m_.reg(r), static_cast<int>(m_.reg(r)));
        line(buf);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%-15s0x%-18x0x%x", "pc", m_.pc(), m_.pc());
    line(buf);
}

int Session::run() {
    std::string input;
    while (std::getline(std::cin, input)) {
        std::string c = trim(input);
        if (c.empty()) { std::fflush(stdout); continue; }
        size_t sp = c.find_first_of(" \t");
        std::string verb = sp == std::string::npos ? c : c.substr(0, sp);
        std::string arg = sp == std::string::npos ? std::string() : trim(c.substr(sp + 1));
        if (verb == "quit" || verb == "q") break;
        else if (verb == "echo") {
            std::string out;
            for (size_t i = 0; i < arg.size(); i++) {
                if (arg[i] == '\\' && i + 1 < arg.size()) { i++; out += arg[i] == 'n' ? '\n' : arg[i] == 't' ? '\t' : arg[i]; }
                else out += arg[i];
            }
            say(out);
        }
        else if (verb == "set") cmdSet(arg);
        else if (verb == "break" || verb == "b" || verb == "tbreak") cmdBreak(arg);
        else if (verb == "delete" || verb == "d") cmdDelete(arg);
        else if (verb == "run" || verb == "r") {
            if (!alive_) line("The program is not being run.");
            else {
                line("Starting program: " + m_.programName());
                started_ = true;
                // The entry is a function's first packet: its frame is pushed by the first track().
                track(m_.pc());
                if (const Breakpoint *b = breakAt(m_.pc())) stopped(("\nBreakpoint " + std::to_string(b->id) + ", ").c_str(), true);
                else go(Continue);
            }
        }
        else if (verb == "continue" || verb == "c") {
            if (!alive_ || !started_) line("The program is not being run.");
            else { line("Continuing."); go(Continue); }
        }
        else if (verb == "next" || verb == "n") { if (!alive_ || !started_) line("The program is not being run."); else go(Next); }
        else if (verb == "step" || verb == "s") { if (!alive_ || !started_) line("The program is not being run."); else go(Step); }
        else if (verb == "finish" || verb == "fin") {
            if (!alive_ || !started_) line("The program is not being run.");
            else if (frames_.size() <= 1) line("\"finish\" not meaningful in the outermost frame.");
            else { line("Run till exit from #0  " + place(m_.pc(), false)); go(Finish); }
        }
        else if (verb == "backtrace" || verb == "bt" || verb == "where") cmdBacktrace();
        else if (verb == "frame" || verb == "f") cmdFrame(arg);
        else if (verb == "print" || verb == "p") cmdPrint(arg, false);
        else if (verb == "whatis") cmdPrint(arg, true);
        else if (verb.compare(0, 2, "x/") == 0 || verb == "x") cmdExamine(verb, arg);
        else if (verb == "info") {
            if (arg == "args" || arg == "locals") cmdInfoVariables(arg == "args");
            else if (arg == "registers" || arg == "reg") cmdRegisters();
            else if (arg == "breakpoints" || arg == "break" || arg == "b") cmdInfoBreak();
            else if (arg == "line" || arg == "source") line(place(m_.pc(), false));
            else line("Undefined info command: \"" + arg + "\".  Try \"help info\".");
        }
        else line("Undefined command: \"" + verb + "\".  Try \"help\".");
        std::fflush(stdout);
    }
    std::fflush(stdout);
    return alive_ ? 0 : (m_.status() & 0xff);
}

}  // namespace

int shell(Machine &m) { return Session(m).run(); }

namespace {

// The text of an attribute - DW_AT_name("n") gives n, DW_AT_low_pc(greet) greet - or empty.
std::string attribute(const std::string &line, const char *key) {
    size_t at = line.find(key);
    if (at == std::string::npos) return std::string();
    at += std::strlen(key);
    if (at < line.size() && line[at] == '"') {
        size_t end = line.find('"', at + 1);
        return end == std::string::npos ? std::string() : line.substr(at + 1, end - at - 1);
    }
    size_t end = line.find(')', at);
    return end == std::string::npos ? std::string() : trim(line.substr(at, end - at));
}

}  // namespace

// The lines the compilers write, as the assembler kept them - spaced as written, or rebuilt from asm6x's
// tokens with no space after a comma:
//   .dwtag DW_TAG_subprogram, DW_AT_name("greet"), DW_AT_low_pc(greet)
//   .dwtag DW_TAG_formal_parameter, DW_AT_name("n"), DW_AT_type("int"), DW_AT_location[DW_OP_breg15 -44]
//   .dwtag DW_TAG_variable, ..., DW_AT_location[DW_OP_addr L.s.1]
//   .dwendtag
// DW_OP_breg15 is A15 (TI numbers A0-A15 0-15); a location in another register is passed over.
void readTags(const std::string &text, std::map<std::string, std::vector<Variable> > &byLabel) {
    std::map<std::string, TypeInfo> types;
    readTags(text, byLabel, types);
}

// The types (-g), after the functions, each closed by .dwendtag:
//   .dwtag DW_TAG_base_type, DW_AT_name("int"), DW_AT_byte_size(4), DW_AT_encoding(DW_ATE_signed)
//   .dwtag DW_TAG_pointer_type, DW_AT_name("char *"), DW_AT_type("char"), DW_AT_byte_size(4)
//   .dwtag DW_TAG_array_type, DW_AT_name("int [5]"), DW_AT_type("int"), DW_AT_upper_bound(4), DW_AT_byte_size(20)
//   .dwtag DW_TAG_structure_type, DW_AT_name("struct pt"), DW_AT_byte_size(8)
//   .dwtag DW_TAG_member, DW_AT_name("x"), DW_AT_type("int"), DW_AT_data_member_location(0)
//   .dwtag DW_TAG_array_type, DW_AT_name("real[][]"), DW_AT_type("real[]"), DW_AT_GRIDE_runtime_array, DW_AT_byte_size(4)
void readTags(const std::string &text, std::map<std::string, std::vector<Variable> > &byLabel,
              std::map<std::string, TypeInfo> &types) {
    std::string label;
    TypeInfo *open = nullptr;   // the struct or union whose members follow
    // the lexical blocks open inside the function, innermost last: a variable belongs to the innermost
    //   .dwtag DW_TAG_lexical_block, DW_AT_low_pc(L$sum$blk$b2), DW_AT_high_pc(L$sum$blk$e2)
    std::vector<std::pair<std::string, std::string> > blocks;
    size_t from = 0;
    while (from < text.size()) {
        size_t nl = text.find('\n', from);
        std::string line = trim(text.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        from = nl == std::string::npos ? text.size() : nl + 1;
        if (line.compare(0, 9, ".dwendtag") == 0) {
            if (!label.empty() && !blocks.empty()) { blocks.pop_back(); continue; }   // a block's end, not the function's
            label.clear(); open = nullptr; continue;
        }
        if (line.compare(0, 6, ".dwtag") != 0) continue;
        if (line.find("DW_TAG_lexical_block") != std::string::npos) {
            if (!label.empty()) blocks.push_back(std::make_pair(attribute(line, "DW_AT_low_pc("), attribute(line, "DW_AT_high_pc(")));
            continue;
        }
        const bool base = line.find("DW_TAG_base_type") != std::string::npos, ptr = line.find("DW_TAG_pointer_type") != std::string::npos,
                   arr = line.find("DW_TAG_array_type") != std::string::npos, str = line.find("DW_TAG_structure_type") != std::string::npos ||
                   line.find("DW_TAG_class_type") != std::string::npos, uni = line.find("DW_TAG_union_type") != std::string::npos;
        if (base || ptr || arr || str || uni) {
            TypeInfo t;
            t.name = attribute(line, "DW_AT_name(");
            t.element = attribute(line, "DW_AT_type(");
            t.encoding = attribute(line, "DW_AT_encoding(");
            t.size = std::atoi(attribute(line, "DW_AT_byte_size(").c_str());
            t.kind = base ? TypeInfo::Base : ptr ? TypeInfo::Pointer : str ? TypeInfo::Struct : uni ? TypeInfo::Union
                   : line.find("DW_AT_GRIDE_runtime_array") != std::string::npos ? TypeInfo::RuntimeArray : TypeInfo::Array;
            if (t.kind == TypeInfo::Array) t.count = std::atol(attribute(line, "DW_AT_upper_bound(").c_str()) + 1;
            if (t.name.empty()) continue;
            open = &(types[t.name] = t);
            if (open->kind != TypeInfo::Struct && open->kind != TypeInfo::Union) open = nullptr;
            continue;
        }
        if (line.find("DW_TAG_member") != std::string::npos) {
            if (open == nullptr) continue;
            TypeInfo::Member m;
            m.name = attribute(line, "DW_AT_name(");
            m.type = attribute(line, "DW_AT_type(");
            m.offset = std::atoi(attribute(line, "DW_AT_data_member_location(").c_str());
            if (!m.name.empty()) open->members.push_back(m);
            continue;
        }
        if (line.find("DW_TAG_subprogram") != std::string::npos) {
            label = attribute(line, "DW_AT_low_pc(");
            blocks.clear();
            if (!label.empty()) byLabel[label];
            continue;
        }
        bool param = line.find("DW_TAG_formal_parameter") != std::string::npos;
        if (label.empty() || (!param && line.find("DW_TAG_variable") == std::string::npos)) continue;
        Variable v;
        v.name = attribute(line, "DW_AT_name(");
        v.type = attribute(line, "DW_AT_type(");
        v.param = param;
        size_t loc = line.find("DW_AT_location[");
        if (v.name.empty() || loc == std::string::npos) continue;
        size_t end = line.find(']', loc);
        std::string where = line.substr(loc + 15, end == std::string::npos ? std::string::npos : end - loc - 15);
        v.deref = where.find("DW_OP_deref") != std::string::npos;
        size_t breg = where.find("DW_OP_breg"), addr = where.find("DW_OP_addr");
        if (breg != std::string::npos) {
            const char *p = where.c_str() + breg + 10;
            char *after = nullptr;
            long reg = std::strtol(p, &after, 10);
            if (reg != 15 || after == p) continue;
            v.offset = static_cast<int>(std::strtol(after, nullptr, 10));
        } else if (addr != std::string::npos) {
            std::string s = trim(where.substr(addr + 10));
            size_t stop = s.find_first_of(", \t");
            v.symbol = stop == std::string::npos ? s : s.substr(0, stop);
            v.global = !v.symbol.empty();
            if (!v.global) continue;
        } else continue;
        if (!blocks.empty()) { v.blockLow = blocks.back().first; v.blockHigh = blocks.back().second; }
        byLabel[label].push_back(v);
    }
}

}  // namespace dbg
