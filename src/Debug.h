#pragma once

// --debug: a debugger session over stdin and stdout, in gdb's words - the subset RIDE's Debug session
// speaks to gdb (break FILE:LINE, run, continue, next, step, finish, backtrace, frame, print, delete,
// echo, quit) answered in gdb's shapes, so that an editor built to read gdb reads this unchanged. One
// shell, written once, over the Machine below: vm6747 runs the assembly text under it, sim6747 the
// linked image (its src/Debug.* is this file, copied).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dbg {

// A function's parameter or local, as the compilers name it with .dwtag (-g): the C spelling of its
// type, and where it is - offset bytes from the frame's A15, or a symbol's address; deref when that
// place holds the variable's address (a Shalimar by-reference parameter).
struct Variable {
    std::string name, type, symbol;
    int offset = 0;
    bool param = false, deref = false, global = false;
    // a variable of an inner block (DW_TAG_lexical_block): the block's begin and end labels, and once the
    // machine has placed them, the code [lo, hi) it is listed for; unscoped, the whole function
    std::string blockLow, blockHigh;
    uint32_t lo = 0, hi = 0;
    bool scoped = false;
};

// Each inner-block variable's labels made addresses by the machine's own lookup; one that does not
// resolve stays unscoped, listed for the whole function as before.
template <class Lookup> void placeBlocks(std::vector<Variable> &vs, Lookup addressOf) {
    for (Variable &v : vs) {
        if (v.blockLow.empty() || v.blockHigh.empty()) continue;
        uint32_t lo = 0, hi = 0;
        if (addressOf(v.blockLow, lo) && addressOf(v.blockHigh, hi) && lo < hi) { v.lo = lo; v.hi = hi; v.scoped = true; }
    }
}

// A type as the compilers describe it with .dwtag (-g), by the name the variables' DW_AT_type give it:
// a base type's size and encoding, a pointer, a C array (element and count), a struct or union with its
// members, or Shalimar's runtime array - a pointer to {count, element size, data}.
struct TypeInfo {
    enum Kind { Base, Pointer, Array, Struct, Union, RuntimeArray } kind = Base;
    std::string name, element, encoding;   // element: an array's or a pointer's; encoding: DW_ATE_signed, _float ...
    int size = 0;
    long count = 0;
    struct Member { std::string name, type; int offset = 0; };
    std::vector<Member> members;
};

// The .dwtag / .dwendtag lines, one to a line, read into each function's variables by its label
// (DW_AT_low_pc), and into the types by name. A line it does not understand is passed over.
void readTags(const std::string &text, std::map<std::string, std::vector<Variable> > &byLabel);
void readTags(const std::string &text, std::map<std::string, std::vector<Variable> > &byLabel,
              std::map<std::string, TypeInfo> &types);

// Where an instruction came from: a file as the compiler named it, a line, or no line at all (0).
struct Position {
    std::string file;
    int line = 0;
    bool known() const { return line > 0; }
    bool operator==(const Position &o) const { return line == o.line && file == o.file; }
    bool operator!=(const Position &o) const { return !(*this == o); }
};

// What the shell asks of a target. Registers are numbered 0-31 A0-A31, 32-63 B0-B31; a value read is
// the one a halted debugger shows, every result in flight landed, and reading moves nothing.
class Machine {
public:
    virtual ~Machine() {}
    virtual std::string programName() const = 0;
    virtual uint32_t pc() const = 0;
    virtual void step() = 0;                  // one execute packet, or one native library call
    virtual bool ended() const = 0;           // the program has exited
    virtual int status() const = 0;           // its exit status, once ended
    virtual void finish() = 0;                // once ended: what runs after main (atexit handlers)
    virtual bool hasLines() const = 0;        // -g: any instruction with a source position
    virtual std::string noLinesWhy() const = 0;
    virtual bool positionAt(uint32_t pc, Position &p) const = 0;
    // A line-table row begins at pc (a .dwpsn put it there) - a statement starts, even on the line before it.
    virtual bool startsRow(uint32_t pc) const { (void)pc; return false; }
    // The first address of LINE in the file named by its leaf, or of the nearest line after it that has
    // code; lineUsed says which.
    virtual bool addressOfLine(const std::string &leaf, int line, uint32_t &addr, int &lineUsed) const = 0;
    virtual bool functionAt(uint32_t pc, std::string &name, uint32_t &entry) const = 0;
    virtual bool addressOfFunction(const std::string &name, uint32_t &addr) const = 0;
    virtual bool isFunctionEntry(uint32_t pc) const = 0;
    virtual uint32_t reg(int r) const = 0;
    virtual void setReg(int r, uint32_t v) = 0;
    // The text of a source line, for the echo gdb prints after a stop - or empty.
    virtual std::string sourceLine(const std::string &file, int line) = 0;
    // -g: the variables of the function entered at entry, or null when the program names none.
    virtual const std::vector<Variable> *variablesAt(uint32_t entry) const { (void)entry; return nullptr; }
    // -g: a type the compilers described, by name, or null.
    virtual const TypeInfo *typeNamed(const std::string &name) const { (void)name; return nullptr; }
    // Memory as the program sees it; false where there is none.
    virtual bool read(uint32_t addr, uint8_t *buf, size_t n) const { (void)addr; (void)buf; (void)n; return false; }
    // And written, for set variable; false where there is none.
    virtual bool write(uint32_t addr, const uint8_t *buf, size_t n) { (void)addr; (void)buf; (void)n; return false; }
};

// Run the session: commands from stdin until quit or end of input. The exit status to return.
int shell(Machine &m);

}  // namespace dbg
